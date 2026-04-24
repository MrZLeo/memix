#pragma once

#include <cstdint>
#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace memix {

struct StoreError {
  std::string message;
};

enum class MemoryObjectType : std::uint8_t { Agents, Chat, Documents };

std::string_view to_string(MemoryObjectType type) noexcept;
std::optional<MemoryObjectType>
parse_memory_object_type(std::string_view value) noexcept;
std::vector<MemoryObjectType> memory_object_types();

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

class Store {
public:
  explicit Store(std::filesystem::path root);

  [[nodiscard]] const std::filesystem::path &root() const noexcept;

  [[nodiscard]] std::expected<IngestResult, StoreError>
  ingest(MemoryObjectType type, const std::filesystem::path &source_path) const;

  [[nodiscard]] std::expected<std::vector<StoredObject>, StoreError>
  list(std::optional<MemoryObjectType> type = std::nullopt) const;

private:
  std::filesystem::path root_;
};

} // namespace memix
