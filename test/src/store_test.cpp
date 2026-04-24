#include "memix/store.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace fs = std::filesystem;

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
  return fs::temp_directory_path()
       / std::format("memix-{}-{}-{}", label, nanos, random_device());
}

class TemporaryDirectory {
public:
  explicit TemporaryDirectory(std::string_view label)
      : path_(unique_test_path(label)) {
    fs::create_directories(path_);
  }

  ~TemporaryDirectory() {
    std::error_code remove_error;
    fs::remove_all(path_, remove_error);
  }

  [[nodiscard]] const fs::path &path() const noexcept { return path_; }

private:
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

fs::path object_dir_for(const memix::Store &store, memix::MemoryObjectType type,
                        std::string_view digest) {
  return store.root()
       / std::string(memix::to_string(type))
       / std::string(digest);
}

void make_object_mutable_for_corruption(const fs::path &object_dir) {
  std::error_code permissions_error;
  fs::permissions(object_dir, fs::perms::owner_all, fs::perm_options::replace,
                  permissions_error);
  fs::permissions(object_dir / "object", fs::perms::owner_all,
                  fs::perm_options::replace, permissions_error);
  fs::permissions(object_dir / "metadata.json", fs::perms::owner_all,
                  fs::perm_options::replace, permissions_error);
}

} // namespace

TEST_CASE("memory object type helpers accept only supported v1 types",
          "[store]") {
  REQUIRE(memix::parse_memory_object_type("agents")
          == memix::MemoryObjectType::Agents);
  REQUIRE(memix::parse_memory_object_type("chat")
          == memix::MemoryObjectType::Chat);
  REQUIRE(memix::parse_memory_object_type("documents")
          == memix::MemoryObjectType::Documents);
  REQUIRE_FALSE(memix::parse_memory_object_type("pdf").has_value());
  REQUIRE(std::string(memix::to_string(memix::MemoryObjectType::Agents))
          == "agents");
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
  const fs::path source = directory.path() / "agents.md";
  write_file(source, "abc");

  const memix::Store store(directory.path() / "store");
  const auto result = store.ingest(memix::MemoryObjectType::Agents, source);
  REQUIRE(result.has_value());

  const std::string expected_hash =
      "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";
  REQUIRE(result->inserted);
  REQUIRE(result->object.id == expected_hash);
  REQUIRE(result->object.hash == expected_hash);
  REQUIRE(result->object.hash_algorithm == "sha256");
  REQUIRE(result->object.size_bytes == 3);
  REQUIRE(result->object.filename == "agents.md");
  REQUIRE(result->object.content_path == fs::path("object"));

  const fs::path object_dir =
      object_dir_for(store, memix::MemoryObjectType::Agents, expected_hash);
  REQUIRE(fs::is_regular_file(object_dir / "object"));
  REQUIRE(fs::is_regular_file(object_dir / "metadata.json"));
  REQUIRE(read_file(object_dir / "object") == "abc");

  const std::string metadata = read_file(object_dir / "metadata.json");
  REQUIRE(metadata.find("\"schema_version\":1") != std::string::npos);
  REQUIRE(metadata.find("\"object_version\":1") != std::string::npos);
  REQUIRE(metadata.find("\"type\":\"agents\"") != std::string::npos);
  REQUIRE(metadata.find("\"filename\":\"agents.md\"") != std::string::npos);
  REQUIRE(metadata.find(source.string()) == std::string::npos);
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

TEST_CASE("the same content can exist independently across object types",
          "[store]") {
  TemporaryDirectory directory("cross-type");
  const fs::path source = directory.path() / "memory.txt";
  write_file(source, "shared bytes");

  const memix::Store store(directory.path() / "store");
  const auto agents = store.ingest(memix::MemoryObjectType::Agents, source);
  const auto documents =
      store.ingest(memix::MemoryObjectType::Documents, source);
  REQUIRE(agents.has_value());
  REQUIRE(documents.has_value());

  REQUIRE(agents->inserted);
  REQUIRE(documents->inserted);
  REQUIRE(agents->object.id == documents->object.id);
  REQUIRE(fs::exists(object_dir_for(store, memix::MemoryObjectType::Agents,
                                    agents->object.id)));
  REQUIRE(fs::exists(object_dir_for(store, memix::MemoryObjectType::Documents,
                                    documents->object.id)));
  const auto objects = store.list();
  REQUIRE(objects.has_value());
  REQUIRE(objects->size() == 2);
}

TEST_CASE("list supports deterministic ordering and type filters", "[store]") {
  TemporaryDirectory directory("list");
  const fs::path agents_source = directory.path() / "agents.md";
  const fs::path chat_source = directory.path() / "chat.json";
  write_file(agents_source, "agents");
  write_file(chat_source, "chat");

  const memix::Store store(directory.path() / "store");
  const auto chat = store.ingest(memix::MemoryObjectType::Chat, chat_source);
  const auto agents =
      store.ingest(memix::MemoryObjectType::Agents, agents_source);
  REQUIRE(chat.has_value());
  REQUIRE(agents.has_value());

  const auto all = store.list();
  REQUIRE(all.has_value());
  REQUIRE(all->size() == 2);
  REQUIRE((*all)[0].type == memix::MemoryObjectType::Agents);
  REQUIRE((*all)[0].id == agents->object.id);
  REQUIRE((*all)[1].type == memix::MemoryObjectType::Chat);
  REQUIRE((*all)[1].id == chat->object.id);

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
  fs::remove(result->object.object_dir / "object");

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
