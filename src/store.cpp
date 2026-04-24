#include "memix/store.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <ios>
#include <iterator>
#include <limits>
#include <memory>
#include <optional>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <tuple>
#include <utility>
#include <vector>

#include <openssl/evp.h>
#include <simdjson.h>

namespace fs = std::filesystem;

namespace {

constexpr std::string_view kHashAlgorithm = "sha256";
constexpr std::string_view kContentPath = "object";
constexpr std::string_view kMetadataFilename = "metadata.json";
constexpr std::size_t kSha256DigestBytes = 32;
constexpr std::size_t kSha256HexDigestLength = kSha256DigestBytes * 2;
constexpr std::size_t kHashReadBufferBytes = 8192;
constexpr std::size_t kTimestampBufferBytes = 21;
constexpr int kTempDirAttempts = 32;

template <typename Value>
using StoreResult = std::expected<Value, memix::StoreError>;

using StoreVoid = std::expected<void, memix::StoreError>;

std::string type_to_owned_string(memix::MemoryObjectType type) {
  return std::string(memix::to_string(type));
}

std::optional<std::string> env_value(const char *name) {
  // Root resolution reads process environment at an explicit API boundary.
  // NOLINTNEXTLINE(concurrency-mt-unsafe)
  const char *value = std::getenv(name);
  if (value == nullptr || std::string_view(value).empty()) {
    return std::nullopt;
  }
  return std::string(value);
}

std::unexpected<memix::StoreError> store_error(std::string message) {
  return std::unexpected(memix::StoreError{.message = std::move(message)});
}

std::string filesystem_error_message(std::string_view operation,
                                     const fs::path &path,
                                     const std::error_code &error) {
  return std::format("{} `{}`: {}", operation, path.string(), error.message());
}

StoreResult<bool> path_exists(const fs::path &path) {
  std::error_code error;
  const bool exists = fs::exists(path, error);
  if (error) {
    return store_error(
        filesystem_error_message("failed to inspect", path, error));
  }
  return exists;
}

StoreResult<bool> path_is_directory(const fs::path &path) {
  std::error_code error;
  const bool is_directory = fs::is_directory(path, error);
  if (error) {
    return store_error(
        filesystem_error_message("failed to inspect directory", path, error));
  }
  return is_directory;
}

StoreResult<bool> path_is_regular_file(const fs::path &path) {
  std::error_code error;
  const bool is_regular_file = fs::is_regular_file(path, error);
  if (error) {
    return store_error(
        filesystem_error_message("failed to inspect file", path, error));
  }
  return is_regular_file;
}

StoreResult<std::uintmax_t> path_file_size(const fs::path &path) {
  std::error_code error;
  const std::uintmax_t size = fs::file_size(path, error);
  if (error) {
    return store_error(
        filesystem_error_message("failed to read file size for", path, error));
  }
  return size;
}

std::string hex_encode(std::span<const unsigned char> bytes) {
  std::string output;
  output.reserve(bytes.size() * 2);
  for (const unsigned char byte : bytes) {
    output += std::format("{:02x}", static_cast<unsigned int>(byte));
  }
  return output;
}

StoreResult<std::string> sha256_file(const fs::path &path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    return store_error(
        std::format("failed to open `{}` for hashing", path.string()));
  }

  using DigestContext = std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)>;
  const DigestContext digest_context(EVP_MD_CTX_new(), EVP_MD_CTX_free);
  if (!digest_context) {
    return store_error("failed to allocate sha256 context");
  }
  if (EVP_DigestInit_ex(digest_context.get(), EVP_sha256(), nullptr) != 1) {
    return store_error("failed to initialize sha256 context");
  }

  std::array<char, kHashReadBufferBytes> buffer{};
  while (input) {
    input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
    const std::streamsize read_count = input.gcount();
    if (read_count
        > 0
        && EVP_DigestUpdate(digest_context.get(), buffer.data(),
                            static_cast<std::size_t>(read_count))
        != 1) {
      return store_error("failed to update sha256 digest");
    }
  }

  if (input.bad()) {
    return store_error(std::format("failed while reading `{}`", path.string()));
  }

  std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
  unsigned int digest_length = 0;
  if (EVP_DigestFinal_ex(digest_context.get(), digest.data(), &digest_length)
      != 1) {
    return store_error("failed to finalize sha256 digest");
  }
  if (digest_length != kSha256DigestBytes) {
    return store_error("sha256 digest produced an unexpected length");
  }

  return hex_encode(
      std::span<const unsigned char>{digest.data(), digest_length});
}

std::string simdjson_error_text(simdjson::error_code error) {
  return simdjson::error_message(error);
}

StoreResult<std::string>
json_from_builder(const simdjson::builder::string_builder &builder) {
  if (!builder.validate_unicode()) {
    return store_error("metadata JSON contains invalid UTF-8");
  }

  std::string_view output;
  const simdjson::error_code error = builder.view().get(output);
  if (error != simdjson::SUCCESS) {
    return store_error(std::format("failed to build metadata JSON: {}",
                                   simdjson_error_text(error)));
  }
  return std::string(output);
}

StoreResult<std::string> metadata_json_for(const memix::StoredObject &object) {
  simdjson::builder::string_builder builder;
  builder.start_object();
  builder.append_key_value("schema_version", object.schema_version);
  builder.append_comma();
  builder.append_key_value("object_version", object.object_version);
  builder.append_comma();
  builder.append_key_value("type", memix::to_string(object.type));
  builder.append_comma();
  builder.append_key_value("id", object.id);
  builder.append_comma();
  builder.append_key_value("hash_algorithm", object.hash_algorithm);
  builder.append_comma();
  builder.append_key_value("hash", object.hash);
  builder.append_comma();
  builder.append_key_value("size_bytes", object.size_bytes);
  builder.append_comma();
  builder.append_key_value("filename", object.filename);
  builder.append_comma();
  builder.append_key_value("created_at", object.created_at);
  builder.append_comma();
  builder.append_key_value("content_path",
                           object.content_path.generic_string());
  builder.end_object();
  return json_from_builder(builder);
}

StoreResult<std::string> read_text_file(const fs::path &path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    return store_error(
        std::format("failed to open metadata `{}`", path.string()));
  }
  std::string output{std::istreambuf_iterator<char>(input),
                     std::istreambuf_iterator<char>()};
  if (input.bad()) {
    return store_error(
        std::format("failed to read metadata `{}`", path.string()));
  }
  return output;
}

StoreVoid write_text_file(const fs::path &path, std::string_view contents) {
  std::ofstream output(path, std::ios::binary);
  if (!output) {
    return store_error(
        std::format("failed to open `{}` for writing", path.string()));
  }
  output << contents;
  if (!output) {
    return store_error(std::format("failed to write `{}`", path.string()));
  }
  return {};
}

StoreResult<simdjson::dom::element> required_field(simdjson::dom::object fields,
                                                   std::string_view key) {
  simdjson::dom::element value;
  const simdjson::error_code error = fields.at_key(key).get(value);
  if (error != simdjson::SUCCESS) {
    return store_error(
        std::format("metadata is missing or has invalid field `{}`: {}", key,
                    simdjson_error_text(error)));
  }
  return value;
}

StoreResult<std::string> required_string(simdjson::dom::object fields,
                                         std::string_view key) {
  const StoreResult<simdjson::dom::element> value = required_field(fields, key);
  if (!value) {
    return std::unexpected(value.error());
  }
  std::string_view parsed;
  const simdjson::error_code error = value->get(parsed);
  if (error != simdjson::SUCCESS) {
    return store_error(std::format("metadata field `{}` must be a string: {}",
                                   key, simdjson_error_text(error)));
  }
  return std::string(parsed);
}

StoreResult<std::uintmax_t> required_number(simdjson::dom::object fields,
                                            std::string_view key) {
  const StoreResult<simdjson::dom::element> value = required_field(fields, key);
  if (!value) {
    return std::unexpected(value.error());
  }
  std::uint64_t parsed = 0;
  const simdjson::error_code error = value->get(parsed);
  if (error != simdjson::SUCCESS) {
    return store_error(
        std::format("metadata field `{}` must be an unsigned integer: {}", key,
                    simdjson_error_text(error)));
  }
  return static_cast<std::uintmax_t>(parsed);
}

StoreResult<int> checked_int(std::uintmax_t value,
                             const std::string &field_name) {
  if (value > static_cast<std::uintmax_t>(std::numeric_limits<int>::max())) {
    return store_error(
        std::format("metadata field `{}` is too large", field_name));
  }
  return static_cast<int>(value);
}

bool is_lower_hex_sha256(std::string_view value) noexcept {
  if (value.size() != kSha256HexDigestLength) {
    return false;
  }
  return std::ranges::all_of(value, [](char character) {
    return (character >= '0' && character <= '9')
        || (character >= 'a' && character <= 'f');
  });
}

fs::path type_dir_for(const fs::path &root, memix::MemoryObjectType type) {
  return root / std::string(memix::to_string(type));
}

StoreVoid ensure_directory(const fs::path &path, fs::perms permissions) {
  std::error_code create_error;
  fs::create_directories(path, create_error);
  if (create_error) {
    return store_error(filesystem_error_message("failed to create directory",
                                                path, create_error));
  }

  std::error_code permissions_error;
  fs::permissions(path, permissions, fs::perm_options::replace,
                  permissions_error);
  return {};
}

void apply_read_only_file_permissions(const fs::path &path) {
  std::error_code permissions_error;
  fs::permissions(path, fs::perms::owner_read, fs::perm_options::replace,
                  permissions_error);
}

void apply_read_only_directory_permissions(const fs::path &path) {
  std::error_code permissions_error;
  fs::permissions(path, fs::perms::owner_read | fs::perms::owner_exec,
                  fs::perm_options::replace, permissions_error);
}

StoreResult<std::string> utc_timestamp() {
  const auto now = std::chrono::system_clock::now();
  const std::time_t time = std::chrono::system_clock::to_time_t(now);
  std::tm utc{};
#ifdef _WIN32
  if (gmtime_s(&utc, &time) != 0) {
    return store_error("failed to compute UTC timestamp");
  }
#else
  if (gmtime_r(&time, &utc) == nullptr) {
    return store_error("failed to compute UTC timestamp");
  }
#endif
  std::array<char, kTimestampBufferBytes> buffer{};
  if (std::strftime(buffer.data(), buffer.size(), "%Y-%m-%dT%H:%M:%SZ", &utc)
      == 0U) {
    return store_error("failed to format timestamp");
  }
  return {buffer.data()};
}

StoreResult<fs::path> unique_temp_dir(const fs::path &type_dir,
                                      std::string_view hash) {
  std::random_device random_device;
  for (int attempt = 0; attempt < kTempDirAttempts; ++attempt) {
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    const auto nanos =
        std::chrono::duration_cast<std::chrono::nanoseconds>(now).count();
    fs::path candidate =
        type_dir / std::format(".tmp-{}-{}-{}", hash, nanos, random_device());
    const StoreResult<bool> exists = path_exists(candidate);
    if (!exists) {
      return std::unexpected(exists.error());
    }
    if (!*exists) {
      return candidate;
    }
  }
  return store_error("failed to allocate a temporary object directory");
}

StoreResult<memix::StoredObject>
metadata_object_from_fields(simdjson::dom::object fields,
                            const fs::path &object_dir,
                            const fs::path &metadata_path);

StoreVoid validate_metadata_object(const memix::StoredObject &object,
                                   memix::MemoryObjectType dir_type);

StoreVoid validate_content_file(const memix::StoredObject &object);

StoreResult<memix::StoredObject>
object_from_metadata_path(const fs::path &object_dir,
                          memix::MemoryObjectType dir_type,
                          const fs::path &metadata_path) {
  const StoreResult<std::string> metadata_text = read_text_file(metadata_path);
  if (!metadata_text) {
    return std::unexpected(metadata_text.error());
  }
  const simdjson::padded_string metadata_json(*metadata_text);
  simdjson::dom::parser parser;
  simdjson::dom::element document;
  simdjson::error_code error = parser.parse(metadata_json).get(document);
  if (error != simdjson::SUCCESS) {
    return store_error(std::format("invalid metadata JSON `{}`: {}",
                                   metadata_path.string(),
                                   simdjson_error_text(error)));
  }

  simdjson::dom::object fields;
  error = document.get(fields);
  if (error != simdjson::SUCCESS) {
    return store_error(std::format("metadata JSON must be an object `{}`: {}",
                                   metadata_path.string(),
                                   simdjson_error_text(error)));
  }

  StoreResult<memix::StoredObject> object =
      metadata_object_from_fields(fields, object_dir, metadata_path);
  if (!object) {
    return std::unexpected(object.error());
  }
  const StoreVoid metadata_valid = validate_metadata_object(*object, dir_type);
  if (!metadata_valid) {
    return std::unexpected(metadata_valid.error());
  }
  const StoreVoid content_valid = validate_content_file(*object);
  if (!content_valid) {
    return std::unexpected(content_valid.error());
  }

  return object;
}

StoreResult<memix::StoredObject>
metadata_object_from_fields(simdjson::dom::object fields,
                            const fs::path &object_dir,
                            const fs::path &metadata_path) {
  memix::StoredObject object;
  const StoreResult<std::uintmax_t> schema_version =
      required_number(fields, "schema_version");
  if (!schema_version) {
    return std::unexpected(schema_version.error());
  }
  const StoreResult<int> checked_schema_version =
      checked_int(*schema_version, "schema_version");
  if (!checked_schema_version) {
    return std::unexpected(checked_schema_version.error());
  }
  object.schema_version = *checked_schema_version;

  const StoreResult<std::uintmax_t> object_version =
      required_number(fields, "object_version");
  if (!object_version) {
    return std::unexpected(object_version.error());
  }
  const StoreResult<int> checked_object_version =
      checked_int(*object_version, "object_version");
  if (!checked_object_version) {
    return std::unexpected(checked_object_version.error());
  }
  object.object_version = *checked_object_version;

  const StoreResult<std::string> type_text = required_string(fields, "type");
  if (!type_text) {
    return std::unexpected(type_text.error());
  }
  const auto parsed_type = memix::parse_memory_object_type(*type_text);
  if (!parsed_type.has_value()) {
    return store_error(
        std::format("metadata has unsupported type `{}`", *type_text));
  }
  object.type = *parsed_type;

  const StoreResult<std::string> object_id = required_string(fields, "id");
  if (!object_id) {
    return std::unexpected(object_id.error());
  }
  object.id = *object_id;

  const StoreResult<std::string> hash_algorithm =
      required_string(fields, "hash_algorithm");
  if (!hash_algorithm) {
    return std::unexpected(hash_algorithm.error());
  }
  object.hash_algorithm = *hash_algorithm;

  const StoreResult<std::string> hash = required_string(fields, "hash");
  if (!hash) {
    return std::unexpected(hash.error());
  }
  object.hash = *hash;

  const StoreResult<std::uintmax_t> size_bytes =
      required_number(fields, "size_bytes");
  if (!size_bytes) {
    return std::unexpected(size_bytes.error());
  }
  object.size_bytes = *size_bytes;

  const StoreResult<std::string> filename = required_string(fields, "filename");
  if (!filename) {
    return std::unexpected(filename.error());
  }
  object.filename = *filename;

  const StoreResult<std::string> created_at =
      required_string(fields, "created_at");
  if (!created_at) {
    return std::unexpected(created_at.error());
  }
  object.created_at = *created_at;

  const StoreResult<std::string> content_path =
      required_string(fields, "content_path");
  if (!content_path) {
    return std::unexpected(content_path.error());
  }
  object.content_path = fs::path(*content_path);
  object.object_dir = object_dir;
  object.metadata_path = metadata_path;
  return object;
}

StoreVoid validate_metadata_object(const memix::StoredObject &object,
                                   memix::MemoryObjectType dir_type) {
  if (object.schema_version != 1) {
    return store_error("metadata schema_version is not supported");
  }
  if (object.object_version != 1) {
    return store_error("metadata object_version is not supported");
  }
  if (object.type != dir_type) {
    return store_error("metadata type does not match its store directory");
  }
  if (object.hash_algorithm != kHashAlgorithm) {
    return store_error("metadata hash_algorithm is not supported");
  }
  if (object.id != object.hash) {
    return store_error("metadata id does not match hash");
  }
  if (!is_lower_hex_sha256(object.hash)) {
    return store_error("metadata hash is not a lowercase sha256 digest");
  }
  if (object.object_dir.filename().string() != object.hash) {
    return store_error("metadata hash does not match its object directory");
  }
  if (object.content_path != fs::path(kContentPath)) {
    return store_error("metadata content_path is not supported");
  }
  return {};
}

StoreVoid validate_content_file(const memix::StoredObject &object) {
  const fs::path content_file = object.object_dir / object.content_path;
  const StoreResult<bool> content_is_regular_file =
      path_is_regular_file(content_file);
  if (!content_is_regular_file) {
    return std::unexpected(content_is_regular_file.error());
  }
  if (!*content_is_regular_file) {
    return store_error(
        std::format("object content is missing for `{}`", object.id));
  }
  const StoreResult<std::uintmax_t> content_size = path_file_size(content_file);
  if (!content_size) {
    return std::unexpected(content_size.error());
  }
  if (*content_size != object.size_bytes) {
    return store_error(std::format(
        "object content size does not match metadata for `{}`", object.id));
  }

  return {};
}

StoreResult<memix::StoredObject>
read_existing_object(const fs::path &object_dir, memix::MemoryObjectType type) {
  const StoreResult<bool> object_dir_is_directory =
      path_is_directory(object_dir);
  if (!object_dir_is_directory) {
    return std::unexpected(object_dir_is_directory.error());
  }
  if (!*object_dir_is_directory) {
    return store_error(std::format(
        "object path exists but is not a directory `{}`", object_dir.string()));
  }
  return object_from_metadata_path(object_dir, type,
                                   object_dir / kMetadataFilename);
}

bool is_temp_store_entry(const fs::path &path) {
  const std::string name = path.filename().string();
  return name.starts_with(".tmp-");
}

StoreResult<std::vector<memix::StoredObject>>
read_objects_for_type(const fs::path &root, memix::MemoryObjectType type) {
  std::vector<memix::StoredObject> objects;
  const fs::path type_dir = type_dir_for(root, type);
  const StoreResult<bool> exists = path_exists(type_dir);
  if (!exists) {
    return std::unexpected(exists.error());
  }
  if (!*exists) {
    return objects;
  }
  const StoreResult<bool> is_directory = path_is_directory(type_dir);
  if (!is_directory) {
    return std::unexpected(is_directory.error());
  }
  if (!*is_directory) {
    return store_error(std::format("store type path is not a directory `{}`",
                                   type_dir.string()));
  }

  std::error_code iterator_error;
  const fs::directory_iterator end;
  for (fs::directory_iterator iterator{type_dir, iterator_error};
       !iterator_error && iterator != end; iterator.increment(iterator_error)) {
    const fs::directory_entry &entry = *iterator;
    if (is_temp_store_entry(entry.path())) {
      continue;
    }
    std::error_code entry_error;
    const bool entry_is_directory = entry.is_directory(entry_error);
    if (entry_error) {
      return store_error(filesystem_error_message("failed to inspect",
                                                  entry.path(), entry_error));
    }
    if (!entry_is_directory) {
      return store_error(std::format(
          "store object path is not a directory `{}`", entry.path().string()));
    }
    const std::string digest = entry.path().filename().string();
    if (!is_lower_hex_sha256(digest)) {
      return store_error(
          std::format("store object directory is not a sha256 digest `{}`",
                      entry.path().string()));
    }
    StoreResult<memix::StoredObject> object =
        read_existing_object(entry.path(), type);
    if (!object) {
      return std::unexpected(object.error());
    }
    objects.push_back(std::move(*object));
  }
  if (iterator_error) {
    return store_error(filesystem_error_message("failed to list directory",
                                                type_dir, iterator_error));
  }
  return objects;
}

std::vector<memix::MemoryObjectType>
types_for_list(std::optional<memix::MemoryObjectType> type) {
  if (type.has_value()) {
    return {*type};
  }
  return memix::memory_object_types();
}

StoreResult<std::optional<memix::StoredObject>>
read_existing_object_if_present(const fs::path &object_dir,
                                memix::MemoryObjectType type) {
  const StoreResult<bool> object_exists = path_exists(object_dir);
  if (!object_exists) {
    return std::unexpected(object_exists.error());
  }
  if (!*object_exists) {
    return StoreResult<std::optional<memix::StoredObject>>{std::in_place,
                                                           std::nullopt};
  }
  StoreResult<memix::StoredObject> object =
      read_existing_object(object_dir, type);
  if (!object) {
    return std::unexpected(object.error());
  }
  return StoreResult<std::optional<memix::StoredObject>>{std::in_place,
                                                         std::move(*object)};
}

StoreVoid ensure_store_directories(const fs::path &root,
                                   const fs::path &type_dir) {
  const StoreVoid root_created = ensure_directory(root, fs::perms::owner_all);
  if (!root_created) {
    return std::unexpected(root_created.error());
  }
  const StoreVoid type_dir_created =
      ensure_directory(type_dir, fs::perms::owner_all);
  if (!type_dir_created) {
    return std::unexpected(type_dir_created.error());
  }
  return {};
}

struct PublishRequest {
  memix::MemoryObjectType type;
  fs::path source_path;
  fs::path type_dir;
  fs::path object_dir;
  std::string hash;
  std::uintmax_t size_bytes;
};

class TemporaryDirectoryCleanup {
public:
  explicit TemporaryDirectoryCleanup(fs::path path) : path_(std::move(path)) {}

  TemporaryDirectoryCleanup(const TemporaryDirectoryCleanup &) = delete;
  TemporaryDirectoryCleanup &
  operator=(const TemporaryDirectoryCleanup &) = delete;

  TemporaryDirectoryCleanup(TemporaryDirectoryCleanup &&other) noexcept
      : path_(std::move(other.path_)), active_(other.active_) {
    other.release();
  }

  TemporaryDirectoryCleanup &
  operator=(TemporaryDirectoryCleanup &&other) noexcept = delete;

  ~TemporaryDirectoryCleanup() {
    if (!active_) {
      return;
    }
    std::error_code remove_error;
    fs::remove_all(path_, remove_error);
  }

  void release() noexcept { active_ = false; }

private:
  fs::path path_;
  bool active_ = true;
};

StoreResult<memix::IngestResult> publish_new_object(PublishRequest request) {
  const StoreResult<fs::path> temp_dir =
      unique_temp_dir(request.type_dir, request.hash);
  if (!temp_dir) {
    return std::unexpected(temp_dir.error());
  }

  std::error_code create_temp_error;
  const bool created_temp_dir =
      fs::create_directory(*temp_dir, create_temp_error);
  if (create_temp_error) {
    return store_error(
        filesystem_error_message("failed to create temporary object directory",
                                 *temp_dir, create_temp_error));
  }
  if (!created_temp_dir) {
    return store_error(
        std::format("failed to create temporary object directory `{}`",
                    temp_dir->string()));
  }
  TemporaryDirectoryCleanup temp_dir_cleanup(*temp_dir);

  const fs::path temp_object = *temp_dir / kContentPath;
  const fs::path temp_metadata = *temp_dir / kMetadataFilename;

  std::error_code copy_error;
  const bool copied = fs::copy_file(request.source_path, temp_object,
                                    fs::copy_options::none, copy_error);
  if (copy_error) {
    return store_error(filesystem_error_message(
        "failed to copy source", request.source_path, copy_error));
  }
  if (!copied) {
    return store_error(std::format("failed to copy source into store `{}`",
                                   request.source_path.string()));
  }

  const StoreResult<std::string> created_at = utc_timestamp();
  if (!created_at) {
    return std::unexpected(created_at.error());
  }

  memix::StoredObject object;
  object.type = request.type;
  object.id = request.hash;
  object.hash = std::move(request.hash);
  object.hash_algorithm = std::string(kHashAlgorithm);
  object.size_bytes = request.size_bytes;
  object.filename = request.source_path.filename().string();
  object.created_at = *created_at;
  object.content_path = fs::path(kContentPath);
  object.object_dir = request.object_dir;
  object.metadata_path = request.object_dir / kMetadataFilename;

  const StoreResult<std::string> metadata = memix::metadata_json(object);
  if (!metadata) {
    return std::unexpected(metadata.error());
  }
  const StoreVoid metadata_written = write_text_file(temp_metadata, *metadata);
  if (!metadata_written) {
    return std::unexpected(metadata_written.error());
  }
  apply_read_only_file_permissions(temp_object);
  apply_read_only_file_permissions(temp_metadata);

  std::error_code rename_error;
  fs::rename(*temp_dir, request.object_dir, rename_error);
  if (rename_error) {
    StoreResult<std::optional<memix::StoredObject>> existing =
        read_existing_object_if_present(request.object_dir, request.type);
    if (!existing) {
      return std::unexpected(existing.error());
    }
    if (existing->has_value()) {
      return StoreResult<memix::IngestResult>{std::in_place,
                                              std::move(**existing), false};
    }
    return store_error(std::format("failed to publish object `{}`: {}",
                                   request.object_dir.string(),
                                   rename_error.message()));
  }
  temp_dir_cleanup.release();
  apply_read_only_directory_permissions(request.object_dir);
  return StoreResult<memix::IngestResult>{std::in_place, std::move(object),
                                          true};
}

} // namespace

namespace memix {

std::string_view to_string(MemoryObjectType type) noexcept {
  switch (type) {
  case MemoryObjectType::Agents:
    return "agents";
  case MemoryObjectType::Chat:
    return "chat";
  case MemoryObjectType::Documents:
    return "documents";
  }
  return "agents";
}

std::optional<MemoryObjectType>
parse_memory_object_type(std::string_view value) noexcept {
  if (value == "agents")
    return MemoryObjectType::Agents;
  if (value == "chat")
    return MemoryObjectType::Chat;
  if (value == "documents")
    return MemoryObjectType::Documents;

  return std::nullopt;
}

std::vector<MemoryObjectType> memory_object_types() {
  return {
      MemoryObjectType::Agents,
      MemoryObjectType::Chat,
      MemoryObjectType::Documents,
  };
}

std::expected<fs::path, StoreError> default_store_root() {
  if (const auto memix_store_home = env_value("MEMIX_STORE_HOME");
      memix_store_home.has_value()) {
    return fs::path(*memix_store_home);
  }
  if (const auto xdg_data_home = env_value("XDG_DATA_HOME");
      xdg_data_home.has_value()) {
    return fs::path(*xdg_data_home) / "memix";
  }
  if (const auto home = env_value("HOME"); home.has_value()) {
    return fs::path(*home) / ".memix";
  }
  return store_error(
      "cannot determine default root; set MEMIX_STORE_HOME or HOME");
}

std::expected<std::string, StoreError>
metadata_json(const StoredObject &object) {
  return metadata_json_for(object);
}

Store::Store(fs::path root) : root_(std::move(root)) {}

const fs::path &Store::root() const noexcept { return root_; }

std::expected<IngestResult, StoreError>
Store::ingest(MemoryObjectType type, const fs::path &source_path) const {
  const StoreResult<bool> source_is_regular_file =
      path_is_regular_file(source_path);
  if (!source_is_regular_file) {
    return std::unexpected(source_is_regular_file.error());
  }
  if (!*source_is_regular_file) {
    return store_error(std::format("ingest source must be a regular file `{}`",
                                   source_path.string()));
  }

  const StoreResult<std::uintmax_t> size_bytes = path_file_size(source_path);
  if (!size_bytes) {
    return std::unexpected(size_bytes.error());
  }
  const StoreResult<std::string> hash = sha256_file(source_path);
  if (!hash) {
    return std::unexpected(hash.error());
  }
  const fs::path type_dir = type_dir_for(root_, type);
  const fs::path object_dir = type_dir / *hash;

  const StoreVoid directories_created =
      ensure_store_directories(root_, type_dir);
  if (!directories_created) {
    return std::unexpected(directories_created.error());
  }

  StoreResult<std::optional<StoredObject>> existing =
      read_existing_object_if_present(object_dir, type);
  if (!existing) {
    return std::unexpected(existing.error());
  }
  if (existing->has_value()) {
    return StoreResult<IngestResult>{std::in_place, std::move(**existing),
                                     false};
  }

  return publish_new_object(PublishRequest{
      .type = type,
      .source_path = source_path,
      .type_dir = type_dir,
      .object_dir = object_dir,
      .hash = *hash,
      .size_bytes = *size_bytes,
  });
}

std::expected<std::vector<StoredObject>, StoreError>
Store::list(std::optional<MemoryObjectType> type) const {
  std::vector<StoredObject> objects;
  const StoreResult<bool> root_exists = path_exists(root_);
  if (!root_exists) {
    return std::unexpected(root_exists.error());
  }
  if (!*root_exists) {
    return objects;
  }
  const StoreResult<bool> root_is_directory = path_is_directory(root_);
  if (!root_is_directory) {
    return std::unexpected(root_is_directory.error());
  }
  if (!*root_is_directory) {
    return store_error(
        std::format("store root is not a directory `{}`", root_.string()));
  }

  const std::vector<MemoryObjectType> types = types_for_list(type);

  for (const MemoryObjectType current_type : types) {
    StoreResult<std::vector<StoredObject>> type_objects =
        read_objects_for_type(root_, current_type);
    if (!type_objects) {
      return std::unexpected(type_objects.error());
    }
    for (StoredObject &object : *type_objects) {
      objects.push_back(std::move(object));
    }
  }

  std::ranges::sort(
      objects, [](const StoredObject &lhs, const StoredObject &rhs) {
        const auto lhs_key = std::tuple{type_to_owned_string(lhs.type),
                                        lhs.created_at, lhs.id, lhs.filename};
        const auto rhs_key = std::tuple{type_to_owned_string(rhs.type),
                                        rhs.created_at, rhs.id, rhs.filename};
        return lhs_key < rhs_key;
      });
  return objects;
}

} // namespace memix
