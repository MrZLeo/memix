#pragma once

#include <cstdint>
#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace memix {

struct StoreError {
  std::string message;
};

using StoreVoid = std::expected<void, StoreError>;

enum class MemoryObjectType : std::uint8_t { Agents, Chat, Documents };
std::string_view to_string(MemoryObjectType type) noexcept;
std::optional<MemoryObjectType>
parse_memory_object_type(std::string_view value) noexcept;

enum class ContextTreeEntryKind : std::uint8_t { Blob, Tree };
std::vector<MemoryObjectType> memory_object_types();
std::string_view to_string(ContextTreeEntryKind kind) noexcept;
std::optional<ContextTreeEntryKind>
parse_context_tree_entry_kind(std::string_view value) noexcept;

std::expected<std::filesystem::path, StoreError> default_store_root();

struct StoredObject {
  int schema_version = 1;
  int object_version = 1;
  MemoryObjectType type = MemoryObjectType::Agents;
  std::string id;
  std::string hash_algorithm = "sha256";
  std::string hash;
  std::uintmax_t size_bytes = 0;
  std::string filename;
  std::string created_at;
  std::filesystem::path content_path = "object";
  std::filesystem::path object_dir;
  std::filesystem::path metadata_path;
};

[[nodiscard]] std::expected<std::string, StoreError>
metadata_json(const StoredObject &object);

struct IngestResult {
  StoredObject object;
  bool inserted = false;
};

struct ContextTreeEntry {
  std::string name;
  ContextTreeEntryKind kind = ContextTreeEntryKind::Blob;
  std::string id;
};

struct ContextTree {
  int schema_version = 1;
  int object_version = 1;
  MemoryObjectType type = MemoryObjectType::Agents;
  std::string id;
  std::vector<ContextTreeEntry> entries;
  std::filesystem::path object_path;
};

struct ContextGeneration {
  int schema_version = 1;
  int object_version = 1;
  MemoryObjectType type = MemoryObjectType::Agents;
  std::string id;
  std::string root_tree;
  std::optional<std::string> parent;
  std::string created_at;
  std::string message;
  std::filesystem::path object_path;
};

struct FileIngestRequest {
  MemoryObjectType type = MemoryObjectType::Documents;
  std::filesystem::path source_path;
  std::filesystem::path context_path;
  std::string_view message;
};

struct FileIngestResult {
  IngestResult blob;
  ContextGeneration generation;
  std::optional<std::string> previous_generation;
  MemoryObjectType type = MemoryObjectType::Documents;
  std::filesystem::path context_path;
};

struct StoreFile {
  MemoryObjectType type = MemoryObjectType::Documents;
  std::filesystem::path context_path;
  StoredObject blob;
};

class Store {
public:
  explicit Store(std::filesystem::path root) : root_(std::move(root)) {}

  [[nodiscard]] const std::filesystem::path &root() const noexcept {
    return root_;
  }

  [[nodiscard]] std::expected<IngestResult, StoreError>
  ingest(MemoryObjectType type, const std::filesystem::path &source_path) const;

  [[nodiscard]] std::expected<IngestResult, StoreError>
  ingest_blob(MemoryObjectType type,
              const std::filesystem::path &source_path) const;

  [[nodiscard]] std::expected<FileIngestResult, StoreError>
  ingest_file(const FileIngestRequest &request) const;

  [[nodiscard]] std::expected<StoredObject, StoreError>
  read_blob(MemoryObjectType type, std::string_view object_id) const;

  [[nodiscard]] std::expected<ContextTree, StoreError>
  read_tree(MemoryObjectType type, std::string_view object_id) const;

  [[nodiscard]] std::expected<ContextGeneration, StoreError>
  read_generation(MemoryObjectType type, std::string_view object_id) const;

  [[nodiscard]] std::expected<StoredObject, StoreError>
  read_file(MemoryObjectType type, const std::filesystem::path &context_path,
            std::optional<std::string_view> generation = std::nullopt) const;

  [[nodiscard]] std::expected<std::vector<StoreFile>, StoreError>
  list_files(MemoryObjectType type,
             std::optional<std::string_view> generation = std::nullopt) const;

  [[nodiscard]] std::expected<std::optional<std::string>, StoreError>
  read_ref(std::string_view ref_name) const;

  [[nodiscard]] StoreVoid
  update_ref(std::string_view ref_name,
             std::optional<std::string_view> expected_old,
             std::string_view new_generation) const;

  [[nodiscard]] std::expected<std::vector<StoredObject>, StoreError>
  list(std::optional<MemoryObjectType> type = std::nullopt) const;

private:
  std::filesystem::path root_;
};

} // namespace memix
