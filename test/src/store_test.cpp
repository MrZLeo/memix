#include "memix/store.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdlib>
#include <format>
#include <fstream>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace fs = memix::fs;

static_assert(memix::to_string(memix::MemoryObjectType::Rules) == "rules");
static_assert(memix::parse_memory_object_type("documents")
              == memix::MemoryObjectType::Documents);
static_assert(memix::memory_object_type_values()[1]
              == memix::MemoryObjectType::Chat);
static_assert(memix::to_string(memix::ContextTreeEntryKind::Tree) == "tree");
static_assert(memix::parse_context_tree_entry_kind("blob")
              == memix::ContextTreeEntryKind::Blob);

namespace {

void set_environment_value(const char *name,
                           const std::optional<std::string> &value) {
#if defined(_WIN32)
  _putenv_s(name, value.value_or("").c_str());
#else
  if (value.has_value()) {
    setenv(name, value->c_str(), 1);
  } else {
    unsetenv(name);
  }
#endif
}

std::optional<std::string> get_environment_value(const char *name) {
  const char *value = std::getenv(name);
  if (value == nullptr) {
    return std::nullopt;
  }
  return std::string(value);
}

class EnvironmentGuard {
public:
  EnvironmentGuard()
      : memix_store_home_(get_environment_value("MEMIX_STORE_HOME")),
        xdg_data_home_(get_environment_value("XDG_DATA_HOME")),
        home_(get_environment_value("HOME")) {}

  ~EnvironmentGuard() {
    set_environment_value("MEMIX_STORE_HOME", memix_store_home_);
    set_environment_value("XDG_DATA_HOME", xdg_data_home_);
    set_environment_value("HOME", home_);
  }

private:
  std::optional<std::string> memix_store_home_;
  std::optional<std::string> xdg_data_home_;
  std::optional<std::string> home_;
};

fs::path unique_test_path(std::string_view label) {
  std::random_device random_device;
  const auto now = std::chrono::steady_clock::now().time_since_epoch();
  const auto nanos =
      std::chrono::duration_cast<std::chrono::nanoseconds>(now).count();
  const memix::FsResult<fs::path> temp_dir = fs::temp_directory_path();
  REQUIRE(temp_dir);
  return *temp_dir
       / std::format("memix-{}-{}-{}", label, nanos, random_device());
}

class TemporaryDirectory {
public:
  explicit TemporaryDirectory(std::string_view label)
      : path_(unique_test_path(label)) {
    REQUIRE(fs::create_directories(path_));
  }

  ~TemporaryDirectory() {
    make_tree_writable();
    static_cast<void>(fs::remove_all(path_));
  }

  [[nodiscard]] const fs::path &path() const noexcept { return path_; }

private:
  void make_tree_writable() const noexcept {
    std::error_code error;
    std::filesystem::permissions(path_, std::filesystem::perms::owner_all,
                                 std::filesystem::perm_options::add, error);
    error.clear();
    const std::filesystem::recursive_directory_iterator end;
    std::filesystem::recursive_directory_iterator iterator{
        path_, std::filesystem::directory_options::skip_permission_denied,
        error};
    while (!error && iterator != end) {
      const std::filesystem::file_status status =
          iterator->symlink_status(error);
      if (!error && std::filesystem::is_directory(status)) {
        std::filesystem::permissions(iterator->path(),
                                     std::filesystem::perms::owner_all,
                                     std::filesystem::perm_options::add, error);
      }
      if (error) {
        error.clear();
      }
      iterator.increment(error);
    }
  }

  fs::path path_;
};

void write_file(const fs::path &path, std::string_view contents) {
  std::ofstream output(path, std::ios::binary);
  REQUIRE(output);
  output << contents;
  REQUIRE(output);
}

std::string read_file(const fs::path &path) {
  std::ifstream input(path, std::ios::binary);
  REQUIRE(input);
  return {std::istreambuf_iterator<char>(input),
          std::istreambuf_iterator<char>()};
}

std::string read_cli_text_file(const fs::path &path) {
  std::ifstream input(path);
  REQUIRE(input);
  return {std::istreambuf_iterator<char>(input),
          std::istreambuf_iterator<char>()};
}

bool path_exists(const fs::path &path) {
  const memix::FsResult<bool> exists = fs::exists(path);
  REQUIRE(exists);
  return *exists;
}

bool path_is_regular_file(const fs::path &path) {
  const memix::FsResult<bool> regular_file = fs::is_regular_file(path);
  REQUIRE(regular_file);
  return *regular_file;
}

fs::path object_dir_for(const memix::Store &store, memix::MemoryObjectType type,
                        std::string_view digest) {
  return store.root()
       / std::string(memix::to_string(type))
       / "blobs"
       / std::string(digest);
}

void make_object_mutable_for_corruption(const fs::path &object_dir) {
  REQUIRE(fs::permissions(object_dir, fs::perms::owner_all));
  REQUIRE(fs::permissions(object_dir / "object", fs::perms::owner_all));
  REQUIRE(fs::permissions(object_dir / "metadata.json", fs::perms::owner_all));
}

void make_directory_tree_writable(const fs::path &root) {
  REQUIRE(fs::permissions(root, fs::perms::owner_all));
  std::error_code iterator_error;
  const fs::recursive_directory_iterator end;
  for (fs::recursive_directory_iterator iterator{root, iterator_error};
       !iterator_error && iterator != end; iterator.increment(iterator_error)) {
    const fs::directory_entry &entry = *iterator;
    std::error_code entry_error;
    if (entry.is_directory(entry_error)) {
      REQUIRE(fs::permissions(entry.path(), fs::perms::owner_all));
    }
    REQUIRE_FALSE(entry_error);
  }
  REQUIRE_FALSE(iterator_error);
}

std::string shell_quote(std::string_view value) {
#ifdef _WIN32
  std::string quoted{"\""};
  for (const char character : value) {
    if (character == '"') {
      quoted += "\\\"";
    } else {
      quoted.push_back(character);
    }
  }
  quoted.push_back('"');
#else
  std::string quoted{"'"};
  for (const char character : value) {
    if (character == '\'') {
      quoted += "'\\''";
    } else {
      quoted.push_back(character);
    }
  }
  quoted.push_back('\'');
#endif
  return quoted;
}

struct CliResult {
  int status = 0;
  std::string output;
  std::string error;
};

CliResult run_cli(const fs::path &directory, const fs::path &store_root,
                  const std::vector<std::string> &arguments) {
  const fs::path output_path = directory / "store-cli-stdout.txt";
  const fs::path error_path = directory / "store-cli-stderr.txt";
  std::string_view binary = MEMIX_TEST_BINARY;
  if (const char *override_binary =
          std::getenv("MEMIX_TEST_BINARY_OVERRIDE");
      override_binary != nullptr && override_binary[0] != '\0') {
    binary = override_binary;
  }
  std::string command = shell_quote(binary);
  command += " store --store-root ";
  command += shell_quote(store_root.string());
  for (const std::string &argument : arguments) {
    command.push_back(' ');
    command += shell_quote(argument);
  }
  command += " >";
  command += shell_quote(output_path.string());
  command += " 2>";
  command += shell_quote(error_path.string());
#ifdef _WIN32
  command.insert(command.begin(), '"');
  command.push_back('"');
#endif
  const int status = std::system(command.c_str()); // NOLINT(cert-env33-c)
  return CliResult{
      .status = status,
      .output = read_cli_text_file(output_path),
      .error = read_cli_text_file(error_path),
  };
}

} // namespace

TEST_CASE("memory object type helpers accept only supported v1 types",
          "[store]") {
  REQUIRE(std::string(memix::to_string(memix::MemoryObjectType::Rules))
          == "rules");
  REQUIRE(std::string(memix::to_string(memix::MemoryObjectType::Chat))
          == "chat");
  REQUIRE(std::string(memix::to_string(memix::MemoryObjectType::Documents))
          == "documents");
  REQUIRE(memix::parse_memory_object_type("rules")
          == memix::MemoryObjectType::Rules);
  REQUIRE(memix::parse_memory_object_type("chat")
          == memix::MemoryObjectType::Chat);
  REQUIRE(memix::parse_memory_object_type("documents")
          == memix::MemoryObjectType::Documents);
  REQUIRE_FALSE(memix::parse_memory_object_type("pdf").has_value());
  REQUIRE_FALSE(memix::parse_memory_object_type("agents").has_value());
  REQUIRE_FALSE(memix::parse_memory_object_type("Rules").has_value());

  const std::vector expected_types{
      memix::MemoryObjectType::Rules,
      memix::MemoryObjectType::Chat,
      memix::MemoryObjectType::Documents,
  };
  REQUIRE(memix::memory_object_types() == expected_types);

  REQUIRE(memix::to_string(static_cast<memix::MemoryObjectType>(255)).empty());
}

TEST_CASE("context tree entry kind helpers accept only supported v1 kinds",
          "[store]") {
  REQUIRE(std::string(memix::to_string(memix::ContextTreeEntryKind::Blob))
          == "blob");
  REQUIRE(std::string(memix::to_string(memix::ContextTreeEntryKind::Tree))
          == "tree");
  REQUIRE(memix::parse_context_tree_entry_kind("blob")
          == memix::ContextTreeEntryKind::Blob);
  REQUIRE(memix::parse_context_tree_entry_kind("tree")
          == memix::ContextTreeEntryKind::Tree);
  REQUIRE_FALSE(memix::parse_context_tree_entry_kind("TREE").has_value());
  REQUIRE_FALSE(memix::parse_context_tree_entry_kind("directory").has_value());

  REQUIRE(
      memix::to_string(static_cast<memix::ContextTreeEntryKind>(255)).empty());
}

TEST_CASE("default store root follows MEMIX_STORE_HOME then XDG then HOME",
          "[store]") {
  EnvironmentGuard guard;
  set_environment_value("MEMIX_STORE_HOME", std::string("/tmp/memix-store"));
  set_environment_value("XDG_DATA_HOME", std::string("/tmp/xdg-data"));
  set_environment_value("HOME", std::string("/tmp/home"));
  auto root = memix::default_store_root();
  REQUIRE(root.has_value());
  REQUIRE(*root == fs::path("/tmp/memix-store"));

  set_environment_value("MEMIX_STORE_HOME", std::nullopt);
  root = memix::default_store_root();
  REQUIRE(root.has_value());
  REQUIRE(*root == fs::path("/tmp/xdg-data/memix"));

  set_environment_value("XDG_DATA_HOME", std::nullopt);
  root = memix::default_store_root();
  REQUIRE(root.has_value());
  REQUIRE(*root == fs::path("/tmp/home/.memix"));

  set_environment_value("HOME", std::nullopt);
  REQUIRE_FALSE(memix::default_store_root().has_value());
}

TEST_CASE("ingest stores a regular file as immutable metadata and content",
          "[store]") {
  TemporaryDirectory directory("ingest");
  const fs::path source = directory.path() / "document.md";
  write_file(source, "abc");

  const memix::Store store(directory.path() / "store");
  const auto result = store.ingest(memix::MemoryObjectType::Documents, source);
  REQUIRE(result.has_value());

  const std::string expected_hash =
      "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";
  REQUIRE(result->inserted);
  REQUIRE(result->object.id == expected_hash);
  REQUIRE(result->object.hash == expected_hash);
  REQUIRE(result->object.hash_algorithm == "sha256");
  REQUIRE(result->object.size_bytes == 3);
  REQUIRE(result->object.filename == "document.md");
  REQUIRE(result->object.content_path == fs::path("object"));

  const fs::path object_dir =
      object_dir_for(store, memix::MemoryObjectType::Documents, expected_hash);
  REQUIRE(path_is_regular_file(object_dir / "object"));
  REQUIRE(path_is_regular_file(object_dir / "metadata.json"));
  REQUIRE(read_file(object_dir / "object") == "abc");

  const std::string metadata = read_file(object_dir / "metadata.json");
  REQUIRE(metadata.find("\"schema_version\":1") != std::string::npos);
  REQUIRE(metadata.find("\"object_version\":1") != std::string::npos);
  REQUIRE(metadata.find("\"type\":\"documents\"") != std::string::npos);
  REQUIRE(metadata.find("\"filename\":\"document.md\"") != std::string::npos);
  REQUIRE(metadata.find(source.string()) == std::string::npos);
}

TEST_CASE("generic ingest stores rule blobs without a logical destination",
          "[store]") {
  TemporaryDirectory directory("rules-generic-blob-ingest");
  const fs::path source = directory.path() / "AGENTS.md";
  write_file(source, "rules");

  const memix::Store store(directory.path() / "store");
  const auto result = store.ingest(memix::MemoryObjectType::Rules, source);
  REQUIRE(result.has_value());

  REQUIRE(result->inserted);
  REQUIRE(result->object.type == memix::MemoryObjectType::Rules);
  REQUIRE(read_file(result->object.object_dir / result->object.content_path)
          == "rules");
}

TEST_CASE("ingest uses SHA-256 for empty files", "[store]") {
  TemporaryDirectory directory("empty");
  const fs::path source = directory.path() / "empty.json";
  write_file(source, "");

  const memix::Store store(directory.path() / "store");
  const auto result = store.ingest(memix::MemoryObjectType::Chat, source);
  REQUIRE(result.has_value());

  REQUIRE(result->object.id
          == "e3b0c44298fc1c149afbf4c8996fb924"
             "27ae41e4649b934ca495991b7852b855");
  REQUIRE(result->object.size_bytes == 0);
}

TEST_CASE("duplicate ingest in the same type returns the existing object",
          "[store]") {
  TemporaryDirectory directory("duplicate");
  const fs::path source = directory.path() / "chat.json";
  write_file(source, "{\"messages\":[]}");

  const memix::Store store(directory.path() / "store");
  const auto first = store.ingest(memix::MemoryObjectType::Chat, source);
  REQUIRE(first.has_value());
  const std::string first_metadata =
      read_file(first->object.object_dir / "metadata.json");
  const auto second = store.ingest(memix::MemoryObjectType::Chat, source);
  REQUIRE(second.has_value());
  const std::string second_metadata =
      read_file(second->object.object_dir / "metadata.json");

  REQUIRE(first->inserted);
  REQUIRE_FALSE(second->inserted);
  REQUIRE(first->object.id == second->object.id);
  REQUIRE(first_metadata == second_metadata);
  const auto chat_objects = store.list(memix::MemoryObjectType::Chat);
  REQUIRE(chat_objects.has_value());
  REQUIRE(chat_objects->size() == 1);
}

TEST_CASE("rule blob ingest stores duplicate content once", "[store]") {
  TemporaryDirectory directory("rule-blob-duplicate");
  const fs::path source = directory.path() / "AGENTS.md";
  write_file(source, "python rules");

  const memix::Store store(directory.path() / "store");
  const auto first = store.ingest_blob(memix::MemoryObjectType::Rules, source);
  const auto second = store.ingest_file({
      .type = memix::MemoryObjectType::Rules,
      .source_path = source,
      .context_path = "lang/python/AGENTS.md",
      .message = "add python",
  });
  REQUIRE(first.has_value());
  REQUIRE(second.has_value());

  REQUIRE(first->inserted);
  REQUIRE_FALSE(second->blob.inserted);
  REQUIRE(first->object.id == second->blob.object.id);
  REQUIRE(path_is_regular_file(
      object_dir_for(store, memix::MemoryObjectType::Rules, first->object.id)
      / "object"));

  const auto rules = store.list(memix::MemoryObjectType::Rules);
  REQUIRE(rules.has_value());
  REQUIRE(rules->size() == 1);
}

TEST_CASE("rule ingest creates a generation and latest ref", "[store]") {
  TemporaryDirectory directory("rule-ingest-first");
  const fs::path source = directory.path() / "AGENTS.md";
  write_file(source, "prefer uv\n");

  const memix::Store store(directory.path() / "store");
  const auto ingest = store.ingest_file({
      .type = memix::MemoryObjectType::Rules,
      .source_path = source,
      .context_path = "lang/python/AGENTS.md",
      .message = "add python",
  });
  REQUIRE(ingest.has_value());

  REQUIRE(ingest->context_path == fs::path("lang/python/AGENTS.md"));
  REQUIRE_FALSE(ingest->previous_generation.has_value());
  REQUIRE(path_is_regular_file(
      store.root() / "rules" / "blobs" / ingest->blob.object.id / "object"));
  REQUIRE(path_is_regular_file(
      store.root()
      / "rules"
      / "trees"
      / std::format("{}.json", ingest->generation.root_tree)));
  REQUIRE(
      path_is_regular_file(store.root()
                           / "rules"
                           / "generations"
                           / std::format("{}.json", ingest->generation.id)));

  const auto latest = store.read_ref("rules/latest");
  REQUIRE(latest.has_value());
  REQUIRE(latest->has_value());
  REQUIRE(**latest == ingest->generation.id);
  REQUIRE(path_is_regular_file(store.root() / "refs" / "rules" / "latest"));
  REQUIRE(read_file(store.root() / "refs" / "rules" / "latest")
          == std::format("{}\n", ingest->generation.id));
  const auto first_generation_ref = store.read_ref("rules/generations/000001");
  REQUIRE(first_generation_ref.has_value());
  REQUIRE(first_generation_ref->has_value());
  REQUIRE(**first_generation_ref == ingest->generation.id);
  REQUIRE(path_is_regular_file(
      store.root() / "refs" / "rules" / "generations" / "000001"));

  const auto file =
      store.read_file(memix::MemoryObjectType::Rules, "lang/python/AGENTS.md");
  REQUIRE(file.has_value());
  REQUIRE(read_file(file->object_dir / file->content_path) == "prefer uv\n");

  const auto files = store.list_files(memix::MemoryObjectType::Rules);
  REQUIRE(files.has_value());
  REQUIRE(files->size() == 1);
  REQUIRE((*files)[0].context_path == fs::path("lang/python/AGENTS.md"));
}

TEST_CASE("rule generations preserve old trees after later ingests",
          "[store]") {
  TemporaryDirectory directory("rule-ingest-generations");
  const fs::path python_source = directory.path() / "python.md";
  const fs::path vllm_source = directory.path() / "vllm.md";
  write_file(python_source, "python base\n");
  write_file(vllm_source, "vllm overlay\n");

  const memix::Store store(directory.path() / "store");
  const auto first = store.ingest_file({
      .type = memix::MemoryObjectType::Rules,
      .source_path = python_source,
      .context_path = "lang/python/AGENTS.md",
      .message = "add python",
  });
  REQUIRE(first.has_value());
  const std::string first_generation = first->generation.id;

  const auto second = store.ingest_file({
      .type = memix::MemoryObjectType::Rules,
      .source_path = vllm_source,
      .context_path = "lang/python/vllm/AGENTS.md",
      .message = "add vllm",
  });
  REQUIRE(second.has_value());
  REQUIRE(second->previous_generation == first_generation);
  REQUIRE(second->generation.parent == first_generation);
  const auto second_generation_ref = store.read_ref("rules/generations/000002");
  REQUIRE(second_generation_ref.has_value());
  REQUIRE(second_generation_ref->has_value());
  REQUIRE(**second_generation_ref == second->generation.id);

  const auto old_python =
      store.read_file(memix::MemoryObjectType::Rules, "lang/python/AGENTS.md",
                      std::optional<std::string_view>{first_generation});
  REQUIRE(old_python.has_value());
  REQUIRE(read_file(old_python->object_dir / old_python->content_path)
          == "python base\n");
  const auto missing_old_vllm = store.read_file(
      memix::MemoryObjectType::Rules, "lang/python/vllm/AGENTS.md",
      std::optional<std::string_view>{first_generation});
  REQUIRE_FALSE(missing_old_vllm.has_value());
  REQUIRE(missing_old_vllm.error().kind
          == memix::ErrorKind::ContextPathNotFound);

  const auto latest_files = store.list_files(memix::MemoryObjectType::Rules);
  REQUIRE(latest_files.has_value());
  REQUIRE(latest_files->size() == 2);
  REQUIRE((*latest_files)[0].context_path == fs::path("lang/python/AGENTS.md"));
  REQUIRE((*latest_files)[1].context_path
          == fs::path("lang/python/vllm/AGENTS.md"));
}

TEST_CASE("replacing a rule file leaves prior tree readable", "[store]") {
  TemporaryDirectory directory("rule-replace");
  const fs::path first_source = directory.path() / "first.md";
  const fs::path second_source = directory.path() / "second.md";
  write_file(first_source, "pip workflow\n");
  write_file(second_source, "uv workflow\n");

  const memix::Store store(directory.path() / "store");
  const auto first = store.ingest_file({
      .type = memix::MemoryObjectType::Rules,
      .source_path = first_source,
      .context_path = "lang/python/AGENTS.md",
      .message = "add python",
  });
  REQUIRE(first.has_value());
  const auto second = store.ingest_file({
      .type = memix::MemoryObjectType::Rules,
      .source_path = second_source,
      .context_path = "lang/python/AGENTS.md",
      .message = "replace python",
  });
  REQUIRE(second.has_value());

  const auto old_file =
      store.read_file(memix::MemoryObjectType::Rules, "lang/python/AGENTS.md",
                      std::optional<std::string_view>{first->generation.id});
  const auto latest_file =
      store.read_file(memix::MemoryObjectType::Rules, "lang/python/AGENTS.md");
  REQUIRE(old_file.has_value());
  REQUIRE(latest_file.has_value());
  REQUIRE(read_file(old_file->object_dir / old_file->content_path)
          == "pip workflow\n");
  REQUIRE(read_file(latest_file->object_dir / latest_file->content_path)
          == "uv workflow\n");
  REQUIRE(path_exists(object_dir_for(store, memix::MemoryObjectType::Rules,
                                     first->blob.object.id)));
}

TEST_CASE("removing a rule file creates a generation and preserves history",
          "[store]") {
  TemporaryDirectory directory("rule-remove-file");
  const fs::path python_source = directory.path() / "python.md";
  const fs::path rust_source = directory.path() / "rust.md";
  write_file(python_source, "python rules\n");
  write_file(rust_source, "rust rules\n");

  const memix::Store store(directory.path() / "store");
  const auto first = store.ingest_file({
      .type = memix::MemoryObjectType::Rules,
      .source_path = python_source,
      .context_path = "lang/python/AGENTS.md",
      .message = "add python",
  });
  REQUIRE(first.has_value());
  const auto second = store.ingest_file({
      .type = memix::MemoryObjectType::Rules,
      .source_path = rust_source,
      .context_path = "lang/rust/AGENTS.md",
      .message = "add rust",
  });
  REQUIRE(second.has_value());

  const auto removed = store.remove_file({
      .type = memix::MemoryObjectType::Rules,
      .context_path = "lang/python/AGENTS.md",
      .message = {},
  });
  REQUIRE(removed.has_value());
  REQUIRE(removed->type == memix::MemoryObjectType::Rules);
  REQUIRE(removed->context_path == fs::path("lang/python/AGENTS.md"));
  REQUIRE(removed->previous_generation == second->generation.id);
  REQUIRE(removed->generation.parent == second->generation.id);
  REQUIRE(removed->generation.message == "remove lang/python/AGENTS.md");
  REQUIRE_FALSE(removed->projection_warning.has_value());

  const auto latest = store.read_ref("rules/latest");
  REQUIRE(latest.has_value());
  REQUIRE(latest->has_value());
  REQUIRE(**latest == removed->generation.id);
  const auto third_generation_ref = store.read_ref("rules/generations/000003");
  REQUIRE(third_generation_ref.has_value());
  REQUIRE(third_generation_ref->has_value());
  REQUIRE(**third_generation_ref == removed->generation.id);

  const auto missing =
      store.read_file(memix::MemoryObjectType::Rules, "lang/python/AGENTS.md");
  REQUIRE_FALSE(missing.has_value());
  REQUIRE(missing.error().kind == memix::ErrorKind::ContextPathNotFound);

  const auto files = store.list_files(memix::MemoryObjectType::Rules);
  REQUIRE(files.has_value());
  REQUIRE(files->size() == 1);
  REQUIRE((*files)[0].context_path == fs::path("lang/rust/AGENTS.md"));

  const auto old_file =
      store.read_file(memix::MemoryObjectType::Rules, "lang/python/AGENTS.md",
                      std::optional<std::string_view>{second->generation.id});
  REQUIRE(old_file.has_value());
  REQUIRE(read_file(old_file->object_dir / old_file->content_path)
          == "python rules\n");
  REQUIRE(path_exists(object_dir_for(store, memix::MemoryObjectType::Rules,
                                     first->blob.object.id)));
}

TEST_CASE("removing the last file prunes empty trees down to an empty root",
          "[store]") {
  TemporaryDirectory directory("rule-remove-last");
  const fs::path source = directory.path() / "AGENTS.md";
  write_file(source, "only rules\n");

  const memix::Store store(directory.path() / "store");
  const auto ingest = store.ingest_file({
      .type = memix::MemoryObjectType::Rules,
      .source_path = source,
      .context_path = "lang/python/AGENTS.md",
      .message = "add python",
  });
  REQUIRE(ingest.has_value());

  const auto removed = store.remove_file({
      .type = memix::MemoryObjectType::Rules,
      .context_path = "lang/python/AGENTS.md",
      .message = "drop python",
  });
  REQUIRE(removed.has_value());
  REQUIRE(removed->generation.message == "drop python");

  const auto root_tree = store.read_tree(memix::MemoryObjectType::Rules,
                                         removed->generation.root_tree);
  REQUIRE(root_tree.has_value());
  REQUIRE(root_tree->entries.empty());

  const auto files = store.list_files(memix::MemoryObjectType::Rules);
  REQUIRE(files.has_value());
  REQUIRE(files->empty());
  const auto missing_lang =
      store.read_file(memix::MemoryObjectType::Rules, "lang/python/AGENTS.md");
  REQUIRE_FALSE(missing_lang.has_value());
  REQUIRE(missing_lang.error().kind == memix::ErrorKind::ContextPathNotFound);
}

TEST_CASE("removing a directory removes its subtree recursively", "[store]") {
  TemporaryDirectory directory("document-remove-tree");
  const fs::path design = directory.path() / "design.md";
  const fs::path decisions = directory.path() / "decisions.md";
  const fs::path notes = directory.path() / "notes.md";
  write_file(design, "design\n");
  write_file(decisions, "decisions\n");
  write_file(notes, "notes\n");

  const memix::Store store(directory.path() / "store");
  REQUIRE(store
              .ingest_file({
                  .type = memix::MemoryObjectType::Documents,
                  .source_path = design,
                  .context_path = "repo/design.md",
                  .message = {},
              })
              .has_value());
  REQUIRE(store
              .ingest_file({
                  .type = memix::MemoryObjectType::Documents,
                  .source_path = decisions,
                  .context_path = "repo/arch/decisions.md",
                  .message = {},
              })
              .has_value());
  const auto keeper = store.ingest_file({
      .type = memix::MemoryObjectType::Documents,
      .source_path = notes,
      .context_path = "notes.md",
      .message = {},
  });
  REQUIRE(keeper.has_value());

  const auto removed = store.remove_file({
      .type = memix::MemoryObjectType::Documents,
      .context_path = "repo",
      .message = {},
  });
  REQUIRE(removed.has_value());
  REQUIRE(removed->previous_generation == keeper->generation.id);

  const auto files = store.list_files(memix::MemoryObjectType::Documents);
  REQUIRE(files.has_value());
  REQUIRE(files->size() == 1);
  REQUIRE((*files)[0].context_path == fs::path("notes.md"));
  const auto missing_dir =
      store.read_file(memix::MemoryObjectType::Documents, "repo/design.md");
  REQUIRE_FALSE(missing_dir.has_value());
  REQUIRE(missing_dir.error().kind == memix::ErrorKind::ContextPathNotFound);

  const auto old_files =
      store.list_files(memix::MemoryObjectType::Documents,
                       std::optional<std::string_view>{keeper->generation.id});
  REQUIRE(old_files.has_value());
  REQUIRE(old_files->size() == 3);

  REQUIRE_FALSE(path_exists(store.root() / "latest" / "documents" / "repo"));
  REQUIRE(read_file(store.root() / "latest" / "documents" / "notes.md")
          == "notes\n");
}

TEST_CASE("removing a file keeps the projection in sync", "[store][expose]") {
  TemporaryDirectory directory("latest-projection-remove");
  const fs::path nested_source = directory.path() / "nested.md";
  const fs::path carried_source = directory.path() / "carried.md";
  write_file(nested_source, "nested memory\n");
  write_file(carried_source, "carried memory\n");

  const memix::Store store(directory.path() / "store");
  REQUIRE(store
              .ingest_file({
                  .type = memix::MemoryObjectType::Rules,
                  .source_path = nested_source,
                  .context_path = "repo/nested/AGENTS.md",
                  .message = {},
              })
              .has_value());
  REQUIRE(store
              .ingest_file({
                  .type = memix::MemoryObjectType::Rules,
                  .source_path = carried_source,
                  .context_path = "repo/carried.md",
                  .message = {},
              })
              .has_value());

  const fs::path projected_nested =
      store.root() / "latest" / "rules" / "repo" / "nested" / "AGENTS.md";
  REQUIRE(path_is_regular_file(projected_nested));

  const auto removed = store.remove_file({
      .type = memix::MemoryObjectType::Rules,
      .context_path = "repo/nested/AGENTS.md",
      .message = {},
  });
  REQUIRE(removed.has_value());
  REQUIRE_FALSE(removed->projection_warning.has_value());

  REQUIRE_FALSE(path_exists(projected_nested));
  REQUIRE_FALSE(path_exists(store.root() / "latest" / "rules" / "repo"
                            / "nested"));
  REQUIRE(read_file(store.root() / "latest" / "rules" / "repo" / "carried.md")
          == "carried memory\n");

  const auto report = store.expose();
  REQUIRE(report.has_value());
  REQUIRE(read_file(store.root() / "latest" / "rules" / "repo" / "carried.md")
          == "carried memory\n");
}

TEST_CASE("remove rejects missing paths and empty histories", "[store]") {
  TemporaryDirectory directory("rule-remove-missing");
  const fs::path source = directory.path() / "AGENTS.md";
  write_file(source, "rules\n");

  const memix::Store store(directory.path() / "store");

  const auto empty_store = store.remove_file({
      .type = memix::MemoryObjectType::Rules,
      .context_path = "a.md",
      .message = {},
  });
  REQUIRE_FALSE(empty_store.has_value());
  REQUIRE(empty_store.error().kind == memix::ErrorKind::ContextPathNotFound);

  const auto ingest = store.ingest_file({
      .type = memix::MemoryObjectType::Rules,
      .source_path = source,
      .context_path = "a.md",
      .message = {},
  });
  REQUIRE(ingest.has_value());

  const auto missing = store.remove_file({
      .type = memix::MemoryObjectType::Rules,
      .context_path = "b.md",
      .message = {},
  });
  REQUIRE_FALSE(missing.has_value());
  REQUIRE(missing.error().kind == memix::ErrorKind::ContextPathNotFound);

  const auto through_file = store.remove_file({
      .type = memix::MemoryObjectType::Rules,
      .context_path = "a.md/b.md",
      .message = {},
  });
  REQUIRE_FALSE(through_file.has_value());
  REQUIRE(through_file.error().kind == memix::ErrorKind::InvalidArgument);

  const auto latest = store.read_ref("rules/latest");
  REQUIRE(latest.has_value());
  REQUIRE(latest->has_value());
  REQUIRE(**latest == ingest->generation.id);
  const auto second_generation_ref = store.read_ref("rules/generations/000002");
  REQUIRE(second_generation_ref.has_value());
  REQUIRE_FALSE(second_generation_ref->has_value());
  const auto files = store.list_files(memix::MemoryObjectType::Rules);
  REQUIRE(files.has_value());
  REQUIRE(files->size() == 1);
}

TEST_CASE("remove rejects unsafe paths", "[store]") {
  TemporaryDirectory directory("rule-remove-invalid-paths");
  const fs::path source = directory.path() / "AGENTS.md";
  write_file(source, "rules\n");

  const memix::Store store(directory.path() / "store");
  REQUIRE(store
              .ingest_file({
                  .type = memix::MemoryObjectType::Rules,
                  .source_path = source,
                  .context_path = "a.md",
                  .message = {},
              })
              .has_value());

  const auto expect_invalid = [&store](const fs::path &path) {
    const auto result = store.remove_file({
        .type = memix::MemoryObjectType::Rules,
        .context_path = path,
        .message = {},
    });
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().kind == memix::ErrorKind::InvalidArgument);
  };
  expect_invalid("");
  expect_invalid("/abs/AGENTS.md");
  expect_invalid("lang/../python/AGENTS.md");
  expect_invalid("lang/python/");
  expect_invalid("lang\\python\\AGENTS.md");

  const auto files = store.list_files(memix::MemoryObjectType::Rules);
  REQUIRE(files.has_value());
  REQUIRE(files->size() == 1);
}

TEST_CASE("documents can be ingested into a canonical context tree",
          "[store]") {
  TemporaryDirectory directory("document-context-ingest");
  const fs::path source = directory.path() / "design.md";
  write_file(source, "store design\n");

  const memix::Store store(directory.path() / "store");
  const auto ingest = store.ingest_file({
      .type = memix::MemoryObjectType::Documents,
      .source_path = source,
      .context_path = "notes/design.md",
      .message = "add design",
  });
  REQUIRE(ingest.has_value());

  REQUIRE(ingest->type == memix::MemoryObjectType::Documents);
  REQUIRE(ingest->context_path == fs::path("notes/design.md"));
  REQUIRE_FALSE(ingest->previous_generation.has_value());
  REQUIRE(path_is_regular_file(store.root()
                               / "documents"
                               / "blobs"
                               / ingest->blob.object.id
                               / "object"));
  REQUIRE(path_is_regular_file(
      store.root()
      / "documents"
      / "trees"
      / std::format("{}.json", ingest->generation.root_tree)));
  REQUIRE(
      path_is_regular_file(store.root()
                           / "documents"
                           / "generations"
                           / std::format("{}.json", ingest->generation.id)));

  const auto latest = store.read_ref("documents/latest");
  REQUIRE(latest.has_value());
  REQUIRE(latest->has_value());
  REQUIRE(**latest == ingest->generation.id);
  const auto first_generation_ref =
      store.read_ref("documents/generations/000001");
  REQUIRE(first_generation_ref.has_value());
  REQUIRE(first_generation_ref->has_value());
  REQUIRE(**first_generation_ref == ingest->generation.id);

  const auto file =
      store.read_file(memix::MemoryObjectType::Documents, "notes/design.md");
  REQUIRE(file.has_value());
  REQUIRE(read_file(file->object_dir / file->content_path) == "store design\n");

  const auto files = store.list_files(memix::MemoryObjectType::Documents);
  REQUIRE(files.has_value());
  REQUIRE(files->size() == 1);
  REQUIRE((*files)[0].type == memix::MemoryObjectType::Documents);
  REQUIRE((*files)[0].context_path == fs::path("notes/design.md"));
}

TEST_CASE("store catalog explores types, subtrees, depth, counts, and bytes",
          "[store]") {
  TemporaryDirectory directory("store-catalog");
  const fs::path design = directory.path() / "design.md";
  const fs::path decisions = directory.path() / "decisions.md";
  const fs::path rule = directory.path() / "AGENTS.md";
  write_file(design, "design\n");
  write_file(decisions, "decisions\n");
  write_file(rule, "rule\n");

  const memix::Store store(directory.path() / "store");
  REQUIRE(store
              .ingest_file({
                  .type = memix::MemoryObjectType::Documents,
                  .source_path = design,
                  .context_path = "notes/design.md",
                  .message = {},
              })
              .has_value());
  REQUIRE(store
              .ingest_file({
                  .type = memix::MemoryObjectType::Documents,
                  .source_path = decisions,
                  .context_path = "notes/project/decisions.md",
                  .message = {},
              })
              .has_value());
  REQUIRE(store
              .ingest_file({
                  .type = memix::MemoryObjectType::Rules,
                  .source_path = rule,
                  .context_path = "repo/AGENTS.md",
                  .message = {},
              })
              .has_value());

  const auto all = store.catalog();
  REQUIRE(all.has_value());
  bool saw_documents_root = false;
  bool saw_rules_root = false;
  for (const memix::StoreCatalogEntry &entry : *all) {
    if (entry.directory
        && entry.context_path.empty()
        && entry.type == memix::MemoryObjectType::Documents) {
      saw_documents_root = true;
      REQUIRE(entry.file_count == 2);
      REQUIRE(entry.total_size_bytes == 17);
    }
    if (entry.directory
        && entry.context_path.empty()
        && entry.type == memix::MemoryObjectType::Rules) {
      saw_rules_root = true;
      REQUIRE(entry.file_count == 1);
      REQUIRE(entry.total_size_bytes == 5);
    }
  }
  REQUIRE(saw_documents_root);
  REQUIRE(saw_rules_root);

  const auto notes = store.catalog(memix::StoreCatalogOptions{
      .type = memix::MemoryObjectType::Documents,
      .path = "notes",
      .depth = 1,
      .generation = std::nullopt,
  });
  REQUIRE(notes.has_value());
  REQUIRE(notes->size() == 3);
  REQUIRE((*notes)[0].context_path == fs::path("notes"));
  REQUIRE((*notes)[0].directory);
  REQUIRE((*notes)[0].file_count == 2);
  REQUIRE((*notes)[1].context_path == fs::path("notes/design.md"));
  REQUIRE_FALSE((*notes)[1].directory);
  REQUIRE((*notes)[2].context_path == fs::path("notes/project"));
  REQUIRE((*notes)[2].directory);
  REQUIRE((*notes)[2].file_count == 1);
}

TEST_CASE("document context generations preserve old paths after replacement",
          "[store]") {
  TemporaryDirectory directory("document-context-generations");
  const fs::path first_source = directory.path() / "first.md";
  const fs::path second_source = directory.path() / "second.md";
  write_file(first_source, "initial design\n");
  write_file(second_source, "revised design\n");

  const memix::Store store(directory.path() / "store");
  const auto first = store.ingest_file({
      .type = memix::MemoryObjectType::Documents,
      .source_path = first_source,
      .context_path = "notes/design.md",
      .message = "add design",
  });
  REQUIRE(first.has_value());
  const auto second = store.ingest_file({
      .type = memix::MemoryObjectType::Documents,
      .source_path = second_source,
      .context_path = "notes/design.md",
      .message = "replace design",
  });
  REQUIRE(second.has_value());
  REQUIRE(second->previous_generation == first->generation.id);
  REQUIRE(second->generation.parent == first->generation.id);

  const auto old_file =
      store.read_file(memix::MemoryObjectType::Documents, "notes/design.md",
                      std::optional<std::string_view>{first->generation.id});
  const auto latest_file =
      store.read_file(memix::MemoryObjectType::Documents, "notes/design.md");
  REQUIRE(old_file.has_value());
  REQUIRE(latest_file.has_value());
  REQUIRE(read_file(old_file->object_dir / old_file->content_path)
          == "initial design\n");
  REQUIRE(read_file(latest_file->object_dir / latest_file->content_path)
          == "revised design\n");
}

TEST_CASE("chat context trees use independent refs and logical paths",
          "[store]") {
  TemporaryDirectory directory("chat-context-ingest");
  const fs::path source = directory.path() / "session.json";
  write_file(source, "{\"messages\":[]}\n");

  const memix::Store store(directory.path() / "store");
  const auto ingest = store.ingest_file({
      .type = memix::MemoryObjectType::Chat,
      .source_path = source,
      .context_path = "sessions/root.json",
      .message = "add session",
  });
  REQUIRE(ingest.has_value());

  const auto chat_latest = store.read_ref("chat/latest");
  REQUIRE(chat_latest.has_value());
  REQUIRE(chat_latest->has_value());
  REQUIRE(**chat_latest == ingest->generation.id);
  const auto rules_latest = store.read_ref("rules/latest");
  REQUIRE(rules_latest.has_value());
  REQUIRE_FALSE(rules_latest->has_value());

  const auto files = store.list_files(memix::MemoryObjectType::Chat);
  REQUIRE(files.has_value());
  REQUIRE(files->size() == 1);
  REQUIRE((*files)[0].context_path == fs::path("sessions/root.json"));
}

TEST_CASE("rule ingest rejects replacing a tree with a file", "[store]") {
  TemporaryDirectory directory("rule-tree-file-conflict");
  const fs::path nested_source = directory.path() / "nested.md";
  const fs::path root_source = directory.path() / "root.md";
  write_file(nested_source, "nested rules\n");
  write_file(root_source, "root rules\n");

  const memix::Store store(directory.path() / "store");
  const auto first = store.ingest_file({
      .type = memix::MemoryObjectType::Rules,
      .source_path = nested_source,
      .context_path = "a/b.md",
      .message = "add nested",
  });
  REQUIRE(first.has_value());

  const auto replacement = store.ingest_file({
      .type = memix::MemoryObjectType::Rules,
      .source_path = root_source,
      .context_path = "a",
      .message = "replace tree",
  });
  REQUIRE_FALSE(replacement.has_value());

  const auto latest = store.read_ref("rules/latest");
  REQUIRE(latest.has_value());
  REQUIRE(latest->has_value());
  REQUIRE(**latest == first->generation.id);
  const auto second_generation_ref = store.read_ref("rules/generations/000002");
  REQUIRE(second_generation_ref.has_value());
  REQUIRE_FALSE(second_generation_ref->has_value());

  const auto nested_file =
      store.read_file(memix::MemoryObjectType::Rules, "a/b.md");
  REQUIRE(nested_file.has_value());
  REQUIRE(read_file(nested_file->object_dir / nested_file->content_path)
          == "nested rules\n");
  REQUIRE_FALSE(
      store.read_file(memix::MemoryObjectType::Rules, "a").has_value());
  const auto files = store.list_files(memix::MemoryObjectType::Rules);
  REQUIRE(files.has_value());
  REQUIRE(files->size() == 1);
  REQUIRE((*files)[0].context_path == fs::path("a/b.md"));
}

TEST_CASE("rule paths reject unsafe components", "[store]") {
  TemporaryDirectory directory("rule-invalid-paths");
  const fs::path source = directory.path() / "AGENTS.md";
  write_file(source, "rules");
  const memix::Store store(directory.path() / "store");

  const auto empty_path = store.ingest_file({
      .type = memix::MemoryObjectType::Rules,
      .source_path = source,
      .context_path = "",
      .message = "bad",
  });
  REQUIRE_FALSE(empty_path.has_value());
  REQUIRE(empty_path.error().kind == memix::ErrorKind::InvalidArgument);
  REQUIRE_FALSE(store
                    .ingest_file({
                        .type = memix::MemoryObjectType::Rules,
                        .source_path = source,
                        .context_path = "/abs/AGENTS.md",
                        .message = "bad",
                    })
                    .has_value());
  REQUIRE_FALSE(store
                    .ingest_file({
                        .type = memix::MemoryObjectType::Rules,
                        .source_path = source,
                        .context_path = "lang/../python/AGENTS.md",
                        .message = "bad",
                    })
                    .has_value());
  REQUIRE_FALSE(store
                    .ingest_file({
                        .type = memix::MemoryObjectType::Rules,
                        .source_path = source,
                        .context_path = "lang/python/",
                        .message = "bad",
                    })
                    .has_value());
  REQUIRE_FALSE(store
                    .ingest_file({
                        .type = memix::MemoryObjectType::Rules,
                        .source_path = source,
                        .context_path = "lang\\python\\AGENTS.md",
                        .message = "bad",
                    })
                    .has_value());
}

TEST_CASE("ref updates fail on expected old mismatches", "[store]") {
  TemporaryDirectory directory("rule-ref-mismatch");
  const fs::path first_source = directory.path() / "first.md";
  const fs::path second_source = directory.path() / "second.md";
  write_file(first_source, "first");
  write_file(second_source, "second");

  const memix::Store store(directory.path() / "store");
  const auto first = store.ingest_file({
      .type = memix::MemoryObjectType::Rules,
      .source_path = first_source,
      .context_path = "one.md",
      .message = "one",
  });
  const auto second = store.ingest_file({
      .type = memix::MemoryObjectType::Rules,
      .source_path = second_source,
      .context_path = "two.md",
      .message = "two",
  });
  REQUIRE(first.has_value());
  REQUIRE(second.has_value());

  const memix::StoreVoid result = store.update_ref(
      "rules/latest", std::optional<std::string_view>{first->generation.id},
      second->generation.id);
  REQUIRE_FALSE(result.has_value());

  REQUIRE(fs::create_directory(store.root() / "refs" / "rules" / "latest.lock")
              .value_or(false));
  const memix::StoreVoid locked_result = store.update_ref(
      "rules/latest", std::optional<std::string_view>{second->generation.id},
      first->generation.id);
  REQUIRE_FALSE(locked_result.has_value());
  const auto latest = store.read_ref("rules/latest");
  REQUIRE(latest.has_value());
  REQUIRE(latest->has_value());
  REQUIRE(**latest == second->generation.id);
}

TEST_CASE("failed latest update rolls back numbered generation ref",
          "[store]") {
  TemporaryDirectory directory("rule-generation-ref-rollback");
  const fs::path first_source = directory.path() / "first.md";
  const fs::path second_source = directory.path() / "second.md";
  write_file(first_source, "first");
  write_file(second_source, "second");

  const memix::Store store(directory.path() / "store");
  const auto first = store.ingest_file({
      .type = memix::MemoryObjectType::Rules,
      .source_path = first_source,
      .context_path = "rules.md",
      .message = "first",
  });
  REQUIRE(first.has_value());

  REQUIRE(fs::create_directory(store.root() / "refs" / "rules" / "latest.lock")
              .value_or(false));
  const auto second = store.ingest_file({
      .type = memix::MemoryObjectType::Rules,
      .source_path = second_source,
      .context_path = "rules.md",
      .message = "second",
  });
  REQUIRE_FALSE(second.has_value());

  const auto latest = store.read_ref("rules/latest");
  REQUIRE(latest.has_value());
  REQUIRE(latest->has_value());
  REQUIRE(**latest == first->generation.id);

  const auto second_generation_ref = store.read_ref("rules/generations/000002");
  REQUIRE(second_generation_ref.has_value());
  REQUIRE_FALSE(second_generation_ref->has_value());
}

TEST_CASE("the same content can exist independently across object types",
          "[store]") {
  TemporaryDirectory directory("cross-type");
  const fs::path source = directory.path() / "memory.txt";
  write_file(source, "shared bytes");

  const memix::Store store(directory.path() / "store");
  const auto rules = store.ingest_file({
      .type = memix::MemoryObjectType::Rules,
      .source_path = source,
      .context_path = "memory.txt",
      .message = "add memory",
  });
  const auto documents =
      store.ingest(memix::MemoryObjectType::Documents, source);
  REQUIRE(rules.has_value());
  REQUIRE(documents.has_value());

  REQUIRE(rules->blob.inserted);
  REQUIRE(documents->inserted);
  REQUIRE(rules->blob.object.id == documents->object.id);
  REQUIRE(path_exists(object_dir_for(store, memix::MemoryObjectType::Rules,
                                     rules->blob.object.id)));
  REQUIRE(path_exists(object_dir_for(store, memix::MemoryObjectType::Documents,
                                     documents->object.id)));
  const auto objects = store.list();
  REQUIRE(objects.has_value());
  REQUIRE(objects->size() == 2);
}

TEST_CASE("list supports deterministic ordering and type filters", "[store]") {
  TemporaryDirectory directory("list");
  const fs::path rules_source = directory.path() / "rules.md";
  const fs::path chat_source = directory.path() / "chat.json";
  write_file(rules_source, "rules");
  write_file(chat_source, "chat");

  const memix::Store store(directory.path() / "store");
  const auto chat = store.ingest(memix::MemoryObjectType::Chat, chat_source);
  const auto rules = store.ingest_file({
      .type = memix::MemoryObjectType::Rules,
      .source_path = rules_source,
      .context_path = "rules.md",
      .message = "add rules",
  });
  REQUIRE(chat.has_value());
  REQUIRE(rules.has_value());

  const auto all = store.list();
  REQUIRE(all.has_value());
  REQUIRE(all->size() == 2);
  REQUIRE((*all)[0].type == memix::MemoryObjectType::Chat);
  REQUIRE((*all)[0].id == chat->object.id);
  REQUIRE((*all)[1].type == memix::MemoryObjectType::Rules);
  REQUIRE((*all)[1].id == rules->blob.object.id);

  const auto chat_only = store.list(memix::MemoryObjectType::Chat);
  REQUIRE(chat_only.has_value());
  REQUIRE(chat_only->size() == 1);
  REQUIRE((*chat_only)[0].type == memix::MemoryObjectType::Chat);
  REQUIRE((*chat_only)[0].id == chat->object.id);
}

TEST_CASE("ingest rejects directories", "[store]") {
  TemporaryDirectory directory("directory-source");
  const memix::Store store(directory.path() / "store");

  const auto result =
      store.ingest(memix::MemoryObjectType::Documents, directory.path());
  REQUIRE_FALSE(result.has_value());
}

TEST_CASE("list fails loudly when content is missing", "[store]") {
  TemporaryDirectory directory("missing-content");
  const fs::path source = directory.path() / "doc.html";
  write_file(source, "<p>hello</p>");

  const memix::Store store(directory.path() / "store");
  const auto result = store.ingest(memix::MemoryObjectType::Documents, source);
  REQUIRE(result.has_value());
  make_object_mutable_for_corruption(result->object.object_dir);
  REQUIRE(fs::remove(result->object.object_dir / "object").value_or(false));

  REQUIRE_FALSE(store.list(memix::MemoryObjectType::Documents).has_value());
}

TEST_CASE("list fails loudly when content hash does not match metadata",
          "[store]") {
  TemporaryDirectory directory("mismatched-content-hash");
  const fs::path source = directory.path() / "doc.html";
  write_file(source, "abc");

  const memix::Store store(directory.path() / "store");
  const auto result = store.ingest(memix::MemoryObjectType::Documents, source);
  REQUIRE(result.has_value());
  make_object_mutable_for_corruption(result->object.object_dir);
  write_file(result->object.object_dir / "object", "xyz");

  REQUIRE_FALSE(store.list(memix::MemoryObjectType::Documents).has_value());
}

TEST_CASE("list fails loudly when metadata is invalid", "[store]") {
  TemporaryDirectory directory("invalid-metadata");
  const fs::path source = directory.path() / "doc.html";
  write_file(source, "<p>hello</p>");

  const memix::Store store(directory.path() / "store");
  const auto result = store.ingest(memix::MemoryObjectType::Documents, source);
  REQUIRE(result.has_value());
  make_object_mutable_for_corruption(result->object.object_dir);
  write_file(result->object.object_dir / "metadata.json", "{not json");

  REQUIRE_FALSE(store.list(memix::MemoryObjectType::Documents).has_value());
}

TEST_CASE("context ingests maintain a read-only hardlink projection",
          "[store][expose]") {
  TemporaryDirectory directory("latest-projection");
  const fs::path first_source = directory.path() / "first.md";
  const fs::path carried_source = directory.path() / "carried.md";
  const fs::path replacement_source = directory.path() / "replacement.md";
  const fs::path blob_only_source = directory.path() / "untracked.md";
  write_file(first_source, "first memory\n");
  write_file(carried_source, "carried memory\n");
  write_file(replacement_source, "updated memory\n");
  write_file(blob_only_source, "blob only\n");

  const memix::Store store(directory.path() / "store");
  const auto first = store.ingest_file({
      .type = memix::MemoryObjectType::Rules,
      .source_path = first_source,
      .context_path = "repo/nested/AGENTS.md",
      .message = "first",
  });
  REQUIRE(first);
  REQUIRE_FALSE(first->projection_warning.has_value());
  const fs::path projected =
      store.root() / "latest" / "rules" / "repo" / "nested" / "AGENTS.md";
  REQUIRE(path_is_regular_file(projected));
  REQUIRE(read_file(projected) == "first memory\n");

  const memix::FsResult<fs::file_status> projected_status =
      fs::status(projected);
  REQUIRE(projected_status);
  REQUIRE((projected_status->permissions() & fs::perms::owner_write)
          == fs::perms::none);
  std::error_code hard_link_error;
  const std::uintmax_t hard_link_count =
      std::filesystem::hard_link_count(projected, hard_link_error);
  REQUIRE_FALSE(hard_link_error);
  REQUIRE(hard_link_count >= 2);

  const auto carried = store.ingest_file({
      .type = memix::MemoryObjectType::Rules,
      .source_path = carried_source,
      .context_path = "repo/carried.md",
      .message = "carried",
  });
  REQUIRE(carried);
  REQUIRE_FALSE(carried->projection_warning.has_value());
  REQUIRE(read_file(projected) == "first memory\n");
  REQUIRE(read_file(store.root() / "latest" / "rules" / "repo" / "carried.md")
          == "carried memory\n");

  const auto replacement = store.ingest_file({
      .type = memix::MemoryObjectType::Rules,
      .source_path = replacement_source,
      .context_path = "repo/nested/AGENTS.md",
      .message = "replace",
  });
  REQUIRE(replacement);
  REQUIRE_FALSE(replacement->projection_warning.has_value());
  REQUIRE(read_file(projected) == "updated memory\n");
  REQUIRE(read_file(store.root() / "latest" / "rules" / "repo" / "carried.md")
          == "carried memory\n");

  const auto blob_only =
      store.ingest(memix::MemoryObjectType::Documents, blob_only_source);
  REQUIRE(blob_only);
  REQUIRE_FALSE(path_exists(store.root() / "latest" / "documents"
                            / "untracked.md"));
}

TEST_CASE("expose rebuilds every type and is idempotent",
          "[store][expose]") {
  TemporaryDirectory directory("expose-rebuild");
  const fs::path rule = directory.path() / "AGENTS.md";
  const fs::path chat = directory.path() / "session.txt";
  const fs::path document = directory.path() / "design.md";
  write_file(rule, "rule projection\n");
  write_file(chat, "chat projection\n");
  write_file(document, "document projection\n");

  const memix::Store store(directory.path() / "store");
  REQUIRE(store.ingest_file({
      .type = memix::MemoryObjectType::Rules,
      .source_path = rule,
      .context_path = "repo/AGENTS.md",
      .message = {},
  }));
  REQUIRE(store.ingest_file({
      .type = memix::MemoryObjectType::Chat,
      .source_path = chat,
      .context_path = "sessions/2026/session.txt",
      .message = {},
  }));
  REQUIRE(store.ingest_file({
      .type = memix::MemoryObjectType::Documents,
      .source_path = document,
      .context_path = "notes/design.md",
      .message = {},
  }));

  const fs::path latest = store.root() / "latest";
  make_directory_tree_writable(latest);
  REQUIRE(fs::remove_all(latest));
  REQUIRE_FALSE(path_exists(latest));

  const auto rebuilt = store.expose();
  REQUIRE(rebuilt);
  REQUIRE(rebuilt->files_by_type.size() == 3);
  REQUIRE(rebuilt->files_by_type[0].type == memix::MemoryObjectType::Rules);
  REQUIRE(rebuilt->files_by_type[0].file_count == 1);
  REQUIRE(rebuilt->files_by_type[1].type == memix::MemoryObjectType::Chat);
  REQUIRE(rebuilt->files_by_type[1].file_count == 1);
  REQUIRE(rebuilt->files_by_type[2].type
          == memix::MemoryObjectType::Documents);
  REQUIRE(rebuilt->files_by_type[2].file_count == 1);
  REQUIRE(read_file(latest / "rules" / "repo" / "AGENTS.md")
          == "rule projection\n");
  REQUIRE(read_file(latest / "chat" / "sessions" / "2026" / "session.txt")
          == "chat projection\n");
  REQUIRE(read_file(latest / "documents" / "notes" / "design.md")
          == "document projection\n");

  const auto repeated = store.expose();
  REQUIRE(repeated);
  REQUIRE(repeated->files_by_type.size() == 3);
  REQUIRE(repeated->files_by_type[0].file_count == 1);
  REQUIRE(repeated->files_by_type[1].file_count == 1);
  REQUIRE(repeated->files_by_type[2].file_count == 1);

  const memix::Store empty_store(directory.path() / "empty-store");
  const auto empty = empty_store.expose();
  REQUIRE(empty);
  REQUIRE(empty->files_by_type.size() == 3);
  for (const memix::ExposeTypeReport &report : empty->files_by_type) {
    REQUIRE(report.file_count == 0);
  }
  const fs::path empty_latest = empty_store.root() / "latest";
  REQUIRE(fs::is_directory(empty_latest).value_or(false));
  const memix::FsResult<fs::file_status> empty_status =
      fs::status(empty_latest);
  REQUIRE(empty_status);
  REQUIRE((empty_status->permissions() & fs::perms::owner_write)
          == fs::perms::none);
}

TEST_CASE("memix store expose reports plain and JSON counts",
          "[store][expose][cli]") {
  TemporaryDirectory directory("expose-cli");
  const fs::path rule = directory.path() / "AGENTS.md";
  const fs::path document = directory.path() / "design.md";
  write_file(rule, "rule\n");
  write_file(document, "document\n");
  const fs::path store_root = directory.path() / "store";
  const memix::Store store(store_root);
  REQUIRE(store.ingest_file({
      .type = memix::MemoryObjectType::Rules,
      .source_path = rule,
      .context_path = "repo/AGENTS.md",
      .message = {},
  }));
  REQUIRE(store.ingest_file({
      .type = memix::MemoryObjectType::Documents,
      .source_path = document,
      .context_path = "notes/design.md",
      .message = {},
  }));

  make_directory_tree_writable(store_root / "latest");
  REQUIRE(fs::remove_all(store_root / "latest"));
  const CliResult plain =
      run_cli(directory.path(), store_root, {"expose"});
  INFO(plain.error);
  REQUIRE(plain.status == 0);
  REQUIRE(plain.output
          == "rules: 1 file\nchat: 0 files\ndocuments: 1 file\n");

  make_directory_tree_writable(store_root / "latest");
  REQUIRE(fs::remove_all(store_root / "latest"));
  const CliResult json =
      run_cli(directory.path(), store_root, {"expose", "--json"});
  INFO(json.error);
  REQUIRE(json.status == 0);
  REQUIRE(json.output
          == R"([{"type":"rules","files":1},{"type":"chat","files":0},{"type":"documents","files":1}])"
             "\n");
}

TEST_CASE("projection refresh failures warn without failing ingest",
          "[store][expose][cli]") {
  TemporaryDirectory directory("projection-warning");
  const fs::path first_source = directory.path() / "first.md";
  const fs::path second_source = directory.path() / "second.md";
  const fs::path third_source = directory.path() / "third.md";
  write_file(first_source, "first\n");
  write_file(second_source, "second\n");
  write_file(third_source, "third\n");
  const fs::path store_root = directory.path() / "store";
  const memix::Store store(store_root);
  const auto first = store.ingest_file({
      .type = memix::MemoryObjectType::Rules,
      .source_path = first_source,
      .context_path = "repo/first.md",
      .message = {},
  });
  REQUIRE(first);

  make_directory_tree_writable(store_root / "latest");
  REQUIRE(fs::remove_all(store_root / "latest" / "rules"));
  write_file(store_root / "latest" / "rules", "corrupt projection\n");
  const auto second = store.ingest_file({
      .type = memix::MemoryObjectType::Rules,
      .source_path = second_source,
      .context_path = "repo/second.md",
      .message = {},
  });
  REQUIRE(second);
  REQUIRE(second->projection_warning.has_value());
  REQUIRE(second->projection_warning->contains("memix store expose"));
  const auto latest_after_warning = store.read_ref("rules/latest");
  REQUIRE(latest_after_warning);
  REQUIRE(latest_after_warning->has_value());
  REQUIRE(**latest_after_warning == second->generation.id);
  const auto files_after_warning =
      store.list_files(memix::MemoryObjectType::Rules);
  REQUIRE(files_after_warning);
  REQUIRE(files_after_warning->size() == 2);

  REQUIRE(store.expose());
  make_directory_tree_writable(store_root / "latest");
  REQUIRE(fs::remove_all(store_root / "latest" / "rules"));
  write_file(store_root / "latest" / "rules", "corrupt again\n");
  const CliResult cli =
      run_cli(directory.path(), store_root,
              {"ingest", "--type", "rules", third_source.string(), "--dest",
               "repo/third.md"});
  INFO(cli.error);
  REQUIRE(cli.status == 0);
  REQUIRE(cli.error.contains("[warning]"));
  REQUIRE(cli.error.contains("memix store expose"));
  const auto files_after_cli = store.list_files(memix::MemoryObjectType::Rules);
  REQUIRE(files_after_cli);
  REQUIRE(files_after_cli->size() == 3);
}
