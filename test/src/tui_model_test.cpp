#include "bin/tui_model.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <limits>
#include <optional>
#include <string>
#include <vector>

namespace fs = memix::fs;
namespace tui = memix::tui;

TEST_CASE("row windows clamp bottom sentinels before virtualization",
          "[tui-model]") {
  constexpr std::size_t content_rows = 1'000;
  constexpr std::size_t viewport_rows = 20;
  constexpr std::size_t overscan_rows = 2;
  const tui::RowWindow content = tui::virtualized_row_window({
      .total_rows = content_rows,
      .viewport_rows = viewport_rows,
      .requested_top_row = std::numeric_limits<int>::max() / 2,
      .overscan = overscan_rows,
  });

  REQUIRE(content.top == 980);
  REQUIRE(content.first == 978);
  REQUIRE(content.last == content_rows);

  constexpr std::size_t list_rows = std::size_t{100} * 3;
  REQUIRE(tui::max_scroll_top(list_rows, viewport_rows) == 280);
  REQUIRE(tui::max_scroll_top(10, viewport_rows) == 0);
}

TEST_CASE("column windows preserve exact fits", "[tui-model]") {
  constexpr std::array stack{
      tui::ColumnKind::List,
      tui::ColumnKind::List,
      tui::ColumnKind::Content,
  };
  const tui::ColumnWindow window = tui::column_window(stack, 104);

  REQUIRE_FALSE(window.single_screen);
  REQUIRE(window.first_visible == 0);
  REQUIRE(window.widths == std::vector<std::size_t>{30, 30, 40});
}

TEST_CASE("column windows trim farthest ancestors first", "[tui-model]") {
  constexpr std::array stack{
      tui::ColumnKind::List,
      tui::ColumnKind::List,
      tui::ColumnKind::List,
      tui::ColumnKind::List,
  };
  const tui::ColumnWindow window = tui::column_window(stack, 96);

  REQUIRE_FALSE(window.single_screen);
  REQUIRE(window.first_visible == 1);
  REQUIRE(window.widths == std::vector<std::size_t>{30, 30, 30});
}

TEST_CASE("column windows reserve the minimum content width",
          "[tui-model]") {
  constexpr std::array stack{
      tui::ColumnKind::List,
      tui::ColumnKind::List,
      tui::ColumnKind::List,
      tui::ColumnKind::Content,
  };
  const tui::ColumnWindow window = tui::column_window(stack, 101);

  REQUIRE_FALSE(window.single_screen);
  REQUIRE(window.first_visible == 2);
  REQUIRE(window.widths == std::vector<std::size_t>{30, 69});
}

TEST_CASE("column windows use a single screen on narrow terminals",
          "[tui-model]") {
  constexpr std::array stack{
      tui::ColumnKind::List,
      tui::ColumnKind::List,
      tui::ColumnKind::Content,
  };
  const tui::ColumnWindow window = tui::column_window(stack, 79);

  REQUIRE(window.single_screen);
  REQUIRE(window.first_visible == 2);
  REQUIRE(window.widths == std::vector<std::size_t>{79});
}

TEST_CASE("content columns occupy the flexible remainder", "[tui-model]") {
  constexpr std::array stack{
      tui::ColumnKind::List,
      tui::ColumnKind::List,
      tui::ColumnKind::Content,
  };
  const tui::ColumnWindow window = tui::column_window(stack, 120);

  REQUIRE_FALSE(window.single_screen);
  REQUIRE(window.first_visible == 0);
  REQUIRE(window.widths == std::vector<std::size_t>{30, 30, 56});
}

TEST_CASE(
    "catalog rows contain direct children in directory-first natural order",
    "[tui-model]") {
  const std::vector<memix::StoreCatalogEntry> entries{
      {.type = memix::MemoryObjectType::Rules,
       .context_path = "repo/file10.md",
       .directory = false,
       .file_count = 1,
       .total_size_bytes = 2048,
       .latest_created_at = "2026-08-17T10:00:00Z",
       .blob_id = "aaaaaaaaaaaa1111"},
      {.type = memix::MemoryObjectType::Rules,
       .context_path = "repo/Dir10",
       .directory = true,
       .file_count = 2,
       .total_size_bytes = 1024,
       .latest_created_at = "2026-08-17T09:00:00Z",
       .blob_id = {}},
      {.type = memix::MemoryObjectType::Rules,
       .context_path = "repo",
       .directory = true,
       .file_count = 6,
       .total_size_bytes = 5000,
       .latest_created_at = "2026-08-17T10:00:00Z",
       .blob_id = {}},
      {.type = memix::MemoryObjectType::Rules,
       .context_path = "repo/dir2",
       .directory = true,
       .file_count = 1,
       .total_size_bytes = 12,
       .latest_created_at = "2026-08-17T08:00:00Z",
       .blob_id = {}},
      {.type = memix::MemoryObjectType::Rules,
       .context_path = "repo/file2.md",
       .directory = false,
       .file_count = 1,
       .total_size_bytes = 7,
       .latest_created_at = "2026-08-17T07:00:00Z",
       .blob_id = "bbbbbbbbbbbb2222"},
      {.type = memix::MemoryObjectType::Rules,
       .context_path = "repo/dir2/nested.md",
       .directory = false,
       .file_count = 1,
       .total_size_bytes = 12,
       .latest_created_at = "2026-08-17T08:00:00Z",
       .blob_id = "cccccccccccc3333"},
  };

  const std::vector<tui::ListRow> rows =
      tui::catalog_rows(entries, fs::path("repo"));
  REQUIRE(rows.size() == 4);
  REQUIRE(rows[0].title == "dir2/");
  REQUIRE(rows[0].summary == "1 file · 12 B");
  REQUIRE(rows[1].title == "Dir10/");
  REQUIRE(rows[1].summary == "2 files · 1 KB");
  REQUIRE(rows[2].title == "file2.md");
  REQUIRE(rows[2].summary == "blob bbbbbbbbbbbb · 7 B · 2026-08-17T07:00:00Z");
  REQUIRE(rows[3].title == "file10.md");
  REQUIRE(rows[3].summary == "blob aaaaaaaaaaaa · 2 KB · 2026-08-17T10:00:00Z");
}

TEST_CASE("catalog directory validation rejects file locators",
          "[tui-model]") {
  const std::vector<memix::StoreCatalogEntry> entries{
      {.type = memix::MemoryObjectType::Rules,
       .context_path = "repo/file.txt",
       .directory = false,
       .file_count = 1,
       .total_size_bytes = 7,
       .latest_created_at = "2026-08-17T07:00:00Z",
       .blob_id = "aaaaaaaaaaaa1111"},
      {.type = memix::MemoryObjectType::Rules,
       .context_path = "repo",
       .directory = true,
       .file_count = 1,
       .total_size_bytes = 7,
       .latest_created_at = "2026-08-17T07:00:00Z",
       .blob_id = {}},
  };

  const tui::CatalogDirectoryResult directory =
      tui::catalog_directory_file_count(entries, "repo");
  REQUIRE(directory);
  REQUIRE(*directory == 1);

  const tui::CatalogDirectoryResult file =
      tui::catalog_directory_file_count(entries, "repo/file.txt");
  REQUIRE_FALSE(file);
  REQUIRE(file.error() == "context path is not a directory `repo/file.txt`");

  REQUIRE_FALSE(tui::catalog_directory_file_count(entries, "missing"));
  REQUIRE(tui::catalog_directory_file_count({}, {}).value() == 0);
}

TEST_CASE("environment trees derive from resolved files without view reads",
          "[tui-model]") {
  const auto resolved_file = [](fs::path path, std::string id,
                                std::uintmax_t size_bytes) {
    memix::ResolvedViewFile file;
    file.output_path = std::move(path);
    file.blob.id = std::move(id);
    file.blob.size_bytes = size_bytes;
    return file;
  };
  const std::vector<memix::ResolvedViewFile> files{
      resolved_file("rules/team/file10.md", "aaaaaaaaaaaa1111", 2048),
      resolved_file("rules/team/file2.md", "bbbbbbbbbbbb2222", 7),
      resolved_file("rules/root\nfile.md", "cccccccccccc3333", 12),
      resolved_file("chat/session.md", "dddddddddddd4444", 20),
  };

  const tui::EnvironmentTreeSnapshot root =
      tui::environment_tree_snapshot(files);
  REQUIRE(root.path_exists);
  REQUIRE(root.file_count == 4);
  REQUIRE(root.rows.size() == 2);
  REQUIRE(root.rows[0].title == "chat/");
  REQUIRE(root.rows[0].summary == "1 file · 20 B");
  REQUIRE(root.rows[1].title == "rules/");
  REQUIRE(root.rows[1].summary == "3 files · 2 KB");

  const tui::EnvironmentTreeSnapshot rules =
      tui::environment_tree_snapshot(files, "rules");
  REQUIRE(rules.path_exists);
  REQUIRE(rules.file_count == 3);
  REQUIRE(rules.rows.size() == 2);
  REQUIRE(rules.rows[0].title == "team/");
  REQUIRE(rules.rows[0].summary == "2 files · 2 KB");
  REQUIRE(rules.rows[1].title == "root file.md");
  REQUIRE(rules.rows[1].blob_id == "cccccccccccc3333");

  const tui::EnvironmentTreeSnapshot missing =
      tui::environment_tree_snapshot(files, "documents");
  REQUIRE_FALSE(missing.path_exists);
  REQUIRE(missing.file_count == 0);
  REQUIRE(missing.rows.empty());
}

TEST_CASE("tui locators classify context and inspector targets",
          "[tui-model]") {
  const auto latest_file = tui::parse_locator("rules/AGENTS.md");
  REQUIRE(latest_file);
  REQUIRE(latest_file->kind == tui::LocatorKind::ContextFile);
  REQUIRE(latest_file->type == memix::MemoryObjectType::Rules);
  REQUIRE(latest_file->path == fs::path("AGENTS.md"));
  REQUIRE(latest_file->version.kind == tui::ContextVersionKind::Latest);
  REQUIRE(tui::context_ref_name(*latest_file) == "rules/latest");

  const auto type_root = tui::parse_locator("rules/");
  REQUIRE(type_root);
  REQUIRE(type_root->kind == tui::LocatorKind::ContextDirectory);
  REQUIRE(type_root->type == memix::MemoryObjectType::Rules);
  REQUIRE(type_root->path.empty());
  REQUIRE(type_root->version.kind == tui::ContextVersionKind::Latest);

  const auto latest_directory =
      tui::parse_locator("chat/sessions/2026/@latest");
  REQUIRE(latest_directory);
  REQUIRE(latest_directory->kind == tui::LocatorKind::ContextDirectory);
  REQUIRE(latest_directory->path == fs::path("sessions/2026"));

  const auto generation =
      tui::parse_locator("documents/notes/design.md@generation/000007");
  REQUIRE(generation);
  REQUIRE(generation->version.kind == tui::ContextVersionKind::Generation);
  REQUIRE(generation->version.value == "000007");
  REQUIRE_FALSE(generation->version.direct_generation);
  REQUIRE(tui::context_ref_name(*generation) == "documents/generations/000007");

  const std::string generation_id(64, 'a');
  const auto raw_generation =
      tui::parse_locator("rules/repo/AGENTS.md@" + generation_id);
  REQUIRE(raw_generation);
  REQUIRE(raw_generation->version.kind == tui::ContextVersionKind::Generation);
  REQUIRE(raw_generation->version.value == generation_id);
  REQUIRE(raw_generation->version.direct_generation);

  const auto sha256_generation =
      tui::parse_locator("rules/repo/AGENTS.md@sha256:" + generation_id);
  REQUIRE(sha256_generation);
  REQUIRE(sha256_generation->version.kind
          == tui::ContextVersionKind::Generation);
  REQUIRE(sha256_generation->version.value == generation_id);
  REQUIRE(sha256_generation->version.direct_generation);

  const auto named_ref =
      tui::parse_locator("rules/repo/AGENTS.md@ref/stable/team");
  REQUIRE(named_ref);
  REQUIRE(named_ref->version.kind == tui::ContextVersionKind::Ref);
  REQUIRE(named_ref->version.value == "stable/team");
  REQUIRE(tui::context_ref_name(*named_ref) == "rules/stable/team");

  const auto object = tui::parse_locator("objects/abc123");
  REQUIRE(object);
  REQUIRE(object->kind == tui::LocatorKind::Object);
  REQUIRE(object->value == "abc123");

  const auto environment = tui::parse_locator("environments/def456");
  REQUIRE(environment);
  REQUIRE(environment->kind == tui::LocatorKind::Environment);

  const auto quest =
      tui::parse_locator("quests/rules/repo/AGENTS.md@latest");
  REQUIRE(quest);
  REQUIRE(quest->kind == tui::LocatorKind::Quest);
  REQUIRE(quest->value == "rules/repo/AGENTS.md@latest");
}

TEST_CASE("tui locators reject malformed targets", "[tui-model]") {
  REQUIRE_FALSE(tui::parse_locator(""));
  REQUIRE_FALSE(tui::parse_locator("unknown/file.md"));
  REQUIRE_FALSE(tui::parse_locator("rules"));
  REQUIRE_FALSE(tui::parse_locator("rules/a/../b"));
  REQUIRE_FALSE(tui::parse_locator("rules//"));
  REQUIRE_FALSE(tui::parse_locator("rules//@latest"));
  REQUIRE_FALSE(tui::parse_locator("rules/team//"));
  REQUIRE_FALSE(tui::parse_locator("rules/a@branch"));
  REQUIRE_FALSE(tui::parse_locator("rules/a@generation/"));
  REQUIRE_FALSE(tui::parse_locator("objects/XYZ"));
  REQUIRE_FALSE(tui::parse_locator("environments/"));
  REQUIRE_FALSE(tui::parse_locator("quests/"));
}

TEST_CASE("generation hub and provenance formatting remain stable",
          "[tui-model]") {
  const memix::ContextGeneration generation{
      .schema_version = 1,
      .object_version = 1,
      .type = memix::MemoryObjectType::Rules,
      .id = "0123456789abcdef",
      .root_tree = "tree",
      .parent = "fedcba9876543210",
      .created_at = "2026-08-17T12:00:00Z",
      .message = "initial import",
      .object_path = {},
  };
  REQUIRE(tui::generation_hub_text(generation, false)
          == "gen 0123456789ab · latest");
  REQUIRE(tui::generation_hub_text(generation, true)
          == "gen 0123456789ab · pinned");

  const std::vector<std::string> lines =
      tui::provenance_lines(tui::Provenance{
          .blob_id = "blob-id",
          .sha256 = "hash-value",
          .size_bytes = 1536,
          .created_at = "2026-08-17T12:00:00Z",
          .generation = "generation-id",
          .parent_generation = "parent-id",
          .locator = "rules/repo/AGENTS.md@latest",
          .source_generation = "source-id",
      });
  REQUIRE(lines
          == std::vector<std::string>{
              "blob: blob-id",
              "sha256: hash-value",
              "size: 1.5 KB",
              "created_at: 2026-08-17T12:00:00Z",
              "generation: generation-id",
              "parent: parent-id",
              "locator: rules/repo/AGENTS.md@latest",
              "source_generation: source-id",
          });
}

TEST_CASE("fixed-height labels normalize line breaks", "[tui-model]") {
  REQUIRE(tui::normalize_line_breaks("one\r\ntwo\nthree\rfour")
          == "one two three four");

  memix::ContextGeneration generation;
  generation.id = "0123456789abcdef";
  generation.created_at = "2026-08-17T12:00:00Z";
  generation.message = "first line\nsecond line";
  REQUIRE(tui::generation_hub_text(generation, false)
          == "gen 0123456789ab · latest");
}

TEST_CASE("multiline metadata splits into one virtual row per line",
          "[tui-model]") {
  REQUIRE(tui::split_display_lines("first\nsecond\r\nthird\rfourth")
          == std::vector<std::string>{"first", "second", "third", "fourth"});
  REQUIRE(tui::split_labeled_lines(
              {.label = "intent", .value = "first\nsecond\r\nthird\rfourth"})
          == std::vector<std::string>{
              "intent: first",
              "        second",
              "        third",
              "        fourth",
          });
  REQUIRE(tui::split_labeled_lines({.label = "name", .value = ""})
          == std::vector<std::string>{"name: "});

  const memix::QuestFreshnessInput freshness_input{
      .locator = "rules/team\nAGENTS.md@latest",
      .recorded_generation = "0123456789abcdef",
      .current_generation = "fedcba9876543210",
      .pinned = false,
      .fresh = false,
  };
  REQUIRE(tui::quest_freshness_input_lines(freshness_input)
          == std::vector<std::string>{
              "rules/team",
              "AGENTS.md@latest · recorded 0123456789ab · current "
              "fedcba987654 · ref · stale",
          });

  const std::vector<std::string> provenance = tui::provenance_lines({
      .blob_id = "blob-id",
      .sha256 = "hash-value",
      .size_bytes = 1,
      .created_at = "2026-08-17T12:00:00Z",
      .generation = std::nullopt,
      .parent_generation = std::nullopt,
      .locator = "rules/team\nAGENTS.md@latest",
      .source_generation = std::nullopt,
  });
  REQUIRE(provenance
          == std::vector<std::string>{
              "blob: blob-id",
              "sha256: hash-value",
              "size: 1 B",
              "created_at: 2026-08-17T12:00:00Z",
              "locator: rules/team",
              "         AGENTS.md@latest",
          });
}

TEST_CASE("ellipsize clips by codepoint with an ellipsis marker",
          "[tui-model]") {
  REQUIRE(tui::ellipsize("short", 10) == "short");
  REQUIRE(tui::ellipsize("exact", 5) == "exact");
  REQUIRE(tui::ellipsize("exactly", 5) == "exac…");
  REQUIRE(tui::ellipsize("generation 0123456789ab (latest)", 12)
          == "generation …");
  REQUIRE(tui::ellipsize("aé世界😀z", 4) == "aé世…");
  const std::string long_utf8_label = std::string(56, 'x') + "世界😀éz";
  REQUIRE(tui::ellipsize(long_utf8_label, 60)
          == std::string(56, 'x') + "世界😀…");
  REQUIRE(tui::ellipsize("anything", 1) == "…");
  REQUIRE(tui::ellipsize("anything", 0).empty());
}

TEST_CASE("backspace erases one complete UTF-8 code point", "[tui-model]") {
  std::string input = "aé世界😀";
  tui::erase_last_utf8_code_point(input);
  REQUIRE(input == "aé世界");
  tui::erase_last_utf8_code_point(input);
  REQUIRE(input == "aé世");
  tui::erase_last_utf8_code_point(input);
  REQUIRE(input == "aé");
  tui::erase_last_utf8_code_point(input);
  REQUIRE(input == "a");
  tui::erase_last_utf8_code_point(input);
  REQUIRE(input.empty());
  tui::erase_last_utf8_code_point(input);
  REQUIRE(input.empty());
}

TEST_CASE("row filtering searches title and summary without case",
          "[tui-model]") {
  const tui::ListRow row{
      .kind = tui::RowKind::File,
      .path = "Design.md",
      .title = "Design.md",
      .summary = "blob abcdef · 2 KB · TODAY",
      .blob_id = "abcdef",
  };
  REQUIRE(tui::fields_match(row.title, row.summary, "design"));
  REQUIRE(tui::fields_match(row.title, row.summary, "ABCDEF"));
  REQUIRE(tui::fields_match(row.title, row.summary, "today"));
  REQUIRE(tui::fields_match(row.title, row.summary, ""));
  REQUIRE_FALSE(tui::fields_match(row.title, row.summary, "missing"));
}

TEST_CASE("pluralize picks singular and plural forms", "[tui-model]") {
  REQUIRE(tui::pluralize(0, "file", "files") == "0 files");
  REQUIRE(tui::pluralize(1, "file", "files") == "1 file");
  REQUIRE(tui::pluralize(2, "environment", "environments")
          == "2 environments");
  REQUIRE(tui::pluralized_files(1) == "1 file");
  REQUIRE(tui::pluralized_files(3) == "3 files");
}

TEST_CASE("content decoding distinguishes UTF-8 and caps text previews",
          "[tui-model]") {
  const tui::DecodedContent text = tui::decode_content("hello\n世界");
  REQUIRE_FALSE(text.binary);
  REQUIRE_FALSE(text.truncated);
  REQUIRE(text.lines == std::vector<std::string>{"hello", "世界"});

  std::string invalid{"valid"};
  invalid.push_back(static_cast<char>(0xFF));
  const tui::DecodedContent binary = tui::decode_content(invalid);
  REQUIRE(binary.binary);
  REQUIRE(binary.lines == std::vector<std::string>{"Binary data, 6 bytes"});

  std::string oversized(tui::kContentLimitBytes + 5, 'x');
  const tui::DecodedContent truncated = tui::decode_content(oversized);
  REQUIRE_FALSE(truncated.binary);
  REQUIRE(truncated.truncated);
  REQUIRE(truncated.byte_count == oversized.size());
  REQUIRE(truncated.lines.back() == std::string(tui::kTruncatedMarker));
  REQUIRE(truncated.lines.front().size() == tui::kContentLimitBytes);

  std::string split_codepoint(tui::kContentLimitBytes - 1, 'x');
  split_codepoint += "€tail";
  const tui::DecodedContent boundary =
      tui::decode_content(split_codepoint);
  REQUIRE(boundary.truncated);
  REQUIRE(boundary.lines.front().size() == tui::kContentLimitBytes - 1);
  REQUIRE(boundary.lines.back() == std::string(tui::kTruncatedMarker));

  REQUIRE(tui::bounded_content_read_size(12) == 12);
  REQUIRE(tui::bounded_content_read_size(tui::kContentLimitBytes + 100)
          == tui::kContentLimitBytes);
  const std::string bounded_prefix(tui::kContentLimitBytes, 'x');
  const tui::DecodedContent bounded = tui::decode_content(
      bounded_prefix, tui::kContentLimitBytes + 100);
  REQUIRE(bounded.byte_count == tui::kContentLimitBytes + 100);
  REQUIRE(bounded.truncated);
  REQUIRE(bounded.lines.front().size() == tui::kContentLimitBytes);
  REQUIRE(bounded.lines.back() == std::string(tui::kTruncatedMarker));

  std::string split_prefix(tui::kContentLimitBytes - 1, 'x');
  split_prefix += "\xE2";
  const tui::DecodedContent bounded_boundary =
      tui::decode_content(split_prefix, tui::kContentLimitBytes + 2);
  REQUIRE_FALSE(bounded_boundary.binary);
  REQUIRE(bounded_boundary.truncated);
  REQUIRE(bounded_boundary.lines.front().size()
          == tui::kContentLimitBytes - 1);
  REQUIRE(bounded_boundary.lines.back() == std::string(tui::kTruncatedMarker));
}

TEST_CASE("default edit messages identify the operation and logical path",
          "[tui-model]") {
  REQUIRE(tui::default_edit_message(tui::EditOperation::Edit,
                                        memix::MemoryObjectType::Rules,
                                        "repo/AGENTS.md")
          == "edit rules/repo/AGENTS.md");
  REQUIRE(tui::default_edit_message(tui::EditOperation::Add,
                                        memix::MemoryObjectType::Chat,
                                        "sessions/new.md")
          == "add chat/sessions/new.md");
  REQUIRE(tui::default_edit_message(tui::EditOperation::Remove,
                                        memix::MemoryObjectType::Documents,
                                        "notes/old.md")
          == "remove documents/notes/old.md");
}

TEST_CASE("new file names are relative non-empty logical paths",
          "[tui-model]") {
  const tui::NewFileNameResult simple =
      tui::validate_new_file_name("new.md");
  REQUIRE(simple);
  REQUIRE(*simple == fs::path("new.md"));

  const tui::NewFileNameResult nested =
      tui::validate_new_file_name("team/new.md");
  REQUIRE(nested);
  REQUIRE(*nested == fs::path("team/new.md"));

  REQUIRE_FALSE(tui::validate_new_file_name(""));
  REQUIRE_FALSE(tui::validate_new_file_name("/absolute.md"));
  REQUIRE_FALSE(tui::validate_new_file_name("../outside.md"));
  REQUIRE_FALSE(tui::validate_new_file_name("team/../outside.md"));
  REQUIRE_FALSE(tui::validate_new_file_name("team//new.md"));
  REQUIRE_FALSE(tui::validate_new_file_name("team/new.md/"));
  REQUIRE_FALSE(tui::validate_new_file_name("team\\new.md"));
  REQUIRE_FALSE(tui::validate_new_file_name("./new.md"));
}

TEST_CASE("unchanged content detection compares exact bytes",
          "[tui-model]") {
  REQUIRE(tui::content_unchanged("same\n", "same\n"));
  REQUIRE(tui::content_unchanged({}, {}));
  REQUIRE_FALSE(tui::content_unchanged("same", "same\n"));
  REQUIRE_FALSE(tui::content_unchanged("one\r\n", "one\n"));

  const std::string original{"a\0b", 3};
  const std::string edited{"a\0c", 3};
  REQUIRE_FALSE(tui::content_unchanged(original, edited));
}
