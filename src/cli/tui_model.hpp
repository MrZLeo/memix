#pragma once

#include "memix/store.hpp"
#include "memix/view.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <map>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace memix::tui {

inline constexpr std::size_t kShortIdLength = 12;
inline constexpr std::size_t kContentLimitBytes =
    std::size_t{256} * std::size_t{1024};
inline constexpr std::string_view kTruncatedMarker = "… truncated …";
inline constexpr std::size_t kListColumnWidth = 30;
// A column gap is a space plus a dim vertical line: the space keeps a list's
// scroll-indicator track from touching the separator glyph.
inline constexpr std::size_t kColumnSeparatorWidth = 2;
inline constexpr std::size_t kContentColumnMinimumWidth = 40;
inline constexpr std::size_t kNarrowTerminalWidth = 80;

enum class ColumnKind : std::uint8_t { List, Content };

struct ColumnWindow {
  std::size_t first_visible = 0;
  std::vector<std::size_t> widths;
  bool single_screen = false;
};

struct RowWindow {
  std::size_t top = 0;
  std::size_t first = 0;
  std::size_t last = 0;
};

struct RowWindowRequest {
  std::size_t total_rows = 0;
  std::size_t viewport_rows = 0;
  int requested_top_row = 0;
  std::size_t overscan = 0;
};

[[nodiscard]] constexpr std::size_t
max_scroll_top(std::size_t total_rows, std::size_t viewport_rows) noexcept {
  return total_rows > viewport_rows ? total_rows - viewport_rows : 0;
}

[[nodiscard]] constexpr RowWindow
virtualized_row_window(RowWindowRequest request) noexcept {
  const std::size_t requested_top =
      request.requested_top_row > 0
          ? static_cast<std::size_t>(request.requested_top_row)
          : 0;
  const std::size_t top = std::min(
      requested_top, max_scroll_top(request.total_rows, request.viewport_rows));
  const std::size_t first = top > request.overscan ? top - request.overscan : 0;
  const std::size_t rows_after_top = request.total_rows - top;
  const std::size_t visible_rows =
      std::min(request.viewport_rows, rows_after_top);
  const std::size_t overscan_rows =
      std::min(request.overscan, rows_after_top - visible_rows);
  return RowWindow{
      .top = top,
      .first = first,
      .last = top + visible_rows + overscan_rows,
  };
}

[[nodiscard]] inline ColumnWindow
column_window(std::span<const ColumnKind> stack, std::size_t terminal_width) {
  if (stack.empty()) {
    return {};
  }
  if (terminal_width < kNarrowTerminalWidth) {
    return ColumnWindow{
        .first_visible = stack.size() - 1,
        .widths = {terminal_width},
        .single_screen = true,
    };
  }

  const bool content_active = stack.back() == ColumnKind::Content;
  const std::size_t active_cost = content_active
                                    ? kContentColumnMinimumWidth
                                    : kListColumnWidth + kColumnSeparatorWidth;
  std::size_t remaining = terminal_width - active_cost;
  std::size_t first_visible = stack.size() - 1;
  const std::size_t list_cost = kListColumnWidth + kColumnSeparatorWidth;
  while (first_visible > 0
         && stack[first_visible - 1] == ColumnKind::List
         && remaining >= list_cost) {
    --first_visible;
    remaining -= list_cost;
  }

  const std::size_t visible_count = stack.size() - first_visible;
  std::vector<std::size_t> widths(visible_count, kListColumnWidth);
  if (content_active) {
    const std::size_t ancestor_count = visible_count - 1;
    widths.back() = terminal_width - (ancestor_count * list_cost);
  }
  return ColumnWindow{
      .first_visible = first_visible,
      .widths = std::move(widths),
      .single_screen = false,
  };
}

[[nodiscard]] inline std::string normalize_line_breaks(std::string_view text) {
  std::string normalized;
  normalized.reserve(text.size());
  for (std::size_t index = 0; index < text.size(); ++index) {
    const char value = text[index];
    if (value != '\r' && value != '\n') {
      normalized.push_back(value);
      continue;
    }
    normalized.push_back(' ');
    if (value == '\r' && index + 1 < text.size() && text[index + 1] == '\n') {
      ++index;
    }
  }
  return normalized;
}

[[nodiscard]] inline std::string short_id(std::string_view value) {
  return std::string(value.substr(0, std::min(value.size(), kShortIdLength)));
}

[[nodiscard]] inline std::string format_size(std::uintmax_t size_bytes) {
  constexpr std::array<std::string_view, 7> units = {
      {"B", "KB", "MB", "GB", "TB", "PB", "EB"},
  };
  constexpr std::uintmax_t unit_base_bytes = 1024;
  constexpr double unit_base = 1024.0;

  if (size_bytes < unit_base_bytes) {
    return std::format("{} B", size_bytes);
  }

  auto size = static_cast<double>(size_bytes);
  std::size_t unit_index = 0;
  while (size >= unit_base && unit_index + 1 < units.size()) {
    size /= unit_base;
    ++unit_index;
  }

  std::string formatted = std::format("{:.1f}", size);
  if (formatted.ends_with(".0")) {
    formatted.resize(formatted.size() - 2);
  }
  return std::format("{} {}", formatted, units[unit_index]);
}

[[nodiscard]] inline std::string
pluralize(std::size_t count, std::string_view singular,
          std::string_view plural) {
  return std::format("{} {}", count, count == 1 ? singular : plural);
}

[[nodiscard]] inline std::string pluralized_files(std::size_t count) {
  return pluralize(count, "file", "files");
}

[[nodiscard]] constexpr unsigned char ascii_lower(unsigned char value) {
  if (value >= static_cast<unsigned char>('A')
      && value <= static_cast<unsigned char>('Z')) {
    return static_cast<unsigned char>(value
                                      - static_cast<unsigned char>('A')
                                      + static_cast<unsigned char>('a'));
  }
  return value;
}

[[nodiscard]] inline std::string lowercase_copy(std::string_view text) {
  std::string result;
  result.reserve(text.size());
  for (const char value : text) {
    result.push_back(
        static_cast<char>(ascii_lower(static_cast<unsigned char>(value))));
  }
  return result;
}

[[nodiscard]] inline bool
contains_case_insensitive(std::string_view haystack, std::string_view needle) {
  if (needle.empty()) {
    return true;
  }
  return lowercase_copy(haystack).contains(lowercase_copy(needle));
}

enum class RowKind : std::uint8_t { Directory, File };

struct ListRow {
  RowKind kind = RowKind::File;
  fs::path path;
  std::string title;
  std::string summary;
  std::string blob_id;
};

namespace detail {

[[nodiscard]] inline int
compare_digit_runs(std::string_view left, std::size_t &left_index,
                   std::string_view right, std::size_t &right_index) {
  const std::size_t left_start = left_index;
  const std::size_t right_start = right_index;
  while (left_index < left.size()
         && std::isdigit(static_cast<unsigned char>(left[left_index])) != 0) {
    ++left_index;
  }
  while (right_index < right.size()
         && std::isdigit(static_cast<unsigned char>(right[right_index])) != 0) {
    ++right_index;
  }

  std::size_t left_significant = left_start;
  while (left_significant < left_index && left[left_significant] == '0') {
    ++left_significant;
  }
  std::size_t right_significant = right_start;
  while (right_significant < right_index && right[right_significant] == '0') {
    ++right_significant;
  }
  const std::size_t left_digits = left_index - left_significant;
  const std::size_t right_digits = right_index - right_significant;
  if (left_digits != right_digits) {
    return left_digits < right_digits ? -1 : 1;
  }
  for (std::size_t index = 0; index < left_digits; ++index) {
    if (left[left_significant + index] != right[right_significant + index]) {
      return left[left_significant + index] < right[right_significant + index]
               ? -1
               : 1;
    }
  }
  const std::size_t left_run = left_index - left_start;
  const std::size_t right_run = right_index - right_start;
  if (left_run != right_run) {
    return left_run < right_run ? -1 : 1;
  }
  return 0;
}

[[nodiscard]] inline bool
natural_less(std::string_view left, std::string_view right) {
  std::size_t left_index = 0;
  std::size_t right_index = 0;
  while (left_index < left.size() && right_index < right.size()) {
    const auto left_value = static_cast<unsigned char>(left[left_index]);
    const auto right_value = static_cast<unsigned char>(right[right_index]);
    if (std::isdigit(left_value) != 0 && std::isdigit(right_value) != 0) {
      const int digit_comparison =
          compare_digit_runs(left, left_index, right, right_index);
      if (digit_comparison != 0) {
        return digit_comparison < 0;
      }
      continue;
    }
    const auto folded_left = ascii_lower(left_value);
    const auto folded_right = ascii_lower(right_value);
    if (folded_left != folded_right) {
      return folded_left < folded_right;
    }
    ++left_index;
    ++right_index;
  }
  return left.size() < right.size();
}

[[nodiscard]] inline std::string
row_title(const fs::path &path, bool directory) {
  std::string title = normalize_line_breaks(path.filename().generic_string());
  if (directory) {
    title.push_back('/');
  }
  return title;
}

inline void sort_rows(std::vector<ListRow> &rows) {
  std::ranges::stable_sort(rows, [](const ListRow &left, const ListRow &right) {
    if (left.kind != right.kind) {
      return left.kind == RowKind::Directory;
    }
    return natural_less(left.title, right.title);
  });
}

[[nodiscard]] inline bool
direct_child(const fs::path &path, const fs::path &scope) {
  return path != scope && path.parent_path() == scope;
}

[[nodiscard]] constexpr bool
valid_logical_component(std::string_view component) {
  return !component.empty()
      && component != "."
      && component != ".."
      && !component.contains('/')
      && !component.contains('\\');
}

struct LogicalPathRequest {
  std::string_view value;
  std::string_view locator;
};

[[nodiscard]] inline std::expected<fs::path, std::string>
parse_logical_path(LogicalPathRequest request) {
  const std::string_view value = request.value;
  const std::string_view locator = request.locator;
  if (value.empty()) {
    return std::unexpected(
        std::format("locator has an empty path `{}`", locator));
  }
  if (value.contains("//")) {
    return std::unexpected(
        std::format("locator path contains empty components `{}`", locator));
  }
  if (value.contains('\\')) {
    return std::unexpected(
        std::format("locator path must not contain backslashes `{}`", locator));
  }
  if (value.starts_with('/')) {
    return std::unexpected(
        std::format("locator path must be relative `{}`", locator));
  }

  fs::path parsed{std::string(value)};
  for (const fs::path &component_path : parsed) {
    const std::string component = component_path.generic_string();
    if (!valid_logical_component(component)) {
      return std::unexpected(
          std::format("locator path contains invalid component `{}` in `{}`",
                      component, locator));
    }
  }
  return parsed;
}

[[nodiscard]] inline bool valid_hex_prefix(std::string_view value) {
  constexpr std::size_t digest_hex_length = 64;
  return !value.empty()
      && value.size() <= digest_hex_length
      && std::ranges::all_of(value, [](unsigned char character) {
           return (character >= static_cast<unsigned char>('0')
                   && character <= static_cast<unsigned char>('9'))
               || (character >= static_cast<unsigned char>('a')
                   && character <= static_cast<unsigned char>('f'));
         });
}

[[nodiscard]] inline std::string_view trim(std::string_view value) {
  while (!value.empty()
         && std::isspace(static_cast<unsigned char>(value.front())) != 0) {
    value.remove_prefix(1);
  }
  while (!value.empty()
         && std::isspace(static_cast<unsigned char>(value.back())) != 0) {
    value.remove_suffix(1);
  }
  return value;
}

inline constexpr unsigned char kUtf8ContinuationMask = 0xC0U;
inline constexpr unsigned char kUtf8ContinuationPrefix = 0x80U;
inline constexpr unsigned char kUtf8AsciiMaximum = 0x7FU;
inline constexpr unsigned char kUtf8TwoByteMinimum = 0xC2U;
inline constexpr unsigned char kUtf8TwoByteMaximum = 0xDFU;
inline constexpr unsigned char kUtf8TwoBytePayloadMask = 0x1FU;
inline constexpr unsigned char kUtf8ThreeByteMinimum = 0xE0U;
inline constexpr unsigned char kUtf8ThreeByteMaximum = 0xEFU;
inline constexpr unsigned char kUtf8ThreeBytePayloadMask = 0x0FU;
inline constexpr unsigned char kUtf8FourByteMinimum = 0xF0U;
inline constexpr unsigned char kUtf8FourByteMaximum = 0xF4U;
inline constexpr unsigned char kUtf8FourBytePayloadMask = 0x07U;
inline constexpr unsigned char kUtf8ContinuationPayloadMask = 0x3FU;
inline constexpr unsigned int kUtf8PayloadShift = 6;
inline constexpr std::uint32_t kUtf8TwoByteCodepointMinimum = 0x80U;
inline constexpr std::uint32_t kUtf8ThreeByteCodepointMinimum = 0x800U;
inline constexpr std::uint32_t kUtf8FourByteCodepointMinimum = 0x10000U;
inline constexpr std::uint32_t kUtf8MaximumCodepoint = 0x10FFFFU;
inline constexpr std::uint32_t kUtf8SurrogateMinimum = 0xD800U;
inline constexpr std::uint32_t kUtf8SurrogateMaximum = 0xDFFFU;

[[nodiscard]] constexpr bool continuation_byte(unsigned char value) {
  return (value & kUtf8ContinuationMask) == kUtf8ContinuationPrefix;
}

[[nodiscard]] inline bool valid_utf8(std::string_view text) {
  std::size_t index = 0;
  while (index < text.size()) {
    const auto first = static_cast<unsigned char>(text[index]);
    std::size_t length = 0;
    std::uint32_t codepoint = 0;
    std::uint32_t minimum = 0;
    if (first <= kUtf8AsciiMaximum) {
      length = 1;
      codepoint = first;
    } else if (first >= kUtf8TwoByteMinimum && first <= kUtf8TwoByteMaximum) {
      length = 2;
      codepoint = first & kUtf8TwoBytePayloadMask;
      minimum = kUtf8TwoByteCodepointMinimum;
    } else if (first >= kUtf8ThreeByteMinimum
               && first <= kUtf8ThreeByteMaximum) {
      length = 3;
      codepoint = first & kUtf8ThreeBytePayloadMask;
      minimum = kUtf8ThreeByteCodepointMinimum;
    } else if (first >= kUtf8FourByteMinimum && first <= kUtf8FourByteMaximum) {
      length = 4;
      codepoint = first & kUtf8FourBytePayloadMask;
      minimum = kUtf8FourByteCodepointMinimum;
    } else {
      return false;
    }
    if (index + length > text.size()) {
      return false;
    }
    for (std::size_t offset = 1; offset < length; ++offset) {
      const auto next = static_cast<unsigned char>(text[index + offset]);
      if (!continuation_byte(next)) {
        return false;
      }
      codepoint = (codepoint << kUtf8PayloadShift)
                | (static_cast<std::uint32_t>(next)
                   & static_cast<std::uint32_t>(kUtf8ContinuationPayloadMask));
    }
    if (length > 1
        && (codepoint < minimum
            || codepoint > kUtf8MaximumCodepoint
            || (codepoint >= kUtf8SurrogateMinimum
                && codepoint <= kUtf8SurrogateMaximum))) {
      return false;
    }
    index += length;
  }
  return true;
}

[[nodiscard]] inline std::size_t
trim_incomplete_utf8_suffix(std::string_view text) {
  if (text.empty()) {
    return 0;
  }
  std::size_t start = text.size() - 1;
  while (start > 0
         && continuation_byte(static_cast<unsigned char>(text[start]))) {
    --start;
  }
  const auto first = static_cast<unsigned char>(text[start]);
  std::size_t expected_size = 1;
  if (first >= kUtf8TwoByteMinimum && first <= kUtf8TwoByteMaximum) {
    expected_size = 2;
  } else if (first >= kUtf8ThreeByteMinimum && first <= kUtf8ThreeByteMaximum) {
    expected_size = 3;
  } else if (first >= kUtf8FourByteMinimum && first <= kUtf8FourByteMaximum) {
    expected_size = 4;
  }
  const std::size_t suffix_size = text.size() - start;
  if (expected_size > suffix_size
      && std::ranges::all_of(text.substr(start + 1), [](char value) {
           return continuation_byte(static_cast<unsigned char>(value));
         })) {
    return start;
  }
  return text.size();
}

inline void erase_last_utf8_code_point(std::string &text) {
  if (text.empty()) {
    return;
  }
  std::size_t start = text.size() - 1;
  while (start > 0
         && continuation_byte(static_cast<unsigned char>(text[start]))) {
    --start;
  }
  if (valid_utf8(std::string_view(text).substr(start))) {
    text.resize(start);
    return;
  }
  text.pop_back();
}

[[nodiscard]] inline std::vector<std::string>
split_lines(std::string_view text) {
  std::vector<std::string> lines;
  std::size_t start = 0;
  while (start <= text.size()) {
    const std::size_t end = text.find('\n', start);
    std::string line(end == std::string_view::npos
                         ? text.substr(start)
                         : text.substr(start, end - start));
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    lines.push_back(std::move(line));
    if (end == std::string_view::npos) {
      break;
    }
    start = end + 1;
  }
  return lines;
}

} // namespace detail

inline void erase_last_utf8_code_point(std::string &text) {
  detail::erase_last_utf8_code_point(text);
}

[[nodiscard]] inline std::vector<std::string>
split_display_lines(std::string_view value) {
  std::vector<std::string> lines;
  std::size_t start = 0;
  while (start <= value.size()) {
    const std::size_t end = value.find_first_of("\r\n", start);
    lines.emplace_back(end == std::string_view::npos
                           ? value.substr(start)
                           : value.substr(start, end - start));
    if (end == std::string_view::npos) {
      break;
    }
    start = end + 1;
    if (value[end] == '\r' && start < value.size() && value[start] == '\n') {
      ++start;
    }
  }
  return lines;
}

struct LabeledLinesRequest {
  std::string_view label;
  std::string_view value;
};

[[nodiscard]] inline std::vector<std::string>
split_labeled_lines(LabeledLinesRequest request) {
  std::vector<std::string> lines = split_display_lines(request.value);

  const std::string prefix = std::format("{}: ", request.label);
  lines.front().insert(0, prefix);
  const std::string continuation_indent(prefix.size(), ' ');
  for (std::size_t index = 1; index < lines.size(); ++index) {
    lines[index].insert(0, continuation_indent);
  }
  return lines;
}

[[nodiscard]] inline std::vector<ListRow>
catalog_rows(const std::vector<StoreCatalogEntry> &entries,
             const fs::path &scope = {}) {
  std::vector<ListRow> rows;
  for (const StoreCatalogEntry &entry : entries) {
    if (!detail::direct_child(entry.context_path, scope)) {
      continue;
    }
    rows.push_back(ListRow{
        .kind = entry.directory ? RowKind::Directory : RowKind::File,
        .path = entry.context_path,
        .title = detail::row_title(entry.context_path, entry.directory),
        .summary =
            entry.directory
                ? std::format("{} · {}", pluralized_files(entry.file_count),
                              format_size(entry.total_size_bytes))
                : normalize_line_breaks(
                      std::format("blob {} · {} · {}", short_id(entry.blob_id),
                                  format_size(entry.total_size_bytes),
                                  entry.latest_created_at)),
        .blob_id = entry.blob_id,
    });
  }
  detail::sort_rows(rows);
  return rows;
}

using CatalogDirectoryResult = std::expected<std::size_t, std::string>;

[[nodiscard]] inline CatalogDirectoryResult
catalog_directory_file_count(const std::vector<StoreCatalogEntry> &entries,
                             const fs::path &scope) {
  const auto base =
      std::ranges::find(entries, scope, &StoreCatalogEntry::context_path);
  if (base == entries.end()) {
    if (scope.empty()) {
      return std::size_t{0};
    }
    return std::unexpected(std::format("context directory does not exist `{}`",
                                       scope.generic_string()));
  }
  if (!base->directory) {
    return std::unexpected(std::format("context path is not a directory `{}`",
                                       scope.generic_string()));
  }
  return base->file_count;
}

struct EnvironmentTreeSnapshot {
  std::vector<ListRow> rows;
  std::size_t file_count = 0;
  bool path_exists = false;
};

[[nodiscard]] inline EnvironmentTreeSnapshot
environment_tree_snapshot(const std::vector<ResolvedViewFile> &files,
                          const fs::path &scope = {}) {
  const std::vector<fs::path> scope_components =
      scope | std::ranges::to<std::vector<fs::path>>();
  const auto is_under_scope = [&scope_components](const fs::path &path) {
    const std::vector<fs::path> path_components =
        path | std::ranges::to<std::vector<fs::path>>();
    return scope_components.size() <= path_components.size()
        && std::ranges::equal(scope_components,
                              path_components
                                  | std::views::take(scope_components.size()));
  };

  struct DirectoryAggregate {
    std::size_t file_count = 0;
    std::uintmax_t total_size_bytes = 0;
  };
  std::map<fs::path, DirectoryAggregate> directories;
  EnvironmentTreeSnapshot snapshot;
  snapshot.path_exists = scope.empty();
  for (const ResolvedViewFile &file : files) {
    if (!is_under_scope(file.output_path)) {
      continue;
    }
    snapshot.path_exists = true;
    ++snapshot.file_count;
    if (file.output_path.parent_path() == scope) {
      snapshot.rows.push_back(ListRow{
          .kind = RowKind::File,
          .path = file.output_path,
          .title = detail::row_title(file.output_path, false),
          .summary = std::format("blob {} · {}", short_id(file.blob.id),
                                 format_size(file.blob.size_bytes)),
          .blob_id = file.blob.id,
      });
      continue;
    }
    const std::vector<fs::path> path_components =
        file.output_path | std::ranges::to<std::vector<fs::path>>();
    if (path_components.size() <= scope_components.size()) {
      continue;
    }
    fs::path directory = scope;
    directory /= path_components[scope_components.size()];
    DirectoryAggregate &aggregate = directories[directory];
    ++aggregate.file_count;
    aggregate.total_size_bytes += file.blob.size_bytes;
  }
  for (const auto &[path, aggregate] : directories) {
    snapshot.rows.push_back(ListRow{
        .kind = RowKind::Directory,
        .path = path,
        .title = detail::row_title(path, true),
        .summary =
            std::format("{} · {}", pluralized_files(aggregate.file_count),
                        format_size(aggregate.total_size_bytes)),
        .blob_id = {},
    });
  }
  detail::sort_rows(snapshot.rows);
  return snapshot;
}

[[nodiscard]] inline bool
fields_match(std::string_view title, std::string_view summary,
             std::string_view query) {
  return contains_case_insensitive(title, query)
      || contains_case_insensitive(summary, query);
}

enum class EditOperation : std::uint8_t { Edit, Add, Remove };

[[nodiscard]] constexpr std::string_view
edit_operation_name(EditOperation operation) noexcept {
  switch (operation) {
  case EditOperation::Edit:
    return "edit";
  case EditOperation::Add:
    return "add";
  case EditOperation::Remove:
    return "remove";
  }
  return {};
}

[[nodiscard]] inline std::string
default_edit_message(EditOperation operation, MemoryObjectType type,
                     const fs::path &path) {
  return std::format("{} {}/{}", edit_operation_name(operation),
                     to_string(type), path.generic_string());
}

using NewFileNameResult = std::expected<fs::path, std::string>;

[[nodiscard]] inline NewFileNameResult
validate_new_file_name(std::string_view name) {
  if (name.empty()) {
    return std::unexpected("file name must not be empty");
  }
  if (name.ends_with('/') || name.ends_with('\\')) {
    return std::unexpected("file name must not end with a slash");
  }
  if (name.contains('\\')) {
    return std::unexpected("file name must not contain backslashes");
  }
  if (name.starts_with('/') || fs::path(std::string(name)).is_absolute()) {
    return std::unexpected("file name must be relative");
  }

  std::size_t component_start = 0;
  while (component_start < name.size()) {
    const std::size_t separator = name.find('/', component_start);
    const std::size_t component_end =
        separator == std::string_view::npos ? name.size() : separator;
    const std::string_view component =
        name.substr(component_start, component_end - component_start);
    if (component.empty()) {
      return std::unexpected("file name must not contain empty components");
    }
    if (component == "..") {
      return std::unexpected("file name must not contain `..`");
    }
    if (component == ".") {
      return std::unexpected("file name must not contain `.`");
    }
    if (separator == std::string_view::npos) {
      break;
    }
    component_start = separator + 1;
  }
  return fs::path(std::string(name));
}

[[nodiscard]] constexpr bool
content_unchanged(std::string_view original, std::string_view edited) noexcept {
  return original == edited;
}

enum class LocatorKind : std::uint8_t {
  ContextFile,
  ContextDirectory,
  Object,
  Environment,
  Quest,
};

enum class ContextVersionKind : std::uint8_t { Latest, Generation, Ref };

struct ContextVersion {
  ContextVersionKind kind = ContextVersionKind::Latest;
  std::string value = "latest";
  // Raw and sha256: selectors are generation object IDs. The
  // generation/<number> spelling remains a generated ref selector.
  bool direct_generation = false;
};

struct Locator {
  LocatorKind kind = LocatorKind::ContextFile;
  std::optional<MemoryObjectType> type;
  fs::path path;
  ContextVersion version;
  std::string value;
};

using LocatorResult = std::expected<Locator, std::string>;

namespace detail {

using OptionalLocatorResult =
    std::expected<std::optional<Locator>, std::string>;

[[nodiscard]] inline OptionalLocatorResult
parse_special_locator(const std::string &source) {
  constexpr std::array special_prefixes{
      std::pair{std::string_view("objects/"), LocatorKind::Object},
      std::pair{std::string_view("environments/"), LocatorKind::Environment},
      std::pair{std::string_view("quests/"), LocatorKind::Quest},
  };
  for (const auto &[prefix, kind] : special_prefixes) {
    if (!source.starts_with(prefix)) {
      continue;
    }
    std::string value = source.substr(prefix.size());
    if (value.empty()) {
      return std::unexpected(std::format("{} locator requires a value",
                                         prefix.substr(0, prefix.size() - 1)));
    }
    if (kind != LocatorKind::Quest && !valid_hex_prefix(value)) {
      return std::unexpected(
          std::format("{} locator requires a lowercase hexadecimal id prefix",
                      prefix.substr(0, prefix.size() - 1)));
    }
    return Locator{
        .kind = kind,
        .type = std::nullopt,
        .path = {},
        .version = ContextVersion(),
        .value = std::move(value),
    };
  }
  return std::nullopt;
}

[[nodiscard]] inline std::expected<ContextVersion, std::string>
parse_context_version(std::string_view version_text,
                      const std::string &source) {
  if (version_text.empty() || version_text == "latest") {
    return ContextVersion{};
  }
  constexpr std::string_view sha256_prefix = "sha256:";
  const bool sha256_selector = version_text.starts_with(sha256_prefix);
  const std::string_view generation_id =
      sha256_selector ? version_text.substr(sha256_prefix.size())
                      : version_text;
  constexpr std::size_t sha256_hex_length = 64;
  if (generation_id.size() == sha256_hex_length
      && valid_hex_prefix(generation_id)) {
    return ContextVersion{
        .kind = ContextVersionKind::Generation,
        .value = std::string(generation_id),
        .direct_generation = true,
    };
  }
  if (version_text.starts_with("generation/")) {
    const std::string value{
        version_text.substr(std::string_view("generation/").size())};
    if (!valid_logical_component(value)) {
      return std::unexpected(std::format(
          "locator has invalid generation selector `{}`", version_text));
    }
    return ContextVersion{
        .kind = ContextVersionKind::Generation,
        .value = value,
        .direct_generation = false,
    };
  }
  if (version_text.starts_with("ref/")) {
    const std::string value{
        version_text.substr(std::string_view("ref/").size())};
    if (!parse_logical_path({.value = value, .locator = source})) {
      return std::unexpected(
          std::format("locator has invalid ref selector `{}`", version_text));
    }
    return ContextVersion{
        .kind = ContextVersionKind::Ref,
        .value = value,
        .direct_generation = false,
    };
  }
  return std::unexpected(
      std::format("locator has unsupported version `{}`", version_text));
}

} // namespace detail

[[nodiscard]] inline LocatorResult parse_locator(std::string_view input) {
  const std::string_view trimmed = detail::trim(input);
  if (trimmed.empty()) {
    return std::unexpected("locator must not be empty");
  }
  const std::string source(trimmed);

  auto special = detail::parse_special_locator(source);
  if (!special) {
    return std::unexpected(special.error());
  }
  if (special->has_value()) {
    return std::move(**special);
  }

  const std::size_t version_separator = source.rfind('@');
  std::string path_text = version_separator == std::string::npos
                            ? source
                            : source.substr(0, version_separator);
  const std::string version_text = version_separator == std::string::npos
                                     ? "latest"
                                     : source.substr(version_separator + 1);
  const std::size_t type_separator = path_text.find('/');
  if (type_separator == std::string::npos || type_separator == 0) {
    return std::unexpected(
        std::format("locator must start with `<type>/` `{}`", source));
  }
  const std::string_view type_text(path_text.data(), type_separator);
  const std::optional<MemoryObjectType> type =
      parse_memory_object_type(type_text);
  if (!type.has_value()) {
    return std::unexpected(
        std::format("locator has unsupported type `{}`", type_text));
  }

  std::string logical_path = path_text.substr(type_separator + 1);
  // A bare `<type>/` targets the type root directory: the trailing slash is
  // the type separator itself, so an empty remainder also means directory.
  const bool directory = logical_path.empty() || logical_path.ends_with('/');
  if (!logical_path.empty()
      && (logical_path.starts_with('/') || logical_path.contains("//"))) {
    return std::unexpected(
        std::format("locator path contains empty components `{}`", source));
  }
  if (directory && !logical_path.empty()) {
    logical_path.pop_back();
  }

  auto version = detail::parse_context_version(version_text, source);
  if (!version) {
    return std::unexpected(version.error());
  }

  fs::path parsed_path;
  if (!logical_path.empty()) {
    std::expected<fs::path, std::string> parsed =
        detail::parse_logical_path({.value = logical_path, .locator = source});
    if (!parsed) {
      return std::unexpected(parsed.error());
    }
    parsed_path = std::move(*parsed);
  }
  // An empty logical path (bare `<type>/`) targets the type root directory.

  return Locator{
      .kind =
          directory ? LocatorKind::ContextDirectory : LocatorKind::ContextFile,
      .type = type,
      .path = std::move(parsed_path),
      .version = std::move(*version),
      .value = {},
  };
}

[[nodiscard]] inline std::string context_ref_name(const Locator &locator) {
  if (!locator.type.has_value()) {
    return {};
  }
  const std::string type(to_string(*locator.type));
  switch (locator.version.kind) {
  case ContextVersionKind::Latest:
    return std::format("{}/latest", type);
  case ContextVersionKind::Generation:
    return std::format("{}/generations/{}", type, locator.version.value);
  case ContextVersionKind::Ref:
    return std::format("{}/{}", type, locator.version.value);
  }
  return {};
}

// Clips text to at most max_cells codepoints, appending U+2026 when
// clipped. Width is counted in codepoints rather than terminal cells;
// container clipping stays the backstop for wide glyphs.
[[nodiscard]] inline std::string
ellipsize(std::string_view text, std::size_t max_cells) {
  if (max_cells == 0) {
    return {};
  }
  std::size_t total = 0;
  for (const char ch : text) {
    if (!detail::continuation_byte(static_cast<unsigned char>(ch))) {
      ++total;
    }
  }
  if (total <= max_cells) {
    return std::string(text);
  }
  std::size_t kept = 0;
  std::size_t cut = 0;
  for (std::size_t index = 0; index < text.size() && kept < max_cells - 1;
       ++index) {
    if (detail::continuation_byte(static_cast<unsigned char>(text[index]))) {
      continue;
    }
    ++kept;
    cut = index + 1;
    while (
        cut < text.size()
        && detail::continuation_byte(static_cast<unsigned char>(text[cut]))) {
      ++cut;
    }
  }
  return std::string(text.substr(0, cut)) + "…";
}

[[nodiscard]] inline std::string
generation_hub_text(const ContextGeneration &generation, bool pinned) {
  return normalize_line_breaks(std::format(
      "gen {} · {}", short_id(generation.id), pinned ? "pinned" : "latest"));
}

[[nodiscard]] inline std::vector<std::string>
quest_freshness_input_lines(const QuestFreshnessInput &input) {
  const std::string current = input.current_generation.has_value()
                                ? *input.current_generation
                                : "missing";
  return split_display_lines(std::format(
      "{} · recorded {} · current {} · {} · {}", input.locator,
      short_id(input.recorded_generation), short_id(current),
      input.pinned ? "pinned" : "ref", input.fresh ? "fresh" : "stale"));
}

struct Provenance {
  std::string blob_id;
  std::string sha256;
  std::uintmax_t size_bytes = 0;
  std::string created_at;
  std::optional<std::string> generation;
  std::optional<std::string> parent_generation;
  std::optional<std::string> locator;
  std::optional<std::string> source_generation;
};

[[nodiscard]] inline std::vector<std::string>
provenance_lines(const Provenance &provenance) {
  std::vector<std::string> lines{
      std::format("blob: {}", provenance.blob_id),
      std::format("sha256: {}", provenance.sha256),
      std::format("size: {}", format_size(provenance.size_bytes)),
      std::format("created_at: {}", provenance.created_at),
  };
  if (provenance.generation.has_value()) {
    lines.push_back(std::format("generation: {}", *provenance.generation));
  }
  if (provenance.parent_generation.has_value()) {
    lines.push_back(std::format("parent: {}", *provenance.parent_generation));
  }
  if (provenance.locator.has_value()) {
    std::vector<std::string> locator_lines =
        split_labeled_lines({.label = "locator", .value = *provenance.locator});
    std::ranges::move(locator_lines, std::back_inserter(lines));
  }
  if (provenance.source_generation.has_value()) {
    lines.push_back(
        std::format("source_generation: {}", *provenance.source_generation));
  }
  return lines;
}

struct DecodedContent {
  std::vector<std::string> lines;
  std::uintmax_t byte_count = 0;
  bool binary = false;
  bool truncated = false;
};

[[nodiscard]] constexpr std::size_t
bounded_content_read_size(std::uintmax_t total_size_bytes) noexcept {
  return static_cast<std::size_t>(std::min<std::uintmax_t>(
      total_size_bytes, static_cast<std::uintmax_t>(kContentLimitBytes)));
}

[[nodiscard]] inline DecodedContent
decode_content(std::string_view content, std::uintmax_t total_size_bytes) {
  std::size_t kept_size =
      std::min(content.size(), bounded_content_read_size(total_size_bytes));
  const bool truncated =
      total_size_bytes > static_cast<std::uintmax_t>(kept_size);
  if (truncated) {
    kept_size =
        detail::trim_incomplete_utf8_suffix(content.substr(0, kept_size));
  }
  const std::string_view preview = content.substr(0, kept_size);
  if (!detail::valid_utf8(preview)) {
    return DecodedContent{
        .lines = {std::format("Binary data, {} bytes", total_size_bytes)},
        .byte_count = total_size_bytes,
        .binary = true,
        .truncated = false,
    };
  }

  std::vector<std::string> lines = detail::split_lines(preview);
  if (truncated) {
    lines.emplace_back(kTruncatedMarker);
  }
  return DecodedContent{
      .lines = std::move(lines),
      .byte_count = total_size_bytes,
      .binary = false,
      .truncated = truncated,
  };
}

[[nodiscard]] inline DecodedContent decode_content(std::string_view content) {
  return decode_content(content, content.size());
}

} // namespace memix::tui
