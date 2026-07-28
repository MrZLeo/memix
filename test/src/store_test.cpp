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

  ~TemporaryDirectory() { static_cast<void>(fs::remove_all(path_)); }

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
              })
              .has_value());
  REQUIRE(store
              .ingest_file({
                  .type = memix::MemoryObjectType::Documents,
                  .source_path = decisions,
                  .context_path = "notes/project/decisions.md",
              })
              .has_value());
  REQUIRE(store
              .ingest_file({
                  .type = memix::MemoryObjectType::Rules,
                  .source_path = rule,
                  .context_path = "repo/AGENTS.md",
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
  fs::remove(result->object.object_dir / "object");

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
