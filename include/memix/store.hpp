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

enum class AgentTreeEntryKind : std::uint8_t { Blob, Tree };
std::vector<MemoryObjectType> memory_object_types();
std::string_view to_string(AgentTreeEntryKind kind) noexcept;
std::optional<AgentTreeEntryKind>
parse_agent_tree_entry_kind(std::string_view value) noexcept;

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

struct AgentTreeEntry {
  std::string name;
  AgentTreeEntryKind kind = AgentTreeEntryKind::Blob;
  std::string id;
};

struct AgentTree {
  int schema_version = 1;
  int object_version = 1;
  std::string id;
  std::vector<AgentTreeEntry> entries;
  std::filesystem::path object_path;
};

struct AgentGeneration {
  int schema_version = 1;
  int object_version = 1;
  std::string id;
  std::string root_tree;
  std::optional<std::string> parent;
  std::string created_at;
  std::string message;
  std::filesystem::path object_path;
};

struct AgentIngestResult {
  IngestResult blob;
  AgentGeneration generation;
  std::optional<std::string> previous_generation;
  std::filesystem::path agent_path;
};

struct AgentFileIngestRequest {
  std::filesystem::path source_path;
  std::filesystem::path agent_path;
  std::string_view message;
};

struct AgentFile {
  std::filesystem::path agent_path;
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
  ingest_agent_blob(const std::filesystem::path &source_path) const;

  [[nodiscard]] std::expected<AgentIngestResult, StoreError>
  ingest_agent_file(const AgentFileIngestRequest &request) const;

  [[nodiscard]] std::expected<StoredObject, StoreError>
  read_agent_blob(std::string_view object_id) const;

  [[nodiscard]] std::expected<AgentTree, StoreError>
  read_agent_tree(std::string_view object_id) const;

  [[nodiscard]] std::expected<AgentGeneration, StoreError>
  read_agent_generation(std::string_view object_id) const;

  [[nodiscard]] std::expected<StoredObject, StoreError> read_agent_file(
      const std::filesystem::path &agent_path,
      std::optional<std::string_view> generation = std::nullopt) const;

  [[nodiscard]] std::expected<std::vector<AgentFile>, StoreError>
  list_agent_files(
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
