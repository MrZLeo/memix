#include "memix/store.hpp"

#include "common.hpp"

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
#include <functional>
#include <ios>
#include <iterator>
#include <limits>
#include <memory>
#include <optional>
#include <ranges>
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
constexpr std::string_view kAgentBlobsDir = "blobs";
constexpr std::string_view kAgentTreesDir = "trees";
constexpr std::string_view kAgentGenerationsDir = "generations";
constexpr std::string_view kRefsDir = "refs";
constexpr std::string_view kLocksDir = ".locks";
constexpr std::string_view kAgentUpdateLock = "agents-update";
constexpr std::string_view kAgentsLatestRef = "agents/latest";
constexpr std::string_view kJsonSuffix = ".json";
constexpr std::size_t kSha256DigestBytes = 32;
constexpr std::size_t kHashReadBufferBytes = 8192;
constexpr std::size_t kTimestampBufferBytes = 21;
constexpr std::uint64_t kDecimalRadix = 10;

struct MemoryObjectTypeToken {
  memix::MemoryObjectType value;
  std::string_view token;
};

struct AgentTreeEntryKindToken {
  memix::AgentTreeEntryKind value;
  std::string_view token;
};

constexpr std::array kMemoryObjectTypeTokens{
    MemoryObjectTypeToken{
        .value = memix::MemoryObjectType::Agents,
        .token = "agents",
    },
    MemoryObjectTypeToken{
        .value = memix::MemoryObjectType::Chat,
        .token = "chat",
    },
    MemoryObjectTypeToken{
        .value = memix::MemoryObjectType::Documents,
        .token = "documents",
    },
};

constexpr std::array kAgentTreeEntryKindTokens{
    AgentTreeEntryKindToken{
        .value = memix::AgentTreeEntryKind::Blob,
        .token = "blob",
    },
    AgentTreeEntryKindToken{
        .value = memix::AgentTreeEntryKind::Tree,
        .token = "tree",
    },
};

template <typename Value>
using StoreResult = memix::detail::Result<Value>;

using StoreVoid = memix::detail::VoidResult;

namespace expected_fs = memix::detail::expected_fs;

using memix::detail::apply_read_only_directory_permissions;
using memix::detail::apply_read_only_file_permissions;
using memix::detail::filesystem_error_message;
using memix::detail::is_lower_hex_sha256;

struct AgentTreeEntryJson {
  std::string_view name;
  std::string_view kind;
  std::string_view id;
};

template <typename Builder>
void tag_invoke([[maybe_unused]] simdjson::serialize_tag tag, Builder &builder,
                AgentTreeEntryJson entry) {
  builder.start_object();
  builder.template append_key_value<"name">(entry.name);
  builder.append_comma();
  builder.template append_key_value<"kind">(entry.kind);
  builder.append_comma();
  builder.template append_key_value<"id">(entry.id);
  builder.end_object();
}

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
  return memix::detail::unexpected_error(std::move(message));
}

StoreVoid require_regular_file(const fs::path &path,
                               std::string missing_message) {
  return expected_fs::is_regular_file(path).and_then(
      [missing_message = std::move(missing_message)](
          bool is_regular_file) mutable -> StoreVoid {
        if (!is_regular_file) {
          return store_error(std::move(missing_message));
        }
        return {};
      });
}

StoreVoid require_directory(const fs::path &path, std::string missing_message) {
  return expected_fs::is_directory(path).and_then(
      [missing_message =
           std::move(missing_message)](bool is_directory) mutable -> StoreVoid {
        if (!is_directory) {
          return store_error(std::move(missing_message));
        }
        return {};
      });
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
    if (read_count > 0
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

  return memix::detail::hex_encode(
      std::span<const unsigned char>{digest.data(), digest_length});
}

struct SourceFileMetadata {
  std::uintmax_t size_bytes = 0;
  std::string hash;
};

StoreResult<SourceFileMetadata> regular_file_metadata(const fs::path &path) {
  return expected_fs::file_size(path).and_then(
      [&path](std::uintmax_t size_bytes) {
        return sha256_file(path).transform([size_bytes](std::string hash) {
          return SourceFileMetadata{
              .size_bytes = size_bytes,
              .hash = std::move(hash),
          };
        });
      });
}

StoreResult<std::string> sha256_text(std::string_view text) {
  return memix::detail::sha256_text(text);
}

std::string simdjson_error_text(simdjson::error_code error) {
  return simdjson::error_message(error);
}

StoreResult<std::string>
json_from_builder(const simdjson::builder::string_builder &builder) {
  return memix::detail::json_from_builder(
      builder,
      memix::detail::JsonBuilderMessages{
          .invalid_utf8_message = "metadata JSON contains invalid UTF-8",
          .build_error_prefix = "failed to build metadata JSON",
      });
}

StoreResult<std::string> metadata_json_for(const memix::StoredObject &object) {
  simdjson::builder::string_builder builder;
  builder.start_object();
  builder.append_key_value<"schema_version">(object.schema_version);
  builder.append_comma();
  builder.append_key_value<"object_version">(object.object_version);
  builder.append_comma();
  builder.append_key_value<"type">(memix::to_string(object.type));
  builder.append_comma();
  builder.append_key_value<"id">(object.id);
  builder.append_comma();
  builder.append_key_value<"hash_algorithm">(object.hash_algorithm);
  builder.append_comma();
  builder.append_key_value<"hash">(object.hash);
  builder.append_comma();
  builder.append_key_value<"size_bytes">(object.size_bytes);
  builder.append_comma();
  builder.append_key_value<"filename">(object.filename);
  builder.append_comma();
  builder.append_key_value<"created_at">(object.created_at);
  builder.append_comma();
  builder.append_key_value<"content_path">(
      object.content_path.generic_string());
  builder.end_object();
  return json_from_builder(builder);
}

StoreResult<std::string>
agent_tree_json_for(const std::vector<memix::AgentTreeEntry> &entries) {
  const auto json_entries =
      entries | std::views::transform([](const memix::AgentTreeEntry &entry) {
        return AgentTreeEntryJson{
            .name = entry.name,
            .kind = memix::to_string(entry.kind),
            .id = entry.id,
        };
      });

  simdjson::builder::string_builder builder;
  builder.start_object();
  builder.append_key_value<"schema_version">(1);
  builder.append_comma();
  builder.append_key_value<"object_version">(1);
  builder.append_comma();
  builder.append_key_value<"kind">("agent_tree");
  builder.append_comma();
  builder.append_key_value<"entries">(json_entries);
  builder.end_object();
  return json_from_builder(builder);
}

struct AgentGenerationJsonRequest {
  std::string_view root_tree;
  std::optional<std::string_view> parent;
  std::string_view created_at;
  std::string_view message;
};

StoreResult<std::string>
agent_generation_json_for(AgentGenerationJsonRequest request) {
  simdjson::builder::string_builder builder;
  builder.start_object();
  builder.append_key_value<"schema_version">(1);
  builder.append_comma();
  builder.append_key_value<"object_version">(1);
  builder.append_comma();
  builder.append_key_value<"kind">("agent_generation");
  builder.append_comma();
  builder.append_key_value<"root_tree">(request.root_tree);
  builder.append_comma();
  builder.append_key_value<"parent">(request.parent);
  builder.append_comma();
  builder.append_key_value<"created_at">(request.created_at);
  builder.append_comma();
  builder.append_key_value<"message">(request.message);
  builder.end_object();
  return json_from_builder(builder);
}

StoreResult<std::string> read_text_file(const fs::path &path) {
  return memix::detail::read_text_file(path, "metadata");
}

StoreVoid write_text_file(const fs::path &path, std::string_view contents) {
  return memix::detail::write_text_file(path, contents);
}

template <typename Value>
StoreResult<Value> require_field_as(simdjson::dom::object fields,
                                    std::string_view key,
                                    std::string_view expected_type) {
  Value parsed{};
  const simdjson::error_code error = fields.at_key(key).get(parsed);
  if (error != simdjson::SUCCESS) {
    return store_error(std::format("metadata field `{}` must be {}: {}", key,
                                   expected_type, simdjson_error_text(error)));
  }
  return parsed;
}

StoreResult<simdjson::dom::element> require_field(simdjson::dom::object fields,
                                                  std::string_view key) {
  return require_field_as<simdjson::dom::element>(fields, key, "present");
}

StoreResult<std::string> require_string(simdjson::dom::object fields,
                                        std::string_view key) {
  return require_field_as<std::string_view>(fields, key, "a string")
      .transform([](std::string_view value) { return std::string(value); });
}

StoreResult<std::uintmax_t> require_number(simdjson::dom::object fields,
                                           std::string_view key) {
  return require_field_as<std::uint64_t>(fields, key, "an unsigned integer")
      .transform([](std::uint64_t value) {
        return static_cast<std::uintmax_t>(value);
      });
}

StoreResult<simdjson::dom::array> require_array(simdjson::dom::object fields,
                                                std::string_view key) {
  return require_field_as<simdjson::dom::array>(fields, key, "an array");
}

StoreResult<int> checked_int(std::uintmax_t value,
                             std::string_view field_name) {
  if (value > static_cast<std::uintmax_t>(std::numeric_limits<int>::max())) {
    return store_error(
        std::format("metadata field `{}` is too large", field_name));
  }
  return static_cast<int>(value);
}

StoreResult<int> require_int(simdjson::dom::object fields,
                             std::string_view key) {
  return require_number(fields, key).and_then([key](std::uintmax_t value) {
    return checked_int(value, key);
  });
}

StoreResult<memix::MemoryObjectType>
require_memory_object_type(simdjson::dom::object fields, std::string_view key) {
  return require_string(fields, key)
      .and_then([](std::string_view type_text)
                    -> StoreResult<memix::MemoryObjectType> {
        const std::optional<memix::MemoryObjectType> parsed_type =
            memix::parse_memory_object_type(type_text);
        if (!parsed_type.has_value()) {
          return store_error(
              std::format("metadata has unsupported type `{}`", type_text));
        }
        return *parsed_type;
      });
}

StoreResult<memix::AgentTreeEntryKind>
require_agent_tree_entry_kind(simdjson::dom::object fields,
                              std::string_view key) {
  return require_string(fields, key)
      .and_then([](std::string_view kind_text)
                    -> StoreResult<memix::AgentTreeEntryKind> {
        const std::optional<memix::AgentTreeEntryKind> parsed_kind =
            memix::parse_agent_tree_entry_kind(kind_text);
        if (!parsed_kind.has_value()) {
          return store_error(std::format(
              "agent tree entry has unsupported kind `{}`", kind_text));
        }
        return *parsed_kind;
      });
}

fs::path type_dir_for(const fs::path &root, memix::MemoryObjectType type) {
  return root / std::string(memix::to_string(type));
}

fs::path agents_dir_for(const fs::path &root) {
  return type_dir_for(root, memix::MemoryObjectType::Agents);
}

fs::path agent_blobs_dir_for(const fs::path &root) {
  return agents_dir_for(root) / std::string(kAgentBlobsDir);
}

fs::path agent_trees_dir_for(const fs::path &root) {
  return agents_dir_for(root) / std::string(kAgentTreesDir);
}

fs::path agent_generations_dir_for(const fs::path &root) {
  return agents_dir_for(root) / std::string(kAgentGenerationsDir);
}

fs::path refs_dir_for(const fs::path &root) {
  return root / std::string(kRefsDir);
}

fs::path locks_dir_for(const fs::path &root) {
  return root / std::string(kLocksDir);
}

StoreVoid ensure_directory(const fs::path &path, fs::perms permissions) {
  const StoreResult<bool> directory_created =
      expected_fs::create_directories(path);
  if (!directory_created) {
    return std::unexpected(directory_created.error());
  }

  const StoreVoid permissions_set =
      expected_fs::permissions(path, permissions, fs::perm_options::replace);
  if (!permissions_set) {
    return std::unexpected(permissions_set.error());
  }
  return {};
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
  return memix::detail::unique_temp_dir(
      type_dir,
      memix::detail::TempDirOptions{
          .prefix = hash,
          .failure_message = "failed to allocate a temporary object directory",
      });
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
  const StoreResult<int> schema_version = require_int(fields, "schema_version");
  if (!schema_version) {
    return std::unexpected(schema_version.error());
  }
  object.schema_version = *schema_version;

  const StoreResult<int> object_version = require_int(fields, "object_version");
  if (!object_version) {
    return std::unexpected(object_version.error());
  }
  object.object_version = *object_version;

  const StoreResult<memix::MemoryObjectType> type =
      require_memory_object_type(fields, "type");
  if (!type) {
    return std::unexpected(type.error());
  }
  object.type = *type;

  const StoreResult<std::string> object_id = require_string(fields, "id");
  if (!object_id) {
    return std::unexpected(object_id.error());
  }
  object.id = *object_id;

  const StoreResult<std::string> hash_algorithm =
      require_string(fields, "hash_algorithm");
  if (!hash_algorithm) {
    return std::unexpected(hash_algorithm.error());
  }
  object.hash_algorithm = *hash_algorithm;

  const StoreResult<std::string> hash = require_string(fields, "hash");
  if (!hash) {
    return std::unexpected(hash.error());
  }
  object.hash = *hash;

  const StoreResult<std::uintmax_t> size_bytes =
      require_number(fields, "size_bytes");
  if (!size_bytes) {
    return std::unexpected(size_bytes.error());
  }
  object.size_bytes = *size_bytes;

  const StoreResult<std::string> filename = require_string(fields, "filename");
  if (!filename) {
    return std::unexpected(filename.error());
  }
  object.filename = *filename;

  const StoreResult<std::string> created_at =
      require_string(fields, "created_at");
  if (!created_at) {
    return std::unexpected(created_at.error());
  }
  object.created_at = *created_at;

  const StoreResult<std::string> content_path =
      require_string(fields, "content_path");
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
  const StoreResult<std::uintmax_t> content_size =
      require_regular_file(
          content_file,
          std::format("object content is missing for `{}`", object.id))
          .and_then(
              [&content_file] { return expected_fs::file_size(content_file); });
  if (!content_size) {
    return std::unexpected(content_size.error());
  }
  if (*content_size != object.size_bytes) {
    return store_error(std::format(
        "object content size does not match metadata for `{}`", object.id));
  }

  const StoreResult<std::string> content_hash = sha256_file(content_file);
  if (!content_hash) {
    return std::unexpected(content_hash.error());
  }
  if (*content_hash != object.hash) {
    return store_error(std::format(
        "object content hash does not match metadata for `{}`", object.id));
  }

  return {};
}

StoreResult<memix::StoredObject>
read_existing_object(const fs::path &object_dir, memix::MemoryObjectType type) {
  return require_directory(
             object_dir,
             std::format("object path exists but is not a directory `{}`",
                         object_dir.string()))
      .and_then([&object_dir, type] {
        return object_from_metadata_path(object_dir, type,
                                         object_dir / kMetadataFilename);
      });
}

bool is_temp_store_entry(const fs::path &path) {
  const std::string name = path.filename().string();
  return name.starts_with(".tmp-");
}

StoreResult<std::vector<memix::StoredObject>>
read_objects_for_type(const fs::path &root, memix::MemoryObjectType type) {
  std::vector<memix::StoredObject> objects;
  const fs::path type_dir = type == memix::MemoryObjectType::Agents
                              ? agent_blobs_dir_for(root)
                              : type_dir_for(root, type);
  const StoreResult<bool> exists = expected_fs::exists(type_dir);
  if (!exists) {
    return std::unexpected(exists.error());
  }
  if (!*exists) {
    return objects;
  }
  const StoreResult<bool> is_directory = expected_fs::is_directory(type_dir);
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
                                                  entry_error, entry.path()));
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
                                                iterator_error, type_dir));
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

void sort_stored_objects(std::vector<memix::StoredObject> &objects) {
  std::ranges::sort(objects, [](const memix::StoredObject &lhs,
                                const memix::StoredObject &rhs) {
    const auto lhs_key = std::tuple{type_to_owned_string(lhs.type),
                                    lhs.created_at, lhs.id, lhs.filename};
    const auto rhs_key = std::tuple{type_to_owned_string(rhs.type),
                                    rhs.created_at, rhs.id, rhs.filename};
    return lhs_key < rhs_key;
  });
}

StoreResult<std::vector<memix::StoredObject>>
read_objects_for_types(const fs::path &root,
                       std::span<const memix::MemoryObjectType> types) {
  std::vector<memix::StoredObject> objects;
  for (const memix::MemoryObjectType current_type : types) {
    StoreResult<std::vector<memix::StoredObject>> type_objects =
        read_objects_for_type(root, current_type);
    if (!type_objects) {
      return std::unexpected(type_objects.error());
    }
    for (memix::StoredObject &object : *type_objects) {
      objects.push_back(std::move(object));
    }
  }
  sort_stored_objects(objects);
  return objects;
}

StoreResult<std::vector<memix::StoredObject>>
read_objects_for_existing_root(const fs::path &root,
                               std::optional<memix::MemoryObjectType> type) {
  return require_directory(
             root,
             std::format("store root is not a directory `{}`", root.string()))
      .and_then([&root, type] {
        const std::vector<memix::MemoryObjectType> types = types_for_list(type);
        return read_objects_for_types(root, types);
      });
}

StoreResult<std::optional<memix::StoredObject>>
read_existing_object_if_present(const fs::path &object_dir,
                                memix::MemoryObjectType type) {
  const StoreResult<bool> object_exists = expected_fs::exists(object_dir);
  if (!object_exists) {
    return std::unexpected(object_exists.error());
  }
  if (!*object_exists) {
    return StoreResult<std::optional<memix::StoredObject>>{
        std::in_place,
        std::nullopt,
    };
  }
  StoreResult<memix::StoredObject> object =
      read_existing_object(object_dir, type);
  if (!object) {
    return std::unexpected(object.error());
  }
  return StoreResult<std::optional<memix::StoredObject>>{
      std::in_place,
      std::move(*object),
  };
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
  operator=(TemporaryDirectoryCleanup &&other) = delete;

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

StoreResult<TemporaryDirectoryCleanup>
acquire_directory_lock(const fs::path &lock_path, std::string_view lock_name) {
  const StoreVoid parent_created =
      ensure_directory(lock_path.parent_path(), fs::perms::owner_all);
  if (!parent_created) {
    return std::unexpected(parent_created.error());
  }

  const StoreResult<bool> lock_created =
      expected_fs::create_directory(lock_path);
  if (!lock_created) {
    return std::unexpected(lock_created.error());
  }
  if (!*lock_created) {
    return store_error(std::format("{} is locked", lock_name));
  }
  return StoreResult<TemporaryDirectoryCleanup>{std::in_place, lock_path};
}

StoreResult<TemporaryDirectoryCleanup>
acquire_agent_update_lock(const fs::path &root) {
  return acquire_directory_lock(
      locks_dir_for(root) / std::string(kAgentUpdateLock), "agent update");
}

StoreVoid write_immutable_text_file_if_absent(const fs::path &path,
                                              std::string_view contents) {
  const StoreResult<bool> existing = expected_fs::exists(path);
  if (!existing) {
    return std::unexpected(existing.error());
  }
  if (*existing) {
    const StoreResult<bool> existing_is_file =
        expected_fs::is_regular_file(path);
    if (!existing_is_file) {
      return std::unexpected(existing_is_file.error());
    }
    if (!*existing_is_file) {
      return store_error(std::format("immutable object path is not a file `{}`",
                                     path.string()));
    }
    return {};
  }

  const StoreVoid parent_created =
      ensure_directory(path.parent_path(), fs::perms::owner_all);
  if (!parent_created) {
    return std::unexpected(parent_created.error());
  }

  const StoreResult<fs::path> temp_dir =
      unique_temp_dir(path.parent_path(), path.filename().string());
  if (!temp_dir) {
    return std::unexpected(temp_dir.error());
  }
  const StoreResult<bool> created_temp_dir =
      expected_fs::create_directory(*temp_dir);
  if (!created_temp_dir) {
    return std::unexpected(created_temp_dir.error());
  }
  if (!*created_temp_dir) {
    return store_error(
        std::format("failed to create temporary object directory `{}`",
                    temp_dir->string()));
  }
  TemporaryDirectoryCleanup temp_dir_cleanup(*temp_dir);

  const fs::path temp_file = *temp_dir / "object";
  const StoreVoid written = write_text_file(temp_file, contents);
  if (!written) {
    return std::unexpected(written.error());
  }
  const StoreVoid temp_file_read_only =
      apply_read_only_file_permissions(temp_file);
  if (!temp_file_read_only) {
    return std::unexpected(temp_file_read_only.error());
  }

  const StoreVoid renamed = expected_fs::rename(temp_file, path);
  if (!renamed) {
    const StoreResult<bool> raced_existing = expected_fs::exists(path);
    if (!raced_existing) {
      return std::unexpected(raced_existing.error());
    }
    if (*raced_existing) {
      return {};
    }
    return std::unexpected(renamed.error());
  }
  temp_dir_cleanup.release();
  const StoreResult<std::uintmax_t> removed =
      expected_fs::remove_all(*temp_dir);
  if (!removed) {
    return std::unexpected(removed.error());
  }
  return {};
}

StoreResult<memix::IngestResult>
publish_new_object(const PublishRequest &request) {
  const StoreVoid source_is_regular = require_regular_file(
      request.source_path,
      std::format("ingest source must be a regular file `{}`",
                  request.source_path.string()));
  if (!source_is_regular) {
    return std::unexpected(source_is_regular.error());
  }

  const StoreResult<fs::path> temp_dir = unique_temp_dir(
      request.type_dir, request.source_path.filename().string());
  if (!temp_dir) {
    return std::unexpected(temp_dir.error());
  }

  const StoreResult<bool> created_temp_dir =
      expected_fs::create_directory(*temp_dir);
  if (!created_temp_dir) {
    return std::unexpected(created_temp_dir.error());
  }
  if (!*created_temp_dir) {
    return store_error(
        std::format("failed to create temporary object directory `{}`",
                    temp_dir->string()));
  }
  TemporaryDirectoryCleanup temp_dir_cleanup(*temp_dir);

  const fs::path temp_object = *temp_dir / kContentPath;
  const fs::path temp_metadata = *temp_dir / kMetadataFilename;

  const StoreResult<bool> copied = expected_fs::copy_file(
      request.source_path, temp_object, fs::copy_options::none);
  if (!copied) {
    return std::unexpected(copied.error());
  }
  if (!*copied) {
    return store_error(std::format("failed to copy source into store `{}`",
                                   request.source_path.string()));
  }

  StoreResult<SourceFileMetadata> source = regular_file_metadata(temp_object);
  if (!source) {
    return std::unexpected(source.error());
  }
  const fs::path object_dir = request.type_dir / source->hash;
  StoreResult<std::optional<memix::StoredObject>> existing =
      read_existing_object_if_present(object_dir, request.type);
  if (!existing) {
    return std::unexpected(existing.error());
  }
  if (existing->has_value()) {
    return StoreResult<memix::IngestResult>{
        std::in_place,
        std::move(**existing),
        false,
    };
  }

  const StoreResult<std::string> created_at = utc_timestamp();
  if (!created_at) {
    return std::unexpected(created_at.error());
  }

  memix::StoredObject object;
  object.type = request.type;
  object.id = source->hash;
  object.hash = std::move(source->hash);
  object.hash_algorithm = std::string(kHashAlgorithm);
  object.size_bytes = source->size_bytes;
  object.filename = request.source_path.filename().string();
  object.created_at = *created_at;
  object.content_path = fs::path(kContentPath);
  object.object_dir = object_dir;
  object.metadata_path = object_dir / kMetadataFilename;

  const StoreResult<std::string> metadata = memix::metadata_json(object);
  if (!metadata) {
    return std::unexpected(metadata.error());
  }
  const StoreVoid metadata_written = write_text_file(temp_metadata, *metadata);
  if (!metadata_written) {
    return std::unexpected(metadata_written.error());
  }
  const StoreVoid object_read_only =
      apply_read_only_file_permissions(temp_object);
  if (!object_read_only) {
    return std::unexpected(object_read_only.error());
  }
  const StoreVoid metadata_read_only =
      apply_read_only_file_permissions(temp_metadata);
  if (!metadata_read_only) {
    return std::unexpected(metadata_read_only.error());
  }

  const StoreVoid renamed = expected_fs::rename(*temp_dir, object_dir);
  if (!renamed) {
    StoreResult<std::optional<memix::StoredObject>> raced_existing =
        read_existing_object_if_present(object_dir, request.type);
    if (!raced_existing) {
      return std::unexpected(raced_existing.error());
    }
    if (raced_existing->has_value()) {
      return StoreResult<memix::IngestResult>{
          std::in_place,
          std::move(**raced_existing),
          false,
      };
    }
    return std::unexpected(renamed.error());
  }

  temp_dir_cleanup.release();

  const StoreVoid object_dir_read_only =
      apply_read_only_directory_permissions(object_dir);
  if (!object_dir_read_only) {
    return std::unexpected(object_dir_read_only.error());
  }
  return StoreResult<memix::IngestResult>{
      std::in_place,
      std::move(object),
      true,
  };
}

bool valid_logical_component(std::string_view component) {
  return !component.empty()
      && component != "."
      && component != ".."
      && !component.contains('/')
      && !component.contains('\\');
}

struct LogicalPathParseRequest {
  std::string_view path;
  std::string_view label;
};

StoreResult<std::vector<std::string>>
logical_path_components(LogicalPathParseRequest request) {
  if (request.path.empty()) {
    return store_error(std::format("{} must not be empty", request.label));
  }
  if (request.path.ends_with('/')) {
    return store_error(std::format("{} must not end with `/`", request.label));
  }
  if (request.path.contains("//")) {
    return store_error(
        std::format("{} must not contain empty components", request.label));
  }

  const fs::path parsed{std::string(request.path)};
  if (parsed.is_absolute()) {
    return store_error(std::format("{} must be relative", request.label));
  }

  std::vector<std::string> components;
  for (const fs::path &component_path : parsed) {
    const std::string component = component_path.generic_string();
    if (!valid_logical_component(component)) {
      return store_error(std::format("{} contains invalid component `{}`",
                                     request.label, component));
    }
    components.push_back(component);
  }
  if (components.empty()) {
    return store_error(std::format("{} must not be empty", request.label));
  }
  return components;
}

StoreResult<std::vector<std::string>>
agent_path_components(const fs::path &agent_path) {
  const fs::path::string_type &native_path = agent_path.native();
  if (native_path.contains(static_cast<fs::path::value_type>('\\'))) {
    return store_error("agent path must not contain backslashes");
  }
  return logical_path_components({
      .path = agent_path.generic_string(),
      .label = "agent path",
  });
}

StoreResult<std::vector<std::string>>
ref_name_components(std::string_view ref_name) {
  return logical_path_components({
      .path = ref_name,
      .label = "ref name",
  });
}

fs::path path_from_components(const std::vector<std::string> &components) {
  fs::path result;
  for (const std::string &component : components) {
    result /= component;
  }
  return result;
}

struct AgentObjectIdValidation {
  std::string_view object_id;
  std::string_view object_kind;
};

StoreVoid validate_agent_object_id(AgentObjectIdValidation validation) {
  if (!is_lower_hex_sha256(validation.object_id)) {
    return store_error(std::format("{} id is not a lowercase sha256 digest",
                                   validation.object_kind));
  }
  return {};
}

StoreVoid
validate_agent_tree_entries(const std::vector<memix::AgentTreeEntry> &entries) {
  std::optional<std::string_view> previous_name;
  for (const memix::AgentTreeEntry &entry : entries) {
    if (!valid_logical_component(entry.name)) {
      return store_error(std::format(
          "agent tree contains invalid entry name `{}`", entry.name));
    }
    if (previous_name.has_value() && entry.name <= *previous_name) {
      return store_error("agent tree entries must be sorted and unique");
    }
    const StoreVoid id_valid = validate_agent_object_id({
        .object_id = entry.id,
        .object_kind = "agent tree entry",
    });
    if (!id_valid) {
      return std::unexpected(id_valid.error());
    }
    previous_name = entry.name;
  }
  return {};
}

StoreResult<std::vector<memix::AgentTreeEntry>>
canonical_agent_tree_entries(std::vector<memix::AgentTreeEntry> entries) {
  std::ranges::sort(entries, {}, &memix::AgentTreeEntry::name);
  return validate_agent_tree_entries(entries).transform(
      [&entries] { return std::move(entries); });
}

StoreResult<memix::StoredObject>
read_agent_blob_object(const fs::path &root, std::string_view object_id) {
  return validate_agent_object_id({
                                      .object_id = object_id,
                                      .object_kind = "agent blob",
                                  })
      .and_then([&root, object_id] {
        return read_existing_object(agent_blobs_dir_for(root)
                                        / std::string(object_id),
                                    memix::MemoryObjectType::Agents);
      });
}

StoreResult<memix::AgentTreeEntry>
agent_tree_entry_from_json(simdjson::dom::element entry_element) {
  simdjson::dom::object entry_fields;
  const simdjson::error_code error = entry_element.get(entry_fields);
  if (error != simdjson::SUCCESS) {
    return store_error(std::format("agent tree entry must be an object: {}",
                                   simdjson_error_text(error)));
  }

  const StoreResult<std::string> name = require_string(entry_fields, "name");
  if (!name) {
    return std::unexpected(name.error());
  }
  const StoreResult<memix::AgentTreeEntryKind> entry_kind =
      require_agent_tree_entry_kind(entry_fields, "kind");
  if (!entry_kind) {
    return std::unexpected(entry_kind.error());
  }
  const StoreResult<std::string> entry_id = require_string(entry_fields, "id");
  if (!entry_id) {
    return std::unexpected(entry_id.error());
  }
  return memix::AgentTreeEntry{
      .name = *name,
      .kind = *entry_kind,
      .id = *entry_id,
  };
}

StoreResult<memix::AgentTree>
read_agent_tree_object(const fs::path &root, std::string_view object_id) {
  const StoreVoid id_valid = validate_agent_object_id({
      .object_id = object_id,
      .object_kind = "agent tree",
  });
  if (!id_valid) {
    return std::unexpected(id_valid.error());
  }
  const fs::path object_path =
      agent_trees_dir_for(root) / std::format("{}{}", object_id, kJsonSuffix);
  const StoreResult<std::string> tree_text = read_text_file(object_path);
  if (!tree_text) {
    return std::unexpected(tree_text.error());
  }
  const StoreResult<std::string> actual_hash = sha256_text(*tree_text);
  if (!actual_hash) {
    return std::unexpected(actual_hash.error());
  }
  if (*actual_hash != object_id) {
    return store_error(std::format("agent tree hash does not match `{}`",
                                   object_path.string()));
  }

  const simdjson::padded_string padded_json(*tree_text);
  simdjson::dom::parser parser;
  simdjson::dom::element document;
  simdjson::error_code error = parser.parse(padded_json).get(document);
  if (error != simdjson::SUCCESS) {
    return store_error(std::format("invalid agent tree JSON `{}`: {}",
                                   object_path.string(),
                                   simdjson_error_text(error)));
  }

  simdjson::dom::object fields;
  error = document.get(fields);
  if (error != simdjson::SUCCESS) {
    return store_error(std::format("agent tree JSON must be an object `{}`: {}",
                                   object_path.string(),
                                   simdjson_error_text(error)));
  }

  memix::AgentTree tree;
  tree.id = std::string(object_id);
  tree.object_path = object_path;

  const StoreResult<int> schema_version = require_int(fields, "schema_version");
  if (!schema_version) {
    return std::unexpected(schema_version.error());
  }
  tree.schema_version = *schema_version;

  const StoreResult<int> object_version = require_int(fields, "object_version");
  if (!object_version) {
    return std::unexpected(object_version.error());
  }
  tree.object_version = *object_version;

  const StoreResult<std::string> kind = require_string(fields, "kind");
  if (!kind) {
    return std::unexpected(kind.error());
  }
  if (*kind != "agent_tree") {
    return store_error(
        std::format("agent tree has unsupported kind `{}`", *kind));
  }

  if (tree.schema_version != 1 || tree.object_version != 1) {
    return store_error("agent tree schema or object version is not supported");
  }

  const StoreResult<simdjson::dom::array> entries =
      require_array(fields, "entries");
  if (!entries) {
    return std::unexpected(entries.error());
  }
  for (const simdjson::dom::element entry_element : *entries) {
    StoreResult<memix::AgentTreeEntry> tree_entry =
        agent_tree_entry_from_json(entry_element);
    if (!tree_entry) {
      return std::unexpected(tree_entry.error());
    }
    tree.entries.push_back(std::move(*tree_entry));
  }

  const StoreVoid entries_valid = validate_agent_tree_entries(tree.entries);
  if (!entries_valid) {
    return std::unexpected(entries_valid.error());
  }
  return tree;
}

StoreResult<memix::AgentTree>
write_agent_tree_object(const fs::path &root,
                        std::vector<memix::AgentTreeEntry> entries) {
  StoreResult<std::vector<memix::AgentTreeEntry>> canonical_entries =
      canonical_agent_tree_entries(std::move(entries));
  if (!canonical_entries) {
    return std::unexpected(canonical_entries.error());
  }
  const StoreResult<std::string> json = agent_tree_json_for(*canonical_entries);
  if (!json) {
    return std::unexpected(json.error());
  }
  const StoreResult<std::string> hash = sha256_text(*json);
  if (!hash) {
    return std::unexpected(hash.error());
  }

  const fs::path object_path =
      agent_trees_dir_for(root) / std::format("{}{}", *hash, kJsonSuffix);
  const StoreVoid written =
      write_immutable_text_file_if_absent(object_path, *json);
  if (!written) {
    return std::unexpected(written.error());
  }
  return memix::AgentTree{
      .id = *hash,
      .entries = std::move(*canonical_entries),
      .object_path = object_path,
  };
}

StoreResult<memix::AgentGeneration>
read_agent_generation_object(const fs::path &root, std::string_view object_id) {
  const StoreVoid id_valid = validate_agent_object_id({
      .object_id = object_id,
      .object_kind = "agent generation",
  });
  if (!id_valid) {
    return std::unexpected(id_valid.error());
  }
  const fs::path object_path = agent_generations_dir_for(root)
                             / std::format("{}{}", object_id, kJsonSuffix);
  const StoreResult<std::string> generation_text = read_text_file(object_path);
  if (!generation_text) {
    return std::unexpected(generation_text.error());
  }
  const StoreResult<std::string> actual_hash = sha256_text(*generation_text);
  if (!actual_hash) {
    return std::unexpected(actual_hash.error());
  }
  if (*actual_hash != object_id) {
    return store_error(std::format("agent generation hash does not match `{}`",
                                   object_path.string()));
  }

  const simdjson::padded_string padded_json(*generation_text);
  simdjson::dom::parser parser;
  simdjson::dom::element document;
  simdjson::error_code error = parser.parse(padded_json).get(document);
  if (error != simdjson::SUCCESS) {
    return store_error(std::format("invalid agent generation JSON `{}`: {}",
                                   object_path.string(),
                                   simdjson_error_text(error)));
  }

  simdjson::dom::object fields;
  error = document.get(fields);
  if (error != simdjson::SUCCESS) {
    return store_error(
        std::format("agent generation JSON must be an object `{}`: {}",
                    object_path.string(), simdjson_error_text(error)));
  }

  memix::AgentGeneration generation;
  generation.id = std::string(object_id);
  generation.object_path = object_path;

  const StoreResult<int> schema_version = require_int(fields, "schema_version");
  if (!schema_version) {
    return std::unexpected(schema_version.error());
  }
  generation.schema_version = *schema_version;

  const StoreResult<int> object_version = require_int(fields, "object_version");
  if (!object_version) {
    return std::unexpected(object_version.error());
  }
  generation.object_version = *object_version;

  const StoreResult<std::string> kind = require_string(fields, "kind");
  if (!kind) {
    return std::unexpected(kind.error());
  }
  if (*kind != "agent_generation") {
    return store_error(
        std::format("agent generation has unsupported kind `{}`", *kind));
  }
  if (generation.schema_version != 1 || generation.object_version != 1) {
    return store_error(
        "agent generation schema or object version is not supported");
  }

  const StoreResult<std::string> root_tree =
      require_string(fields, "root_tree");
  if (!root_tree) {
    return std::unexpected(root_tree.error());
  }
  const StoreVoid root_tree_valid = validate_agent_object_id({
      .object_id = *root_tree,
      .object_kind = "agent generation root tree",
  });
  if (!root_tree_valid) {
    return std::unexpected(root_tree_valid.error());
  }
  generation.root_tree = *root_tree;

  const StoreResult<simdjson::dom::element> parent_field =
      require_field(fields, "parent");
  if (!parent_field) {
    return std::unexpected(parent_field.error());
  }
  if (!parent_field->is_null()) {
    std::string_view parent;
    error = parent_field->get(parent);
    if (error != simdjson::SUCCESS) {
      return store_error(
          std::format("metadata field `parent` must be null or a string: {}",
                      simdjson_error_text(error)));
    }
    const StoreVoid parent_valid = validate_agent_object_id({
        .object_id = parent,
        .object_kind = "agent generation parent",
    });
    if (!parent_valid) {
      return std::unexpected(parent_valid.error());
    }
    generation.parent = std::string(parent);
  }

  const StoreResult<std::string> created_at =
      require_string(fields, "created_at");
  if (!created_at) {
    return std::unexpected(created_at.error());
  }
  generation.created_at = *created_at;

  const StoreResult<std::string> message = require_string(fields, "message");
  if (!message) {
    return std::unexpected(message.error());
  }
  generation.message = *message;
  return generation;
}

StoreResult<memix::AgentGeneration>
write_agent_generation_object(const fs::path &root, std::string_view root_tree,
                              std::optional<std::string_view> parent,
                              std::string_view message) {
  const StoreVoid root_tree_valid = validate_agent_object_id({
      .object_id = root_tree,
      .object_kind = "agent generation root tree",
  });
  if (!root_tree_valid) {
    return std::unexpected(root_tree_valid.error());
  }
  if (parent.has_value()) {
    const StoreVoid parent_valid = validate_agent_object_id({
        .object_id = *parent,
        .object_kind = "agent generation parent",
    });
    if (!parent_valid) {
      return std::unexpected(parent_valid.error());
    }
    StoreResult<memix::AgentGeneration> parent_generation =
        read_agent_generation_object(root, *parent);
    if (!parent_generation) {
      return std::unexpected(parent_generation.error());
    }
  }
  const StoreResult<std::string> created_at = utc_timestamp();
  if (!created_at) {
    return std::unexpected(created_at.error());
  }
  const StoreResult<std::string> json = agent_generation_json_for({
      .root_tree = root_tree,
      .parent = parent,
      .created_at = *created_at,
      .message = message,
  });
  if (!json) {
    return std::unexpected(json.error());
  }
  const StoreResult<std::string> hash = sha256_text(*json);
  if (!hash) {
    return std::unexpected(hash.error());
  }

  const fs::path object_path =
      agent_generations_dir_for(root) / std::format("{}{}", *hash, kJsonSuffix);
  const StoreVoid written =
      write_immutable_text_file_if_absent(object_path, *json);
  if (!written) {
    return std::unexpected(written.error());
  }
  return memix::AgentGeneration{
      .id = *hash,
      .root_tree = std::string(root_tree),
      .parent = parent.transform(
          [](std::string_view value) { return std::string(value); }),
      .created_at = *created_at,
      .message = std::string(message),
      .object_path = object_path,
  };
}

std::vector<memix::AgentTreeEntry>::iterator
find_agent_tree_entry(std::vector<memix::AgentTreeEntry> &entries,
                      std::string_view name) {
  return std::ranges::find(entries, name, &memix::AgentTreeEntry::name);
}

std::vector<memix::AgentTreeEntry>::const_iterator
find_agent_tree_entry(const std::vector<memix::AgentTreeEntry> &entries,
                      std::string_view name) {
  return std::ranges::find(entries, name, &memix::AgentTreeEntry::name);
}

void upsert_agent_tree_entry(std::vector<memix::AgentTreeEntry> &entries,
                             memix::AgentTreeEntry replacement) {
  auto entry = find_agent_tree_entry(entries, replacement.name);
  if (entry == entries.end()) {
    entries.push_back(std::move(replacement));
    return;
  }
  *entry = std::move(replacement);
}

struct AgentTreeRewriteFrame {
  std::string name;
  std::vector<memix::AgentTreeEntry> entries;
};

StoreResult<std::optional<std::string>> next_agent_tree_id_for_ingest_component(
    const std::vector<memix::AgentTreeEntry> &entries, std::string_view name,
    bool is_leaf) {
  const auto entry = find_agent_tree_entry(entries, name);
  if (entry == entries.end()) {
    return std::optional<std::string>{};
  }
  if (is_leaf) {
    if (entry->kind != memix::AgentTreeEntryKind::Blob) {
      return store_error(
          std::format("agent path component `{}` conflicts with a tree", name));
    }
    return std::optional<std::string>{};
  }
  if (entry->kind != memix::AgentTreeEntryKind::Tree) {
    return store_error(
        std::format("agent path component `{}` conflicts with a file", name));
  }
  return std::optional<std::string>{entry->id};
}

StoreResult<std::string> attach_blob_to_agent_tree(
    const fs::path &root, std::optional<std::string_view> tree_id,
    std::span<const std::string> path_components, std::string_view blob_id) {
  if (path_components.empty()) {
    return store_error("agent path must contain at least one component");
  }

  std::optional<std::string> current_tree_id = tree_id.transform(
      [](std::string_view value) { return std::string(value); });
  std::vector<AgentTreeRewriteFrame> frames;

  for (auto component = path_components.begin();
       component != path_components.end(); ++component) {
    std::vector<memix::AgentTreeEntry> entries;
    if (current_tree_id.has_value()) {
      StoreResult<memix::AgentTree> tree =
          read_agent_tree_object(root, *current_tree_id);
      if (!tree) {
        return std::unexpected(tree.error());
      }
      entries = std::move(tree->entries);
    }

    const std::string_view name = *component;
    const bool is_leaf = std::next(component) == path_components.end();
    StoreResult<std::optional<std::string>> next_tree_id =
        next_agent_tree_id_for_ingest_component(entries, name, is_leaf);
    if (!next_tree_id) {
      return std::unexpected(next_tree_id.error());
    }

    frames.push_back(AgentTreeRewriteFrame{
        .name = std::string(name),
        .entries = std::move(entries),
    });
    current_tree_id = std::move(*next_tree_id);
  }

  std::string child_id = std::string(blob_id);
  bool child_is_blob = true;
  for (AgentTreeRewriteFrame &frame : std::views::reverse(frames)) {
    upsert_agent_tree_entry(frame.entries,
                            memix::AgentTreeEntry{
                                .name = frame.name,
                                .kind = child_is_blob
                                          ? memix::AgentTreeEntryKind::Blob
                                          : memix::AgentTreeEntryKind::Tree,
                                .id = child_id,
                            });
    StoreResult<memix::AgentTree> new_tree =
        write_agent_tree_object(root, std::move(frame.entries));
    if (!new_tree) {
      return std::unexpected(new_tree.error());
    }
    child_id = new_tree->id;
    child_is_blob = false;
  }
  return child_id;
}

StoreResult<fs::path> ref_path_for(const fs::path &root,
                                   std::string_view ref_name) {
  return ref_name_components(ref_name).transform(
      [&root](const std::vector<std::string> &components) {
        return refs_dir_for(root) / path_from_components(components);
      });
}

StoreResult<std::optional<std::string>>
read_ref_value(const fs::path &root, std::string_view ref_name) {
  const StoreResult<fs::path> path = ref_path_for(root, ref_name);
  if (!path) {
    return std::unexpected(path.error());
  }
  const StoreResult<bool> exists = expected_fs::exists(*path);
  if (!exists) {
    return std::unexpected(exists.error());
  }
  if (!*exists) {
    return StoreResult<std::optional<std::string>>{std::in_place, std::nullopt};
  }
  const StoreResult<bool> is_file = expected_fs::is_regular_file(*path);
  if (!is_file) {
    return std::unexpected(is_file.error());
  }
  if (!*is_file) {
    return store_error(
        std::format("ref path is not a file `{}`", path->string()));
  }

  StoreResult<std::string> contents = read_text_file(*path);
  if (!contents) {
    return std::unexpected(contents.error());
  }
  while (!contents->empty()
         && (contents->back() == '\n' || contents->back() == '\r')) {
    contents->pop_back();
  }
  const StoreVoid ref_valid = validate_agent_object_id({
      .object_id = *contents,
      .object_kind = "ref target generation",
  });
  if (!ref_valid) {
    return std::unexpected(ref_valid.error());
  }
  StoreResult<memix::AgentGeneration> generation =
      read_agent_generation_object(root, *contents);
  if (!generation) {
    return std::unexpected(generation.error());
  }
  return StoreResult<std::optional<std::string>>{
      std::in_place,
      std::move(*contents),
  };
}

StoreVoid update_ref_unlocked(const fs::path &root, std::string_view ref_name,
                              std::optional<std::string_view> expected_old,
                              std::string_view new_generation) {
  const StoreVoid new_generation_valid = validate_agent_object_id({
      .object_id = new_generation,
      .object_kind = "new generation",
  });
  if (!new_generation_valid) {
    return std::unexpected(new_generation_valid.error());
  }
  StoreResult<memix::AgentGeneration> generation =
      read_agent_generation_object(root, new_generation);
  if (!generation) {
    return std::unexpected(generation.error());
  }

  StoreResult<fs::path> ref_path = ref_path_for(root, ref_name);
  if (!ref_path) {
    return std::unexpected(ref_path.error());
  }
  const StoreVoid parent_created =
      ensure_directory(ref_path->parent_path(), fs::perms::owner_all);
  if (!parent_created) {
    return std::unexpected(parent_created.error());
  }
  const fs::path lock_path =
      fs::path(std::format("{}.lock", ref_path->string()));
  StoreResult<TemporaryDirectoryCleanup> lock =
      acquire_directory_lock(lock_path, std::format("ref `{}`", ref_name));
  if (!lock) {
    return std::unexpected(lock.error());
  }

  StoreResult<std::optional<std::string>> current =
      read_ref_value(root, ref_name);
  if (!current) {
    return std::unexpected(current.error());
  }
  if (expected_old.has_value()) {
    if (!current->has_value() || **current != *expected_old) {
      return store_error(
          std::format("ref `{}` does not match expected value", ref_name));
    }
  } else if (current->has_value()) {
    return store_error(std::format("ref `{}` already exists", ref_name));
  }

  const StoreResult<fs::path> temp_dir =
      unique_temp_dir(ref_path->parent_path(), ref_path->filename().string());
  if (!temp_dir) {
    return std::unexpected(temp_dir.error());
  }
  const StoreResult<bool> created_temp_dir =
      expected_fs::create_directory(*temp_dir);
  if (!created_temp_dir) {
    return std::unexpected(created_temp_dir.error());
  }
  if (!*created_temp_dir) {
    return store_error(std::format(
        "failed to create temporary ref directory `{}`", temp_dir->string()));
  }
  TemporaryDirectoryCleanup temp_dir_cleanup(*temp_dir);
  const fs::path temp_ref = *temp_dir / "ref";
  const StoreVoid written =
      write_text_file(temp_ref, std::format("{}\n", new_generation));
  if (!written) {
    return std::unexpected(written.error());
  }

  const StoreVoid renamed = expected_fs::rename(temp_ref, *ref_path);
  if (!renamed) {
    return std::unexpected(renamed.error());
  }
  temp_dir_cleanup.release();
  const StoreResult<std::uintmax_t> removed =
      expected_fs::remove_all(*temp_dir);
  if (!removed) {
    return std::unexpected(removed.error());
  }
  return {};
}

StoreVoid remove_ref_unlocked(const fs::path &root, std::string_view ref_name) {
  StoreResult<fs::path> ref_path = ref_path_for(root, ref_name);
  if (!ref_path) {
    return std::unexpected(ref_path.error());
  }
  const StoreResult<bool> removed = expected_fs::remove(*ref_path);
  if (!removed) {
    return std::unexpected(removed.error());
  }
  if (!*removed) {
    return store_error(
        std::format("failed to roll back missing ref `{}`", ref_name));
  }
  return {};
}

std::optional<std::uint64_t>
parse_generation_ref_number(std::string_view name) {
  if (name.empty()) {
    return std::nullopt;
  }
  std::uint64_t parsed = 0;
  for (const char digit : name) {
    if (digit < '0' || digit > '9') {
      return std::nullopt;
    }
    const auto value = static_cast<std::uint64_t>(digit - '0');
    if (parsed
        > (std::numeric_limits<std::uint64_t>::max() - value) / kDecimalRadix) {
      return std::nullopt;
    }
    parsed = (parsed * kDecimalRadix) + value;
  }
  return parsed;
}

StoreResult<std::string> next_agent_generation_ref_name(const fs::path &root) {
  const fs::path generation_refs =
      refs_dir_for(root) / "agents" / std::string(kAgentGenerationsDir);
  const StoreResult<bool> exists = expected_fs::exists(generation_refs);
  if (!exists) {
    return std::unexpected(exists.error());
  }
  if (!*exists) {
    return "agents/generations/000001";
  }
  const StoreResult<bool> is_directory =
      expected_fs::is_directory(generation_refs);
  if (!is_directory) {
    return std::unexpected(is_directory.error());
  }
  if (!*is_directory) {
    return store_error(
        std::format("generation refs path is not a directory `{}`",
                    generation_refs.string()));
  }

  std::uint64_t max_generation = 0;
  std::error_code iterator_error;
  const fs::directory_iterator end;
  for (fs::directory_iterator iterator{generation_refs, iterator_error};
       !iterator_error && iterator != end; iterator.increment(iterator_error)) {
    const fs::directory_entry &entry = *iterator;
    std::error_code entry_error;
    if (!entry.is_regular_file(entry_error)) {
      if (entry_error) {
        return store_error(filesystem_error_message("failed to inspect",
                                                    entry_error, entry.path()));
      }
      continue;
    }
    const std::string name = entry.path().filename().string();
    const std::optional<std::uint64_t> parsed =
        parse_generation_ref_number(name);
    if (parsed.has_value()) {
      max_generation = std::max(max_generation, *parsed);
    }
  }
  if (iterator_error) {
    return store_error(filesystem_error_message(
        "failed to list directory", iterator_error, generation_refs));
  }
  return std::format("agents/generations/{:06}", max_generation + 1);
}

StoreResult<std::string>
root_tree_id_for_generation(const fs::path &root,
                            std::string_view generation_id) {
  return validate_agent_object_id({
                                      .object_id = generation_id,
                                      .object_kind = "agent generation",
                                  })
      .and_then([&root, generation_id] {
        return read_agent_generation_object(root, generation_id);
      })
      .transform([](const memix::AgentGeneration &generation) {
        return generation.root_tree;
      });
}

StoreResult<std::string> latest_agent_root_tree_id(const fs::path &root) {
  StoreResult<std::optional<std::string>> latest =
      read_ref_value(root, kAgentsLatestRef);
  if (!latest) {
    return std::unexpected(latest.error());
  }
  if (!latest->has_value()) {
    return store_error("agents/latest ref does not exist");
  }
  return root_tree_id_for_generation(root, **latest);
}

StoreResult<std::string>
requested_agent_root_tree_id(const fs::path &root,
                             std::optional<std::string_view> generation_id) {
  if (generation_id.has_value()) {
    return root_tree_id_for_generation(root, *generation_id);
  }
  return latest_agent_root_tree_id(root);
}

struct AgentFileResolveRequest {
  std::reference_wrapper<const fs::path> root;
  std::reference_wrapper<const fs::path> agent_path;
  std::string_view root_tree_id;
};

StoreResult<memix::StoredObject>
resolve_agent_file(AgentFileResolveRequest request) {
  const StoreResult<std::vector<std::string>> components =
      agent_path_components(request.agent_path.get());
  if (!components) {
    return std::unexpected(components.error());
  }
  std::string tree_id = std::string(request.root_tree_id);
  for (auto component = components->begin(); component != components->end();
       ++component) {
    StoreResult<memix::AgentTree> current_tree =
        read_agent_tree_object(request.root.get(), tree_id);
    if (!current_tree) {
      return std::unexpected(current_tree.error());
    }
    const std::string_view name = *component;
    const auto entry = std::ranges::find(current_tree->entries, name,
                                         &memix::AgentTreeEntry::name);
    if (entry == current_tree->entries.end()) {
      return store_error(
          std::format("agent path does not exist `{}`",
                      request.agent_path.get().generic_string()));
    }
    const bool is_last = std::next(component) == components->end();
    if (is_last) {
      if (entry->kind != memix::AgentTreeEntryKind::Blob) {
        return store_error(
            std::format("agent path is not a file `{}`",
                        request.agent_path.get().generic_string()));
      }
      return read_agent_blob_object(request.root.get(), entry->id);
    }
    if (entry->kind != memix::AgentTreeEntryKind::Tree) {
      return store_error(
          std::format("agent path component `{}` is not a tree", name));
    }
    tree_id = entry->id;
  }
  return store_error("agent path resolution failed");
}

struct AgentFileVisit {
  fs::path path;
  memix::AgentTreeEntryKind kind = memix::AgentTreeEntryKind::Tree;
  std::string object_id;
};

StoreVoid collect_agent_files(const fs::path &root, std::string_view tree_id,
                              const fs::path &prefix,
                              std::vector<memix::AgentFile> &files) {
  std::vector<AgentFileVisit> pending;
  pending.push_back(AgentFileVisit{
      .path = prefix,
      .kind = memix::AgentTreeEntryKind::Tree,
      .object_id = std::string(tree_id),
  });

  while (!pending.empty()) {
    AgentFileVisit visit = std::move(pending.back());
    pending.pop_back();

    if (visit.kind == memix::AgentTreeEntryKind::Blob) {
      StoreResult<memix::StoredObject> blob =
          read_agent_blob_object(root, visit.object_id);
      if (!blob) {
        return std::unexpected(blob.error());
      }
      files.push_back(memix::AgentFile{
          .agent_path = std::move(visit.path),
          .blob = std::move(*blob),
      });
      continue;
    }

    StoreResult<memix::AgentTree> tree =
        read_agent_tree_object(root, visit.object_id);
    if (!tree) {
      return std::unexpected(tree.error());
    }
    for (const memix::AgentTreeEntry &entry :
         std::views::reverse(tree->entries)) {
      pending.push_back(AgentFileVisit{
          .path = visit.path / entry.name,
          .kind = entry.kind,
          .object_id = entry.id,
      });
    }
  }
  return {};
}

} // namespace

namespace memix {

std::string_view to_string(MemoryObjectType type) noexcept {
  for (const MemoryObjectTypeToken mapping : kMemoryObjectTypeTokens) {
    if (mapping.value == type) {
      return mapping.token;
    }
  }
  return {};
}

std::optional<MemoryObjectType>
parse_memory_object_type(std::string_view value) noexcept {
  for (const MemoryObjectTypeToken mapping : kMemoryObjectTypeTokens) {
    if (mapping.token == value) {
      return mapping.value;
    }
  }
  return std::nullopt;
}

std::vector<MemoryObjectType> memory_object_types() {
  std::vector<MemoryObjectType> types;
  types.reserve(kMemoryObjectTypeTokens.size());
  for (const MemoryObjectTypeToken mapping : kMemoryObjectTypeTokens) {
    types.push_back(mapping.value);
  }
  return types;
}

std::string_view to_string(AgentTreeEntryKind kind) noexcept {
  for (const AgentTreeEntryKindToken mapping : kAgentTreeEntryKindTokens) {
    if (mapping.value == kind) {
      return mapping.token;
    }
  }
  return {};
}

std::optional<AgentTreeEntryKind>
parse_agent_tree_entry_kind(std::string_view value) noexcept {
  for (const AgentTreeEntryKindToken mapping : kAgentTreeEntryKindTokens) {
    if (mapping.token == value) {
      return mapping.value;
    }
  }
  return std::nullopt;
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

std::expected<IngestResult, StoreError>
Store::ingest(MemoryObjectType type, const fs::path &source_path) const {
  if (type == MemoryObjectType::Agents) {
    return store_error(
        "agent ingest requires a destination path; use ingest_agent_file");
  }

  const fs::path type_dir = type_dir_for(root_, type);

  const StoreVoid directories_created =
      ensure_store_directories(root_, type_dir);
  if (!directories_created) {
    return std::unexpected(directories_created.error());
  }

  return publish_new_object(PublishRequest{
      .type = type,
      .source_path = source_path,
      .type_dir = type_dir,
  });
}

std::expected<IngestResult, StoreError>
Store::ingest_agent_blob(const fs::path &source_path) const {
  const fs::path type_dir = agent_blobs_dir_for(root_);

  const StoreVoid directories_created =
      ensure_store_directories(root_, type_dir);
  if (!directories_created) {
    return std::unexpected(directories_created.error());
  }

  return publish_new_object(PublishRequest{
      .type = MemoryObjectType::Agents,
      .source_path = source_path,
      .type_dir = type_dir,
  });
}

std::expected<AgentIngestResult, StoreError>
Store::ingest_agent_file(const AgentFileIngestRequest &request) const {
  const StoreResult<std::vector<std::string>> components =
      agent_path_components(request.agent_path);
  if (!components) {
    return std::unexpected(components.error());
  }

  std::expected<IngestResult, StoreError> blob =
      ingest_agent_blob(request.source_path);
  if (!blob) {
    return std::unexpected(blob.error());
  }

  StoreResult<TemporaryDirectoryCleanup> agent_lock =
      acquire_agent_update_lock(root_);
  if (!agent_lock) {
    return std::unexpected(agent_lock.error());
  }

  StoreResult<std::optional<std::string>> previous_generation =
      read_ref_value(root_, kAgentsLatestRef);
  if (!previous_generation) {
    return std::unexpected(previous_generation.error());
  }

  std::optional<std::string> parent_root_tree_id;
  if (previous_generation->has_value()) {
    StoreResult<AgentGeneration> parent =
        read_agent_generation_object(root_, **previous_generation);
    if (!parent) {
      return std::unexpected(parent.error());
    }
    parent_root_tree_id = parent->root_tree;
  }

  const std::optional<std::string_view> parent_root_tree =
      parent_root_tree_id.transform(
          [](std::string_view value) { return value; });
  const std::optional<std::string_view> parent_generation =
      previous_generation->transform(
          [](std::string_view value) { return value; });

  StoreResult<std::string> root_tree = attach_blob_to_agent_tree(
      root_, parent_root_tree, std::span<const std::string>{*components},
      blob->object.id);
  if (!root_tree) {
    return std::unexpected(root_tree.error());
  }

  const std::string default_message = std::format(
      "ingest {}", path_from_components(*components).generic_string());
  const std::string_view generation_message =
      request.message.empty() ? std::string_view(default_message)
                              : std::string_view(request.message);
  StoreResult<AgentGeneration> generation = write_agent_generation_object(
      root_, *root_tree, parent_generation, generation_message);
  if (!generation) {
    return std::unexpected(generation.error());
  }

  StoreResult<std::string> generation_ref =
      next_agent_generation_ref_name(root_);
  if (!generation_ref) {
    return std::unexpected(generation_ref.error());
  }
  StoreVoid generation_ref_updated =
      update_ref_unlocked(root_, *generation_ref, std::nullopt, generation->id);
  if (!generation_ref_updated) {
    return std::unexpected(generation_ref_updated.error());
  }

  StoreVoid ref_updated = update_ref_unlocked(
      root_, kAgentsLatestRef, parent_generation, generation->id);
  if (!ref_updated) {
    const StoreError &ref_error = ref_updated.error();
    const StoreVoid rollback = remove_ref_unlocked(root_, *generation_ref);
    if (!rollback) {
      return store_error(std::format(
          "{}; additionally failed to roll back `{}`: {}", ref_error.message,
          *generation_ref, rollback.error().message));
    }
    return std::unexpected(ref_error);
  }

  std::optional<std::string> previous_generation_result = *previous_generation;
  return AgentIngestResult{
      .blob = std::move(*blob),
      .generation = std::move(*generation),
      .previous_generation = std::move(previous_generation_result),
      .agent_path = path_from_components(*components),
  };
}

std::expected<StoredObject, StoreError>
Store::read_agent_blob(std::string_view object_id) const {
  return read_agent_blob_object(root_, object_id);
}

std::expected<AgentTree, StoreError>
Store::read_agent_tree(std::string_view object_id) const {
  return read_agent_tree_object(root_, object_id);
}

std::expected<AgentGeneration, StoreError>
Store::read_agent_generation(std::string_view object_id) const {
  return read_agent_generation_object(root_, object_id);
}

std::expected<StoredObject, StoreError>
Store::read_agent_file(const fs::path &agent_path,
                       std::optional<std::string_view> generation) const {
  StoreResult<std::string> root_tree_id =
      requested_agent_root_tree_id(root_, generation);
  if (!root_tree_id) {
    return std::unexpected(root_tree_id.error());
  }
  return resolve_agent_file({
      .root = root_,
      .agent_path = agent_path,
      .root_tree_id = *root_tree_id,
  });
}

std::expected<std::vector<AgentFile>, StoreError>
Store::list_agent_files(std::optional<std::string_view> generation) const {
  return requested_agent_root_tree_id(root_, generation)
      .and_then([this](std::string_view tree_id)
                    -> StoreResult<std::vector<AgentFile>> {
        std::vector<AgentFile> files;
        return collect_agent_files(root_, tree_id, fs::path{}, files)
            .transform([&files] { return std::move(files); });
      });
}

std::expected<std::optional<std::string>, StoreError>
Store::read_ref(std::string_view ref_name) const {
  return read_ref_value(root_, ref_name);
}

StoreVoid Store::update_ref(std::string_view ref_name,
                            std::optional<std::string_view> expected_old,
                            std::string_view new_generation) const {
  StoreResult<TemporaryDirectoryCleanup> agent_lock =
      acquire_agent_update_lock(root_);
  if (!agent_lock) {
    return std::unexpected(agent_lock.error());
  }
  return update_ref_unlocked(root_, ref_name, expected_old, new_generation);
}

std::expected<std::vector<StoredObject>, StoreError>
Store::list(std::optional<MemoryObjectType> type) const {
  return expected_fs::exists(root_).and_then(
      [this, type](bool root_exists) -> StoreResult<std::vector<StoredObject>> {
        if (!root_exists) {
          return std::vector<StoredObject>{};
        }
        return read_objects_for_existing_root(root_, type);
      });
}

} // namespace memix
