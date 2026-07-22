#pragma once

#include "memix/error.hpp"
#include "memix/fs.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace memix {

enum class MemoryObjectType : std::uint8_t { Rules, Chat, Documents };
enum class ContextTreeEntryKind : std::uint8_t { Blob, Tree };

namespace detail {

template <typename EntryRange, typename Enum>
[[nodiscard]] constexpr std::string_view
token_for(const EntryRange &entries, Enum value) noexcept {
  auto it = std::ranges::find(entries, value,
                              [](const auto &entry) { return entry.value; });

  if (it != std::ranges::end(entries))
    return it->token;
  return {};
}

template <typename Enum, typename EntryRange>
[[nodiscard]] constexpr std::optional<Enum>
value_for_token(const EntryRange &entries, std::string_view token) noexcept {
  auto it = std::ranges::find(entries, token,
                              [](const auto entry) { return entry.token; });

  if (it != std::ranges::end(entries))
    return it->value;
  return std::nullopt;
}

struct MemoryObjectTypeToken {
  MemoryObjectType value;
  std::string_view token;
};

struct ContextTreeEntryKindToken {
  ContextTreeEntryKind value;
  std::string_view token;
};

inline constexpr std::array kMemoryObjectTypeTokens{
    MemoryObjectTypeToken{
        .value = MemoryObjectType::Rules,
        .token = "rules",
    },
    MemoryObjectTypeToken{
        .value = MemoryObjectType::Chat,
        .token = "chat",
    },
    MemoryObjectTypeToken{
        .value = MemoryObjectType::Documents,
        .token = "documents",
    },
};

inline constexpr std::array kContextTreeEntryKindTokens{
    ContextTreeEntryKindToken{
        .value = ContextTreeEntryKind::Blob,
        .token = "blob",
    },
    ContextTreeEntryKindToken{
        .value = ContextTreeEntryKind::Tree,
        .token = "tree",
    },
};

} // namespace detail

[[nodiscard]] constexpr std::string_view
to_string(MemoryObjectType type) noexcept {
  return detail::token_for(detail::kMemoryObjectTypeTokens, type);
}

[[nodiscard]] constexpr std::optional<MemoryObjectType>
parse_memory_object_type(std::string_view value) noexcept {
  return detail::value_for_token<MemoryObjectType>(
      detail::kMemoryObjectTypeTokens, value);
}

[[nodiscard]] constexpr auto memory_object_type_values() noexcept {
  std::array<MemoryObjectType, detail::kMemoryObjectTypeTokens.size()> types{};
  for (std::size_t index = 0; index < types.size(); ++index) {
    types[index] = detail::kMemoryObjectTypeTokens[index].value;
  }
  return types;
}

[[nodiscard]] constexpr std::vector<MemoryObjectType> memory_object_types() {
  const auto types = memory_object_type_values();
  return {types.begin(), types.end()};
}

[[nodiscard]] constexpr std::string_view
to_string(ContextTreeEntryKind kind) noexcept {
  return detail::token_for(detail::kContextTreeEntryKindTokens, kind);
}

[[nodiscard]] constexpr std::optional<ContextTreeEntryKind>
parse_context_tree_entry_kind(std::string_view value) noexcept {
  return detail::value_for_token<ContextTreeEntryKind>(
      detail::kContextTreeEntryKindTokens, value);
}

StoreResult<fs::path> default_store_root();

struct StoredObject {
  int schema_version = 1;
  int object_version = 1;
  MemoryObjectType type = MemoryObjectType::Rules;
  std::string id;
  std::string hash_algorithm = "sha256";
  std::string hash;
  std::uintmax_t size_bytes = 0;
  std::string filename;
  std::string created_at;
  fs::path content_path = "object";
  fs::path object_dir;
  fs::path metadata_path;
};

[[nodiscard]] StoreResult<std::string>
metadata_json(const StoredObject &object);

struct IngestResult {
  StoredObject object;
  bool inserted = false;
};

struct ContextTreeEntry {
  ContextTreeEntryKind kind = ContextTreeEntryKind::Blob;
  std::string name;
  std::string id;
};

struct ContextTree {
  int schema_version = 1;
  int object_version = 1;
  MemoryObjectType type = MemoryObjectType::Rules;
  std::string id;
  std::vector<ContextTreeEntry> entries;
  fs::path object_path;
};

struct ContextGeneration {
  int schema_version = 1;
  int object_version = 1;
  MemoryObjectType type = MemoryObjectType::Rules;
  std::string id;
  std::string root_tree;
  std::optional<std::string> parent;
  std::string created_at;
  std::string message;
  fs::path object_path;
};

struct FileIngestRequest {
  MemoryObjectType type = MemoryObjectType::Documents;
  fs::path source_path;
  fs::path context_path;
  std::string_view message;
};

struct FileIngestResult {
  IngestResult blob;
  ContextGeneration generation;
  std::optional<std::string> previous_generation;
  MemoryObjectType type = MemoryObjectType::Documents;
  fs::path context_path;
};

struct StoreFile {
  MemoryObjectType type = MemoryObjectType::Documents;
  fs::path context_path;
  StoredObject blob;
};

class Store {
public:
  explicit Store(fs::path root) : root_(std::move(root)) {}

  [[nodiscard]] const fs::path &root() const noexcept { return root_; }

  [[nodiscard]] StoreResult<IngestResult>
  ingest(MemoryObjectType type, const fs::path &source_path) const;

  [[nodiscard]] StoreResult<IngestResult>
  ingest_blob(MemoryObjectType type, const fs::path &source_path) const;

  [[nodiscard]] StoreResult<FileIngestResult>
  ingest_file(const FileIngestRequest &request) const;

  [[nodiscard]] StoreResult<StoredObject>
  read_blob(MemoryObjectType type, std::string_view object_id) const;

  [[nodiscard]] StoreResult<ContextTree>
  read_tree(MemoryObjectType type, std::string_view object_id) const;

  [[nodiscard]] StoreResult<ContextGeneration>
  read_generation(MemoryObjectType type, std::string_view object_id) const;

  [[nodiscard]] StoreResult<StoredObject>
  read_file(MemoryObjectType type, const fs::path &context_path,
            std::optional<std::string_view> generation = std::nullopt) const;

  [[nodiscard]] StoreResult<std::vector<StoreFile>>
  list_files(MemoryObjectType type,
             std::optional<std::string_view> generation = std::nullopt) const;

  [[nodiscard]] StoreResult<std::optional<std::string>>
  read_ref(std::string_view ref_name) const;

  [[nodiscard]] StoreVoid
  update_ref(std::string_view ref_name,
             std::optional<std::string_view> expected_old,
             std::string_view new_generation) const;

  [[nodiscard]] StoreResult<std::vector<StoredObject>>
  list(std::optional<MemoryObjectType> type = std::nullopt) const;

private:
  fs::path root_;
};

} // namespace memix
