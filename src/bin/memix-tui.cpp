#include "detail/scope_exit.hpp"
#include "tui_model.hpp"
#include "memix/error.hpp"
#include "memix/fs.hpp"
#include "memix/store.hpp"
#include "memix/version.hpp"
#include "memix/view.hpp"

#include <algorithm>
#include <argum/data.h>
#include <argum/expected.h>
#include <argum/parser.h>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <expected>
#include <format>
#include <fstream>
#include <ftxui/component/app.hpp>
#include <ftxui/component/component.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/component/mouse.hpp>
#include <ftxui/component/task.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/dom/node.hpp>
#include <ftxui/screen/box.hpp>
#include <ftxui/screen/color.hpp>
#include <ftxui/screen/screen.hpp>
#include <ftxui/screen/terminal.hpp>
#include <ftxui/util/autoreset.hpp>
#include <iostream>
#include <iterator>
#include <limits>
#include <memory>
#include <optional>
#include <ranges>
#include <set>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

namespace fs = memix::fs;

namespace {

constexpr std::size_t kPageMove = 10;
constexpr int kRowsPerEntry = 3;
constexpr int kPercent = 100;
constexpr std::size_t kTabWidth = 8;
constexpr int kDeleteDialogWidth = 76;

struct CliOptions {
  fs::path store_root;
  std::string locator;
  bool help_requested = false;
  bool version_requested = false;
};

struct CliParseResult {
  CliOptions options;
  std::string error;
  std::string help;
};

CliParseResult parse_cli_args(int argc, char **argv) {
  // Fixed program name (not argv[0]) so help output matches the sibling
  // CLIs and the release smoke tests regardless of the invocation path.
  const char *program_name = "memix-tui";
  CliParseResult result;
  Argum::Parser parser;
  parser.add(Argum::Option("--help", "-h")
                 .help("Print this help message and exit")
                 .handler([&result] { result.options.help_requested = true; }));
  parser.add(
      Argum::Option("--version")
          .help("Display program version information and exit")
          .handler([&result] { result.options.version_requested = true; }));
  parser.add(Argum::Option("--store-root")
                 .argName("PATH")
                 .help("Store root. Defaults to MEMIX_STORE_HOME, "
                       "XDG_DATA_HOME/memix, or HOME/.memix.")
                 .handler([&result](std::string_view value) {
                   result.options.store_root = fs::path(value);
                 }));
  parser.add(
      Argum::Positional("locator")
          .occurs(Argum::neverOrOnce)
          .help("Optional context, object, environment, or quest locator")
          .handler([&result](std::string_view value) {
            result.options.locator = value;
          }));

  const Argum::Expected<void> parsed = parser.parse(argc, argv);
  if (const auto parse_error = parsed.error()) {
    result.error = parse_error->message();
    result.help = parser.formatUsage(program_name);
    return result;
  }
  if (result.options.help_requested) {
    result.help = parser.formatHelp(program_name);
    return result;
  }
  return result;
}

struct ScrollState {
  int top_row = 0;
  int max_top_row = 0;
  bool follow_focus = true;
  int viewport_rows = 0;
};

void follow_selection(ScrollState &scroll) { scroll.follow_focus = true; }

int
centered_top_row(int focus_first_row, int focus_last_row, int viewport_rows) {
  const int focus_rows = focus_last_row - focus_first_row + 1;
  if (focus_rows >= viewport_rows) {
    return focus_first_row;
  }
  const int centered = focus_first_row
                     - ((viewport_rows - 1) / 2)
                     + ((focus_last_row - focus_first_row) / 2);
  return std::clamp(centered, focus_last_row - viewport_rows + 1,
                    focus_first_row);
}

int max_top_row_for(int total_rows, int viewport_rows) {
  return static_cast<int>(memix::tui::max_scroll_top(
      static_cast<std::size_t>(std::max(total_rows, 0)),
      static_cast<std::size_t>(std::max(viewport_rows, 0))));
}

// The two row values deliberately mirror Loupe's scrolling primitive.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
void update_scroll_layout(ScrollState &scroll, int focused_top_row,
                          int max_top_row) {
  scroll.max_top_row = std::max(0, max_top_row);
  if (scroll.follow_focus) {
    scroll.top_row = std::clamp(focused_top_row, 0, scroll.max_top_row);
    return;
  }
  scroll.top_row = std::clamp(scroll.top_row, 0, scroll.max_top_row);
}

void scroll_by_rows(ScrollState &scroll, int rows) {
  scroll.follow_focus = false;
  const int clamped_rows =
      std::clamp(rows, -scroll.top_row, scroll.max_top_row - scroll.top_row);
  scroll.top_row += clamped_rows;
}

void scroll_to_top(ScrollState &scroll) {
  scroll.follow_focus = false;
  scroll.top_row = 0;
}

void scroll_to_bottom(ScrollState &scroll) {
  scroll.follow_focus = false;
  scroll.top_row = std::numeric_limits<int>::max() / 2;
}

int scroll_progress_percent(const ScrollState &scroll) {
  if (scroll.max_top_row <= 0) {
    return kPercent;
  }
  const int top_row = std::clamp(scroll.top_row, 0, scroll.max_top_row);
  return static_cast<int>(
      static_cast<long long>(top_row) * kPercent / scroll.max_top_row);
}

class LineFrame final : public ftxui::Node {
public:
  LineFrame(ftxui::Element child, ScrollState &scroll)
      : Node(ftxui::Elements{std::move(child)}), scroll_(scroll) {}

  void ComputeRequirement() override {
    ftxui::Node::ComputeRequirement();
    if (!scroll_.follow_focus) {
      requirement_.focused.enabled = false;
    }
  }

  void SetBox(ftxui::Box box) override {
    ftxui::Node::SetBox(box);
    pending_box_ = box;
    const int external_height = box.y_max - box.y_min;
    scroll_.viewport_rows = external_height + 1;
    pending_internal_height_ = std::max(requirement_.min_y, external_height);
    pending_max_top_row_ =
        max_top_row_for(pending_internal_height_, scroll_.viewport_rows);
    pending_focused_top_row_ = 0;
    if (requirement_.focused.enabled) {
      const auto &focused = requirement_.focused.box;
      pending_focused_top_row_ =
          centered_top_row(focused.y_min, focused.y_max, scroll_.viewport_rows);
    }
    layout_pending_ = true;
    set_child_box();
  }

  void Check(ftxui::Node::Status *status) override {
    ftxui::Node::Status child_status;
    child_status.iteration = status->iteration;
    ftxui::Node::Check(&child_status);
    status->need_iteration |= child_status.need_iteration;
    if (!layout_pending_ || child_status.need_iteration) {
      return;
    }
    update_scroll_layout(scroll_, pending_focused_top_row_,
                         pending_max_top_row_);
    set_child_box();
    layout_pending_ = false;
  }

  void Render(ftxui::Screen &screen) override {
    const ftxui::AutoReset<ftxui::Box> stencil(
        &screen.stencil, ftxui::Box::Intersection(box_, screen.stencil));
    children_[0]->Render(screen);
  }

private:
  void set_child_box() {
    ftxui::Box child_box = pending_box_;
    child_box.y_min = pending_box_.y_min - scroll_.top_row;
    child_box.y_max =
        pending_box_.y_min + pending_internal_height_ - scroll_.top_row;
    children_[0]->SetBox(child_box);
  }

  ScrollState &scroll_;
  ftxui::Box pending_box_;
  int pending_internal_height_ = 0;
  int pending_focused_top_row_ = 0;
  int pending_max_top_row_ = 0;
  bool layout_pending_ = false;
};

class ScrollProgressIndicator final : public ftxui::Node {
public:
  explicit ScrollProgressIndicator(const ScrollState &scroll)
      : scroll_(scroll) {}

  void ComputeRequirement() override {
    requirement_.min_x = 4;
    requirement_.min_y = 1;
  }

  void Render(ftxui::Screen &screen) override {
    const auto label =
        ftxui::text(std::to_string(scroll_progress_percent(scroll_)) + "%")
        | ftxui::align_right;
    label->ComputeRequirement();
    label->SetBox(box_);
    label->Render(screen);
  }

private:
  const ScrollState &scroll_;
};

ftxui::Element line_frame(ftxui::Element content, ScrollState &scroll) {
  return std::make_shared<LineFrame>(std::move(content), scroll);
}

ftxui::Element scroll_progress_indicator(const ScrollState &scroll) {
  return std::make_shared<ScrollProgressIndicator>(scroll);
}

enum class ListKind : std::uint8_t {
  Store,
  TypeTree,
  Generations,
  Objects,
  EnvironmentList,
  EnvironmentTree,
  QuestList,
};

enum class EntryAction : std::uint8_t {
  OpenType,
  OpenObjects,
  OpenEnvironmentList,
  OpenQuestList,
  OpenGenerationList,
  OpenTypeDirectory,
  OpenTypeFile,
  SelectGeneration,
  OpenObject,
  OpenEnvironment,
  OpenEnvironmentDirectory,
  OpenEnvironmentFile,
  OpenQuest,
};

// These types are deliberately plain state records shared by the event and
// rendering paths.
// NOLINTBEGIN(misc-non-private-member-variables-in-classes)
struct Entry {
  Entry() = default;
  Entry(EntryAction entry_action, const std::string &entry_title,
        const std::string &entry_summary,
        memix::MemoryObjectType entry_type = memix::MemoryObjectType::Rules)
      : action(entry_action),
        title(memix::tui::normalize_line_breaks(entry_title)),
        summary(memix::tui::normalize_line_breaks(entry_summary)),
        type(entry_type) {}

  EntryAction action = EntryAction::OpenType;
  std::string title;
  std::string summary;
  memix::MemoryObjectType type = memix::MemoryObjectType::Rules;
  fs::path path;
  std::string id;
  std::optional<memix::QuestInfo> quest;
};

struct FilterState {
  bool active = false;
  std::string input;
  std::string query;
};

enum class FooterPromptKind : std::uint8_t { NewFileName, GenerationMessage };

struct FooterPrompt {
  FooterPromptKind kind = FooterPromptKind::NewFileName;
  std::string input;
  std::string fallback;
  memix::MemoryObjectType type = memix::MemoryObjectType::Rules;
  fs::path directory;
  std::optional<std::string> generation;
};

struct EditorRequest {
  memix::tui::EditOperation operation = memix::tui::EditOperation::Edit;
  memix::MemoryObjectType type = memix::MemoryObjectType::Rules;
  fs::path path;
  std::optional<std::string> generation;
};

struct PendingMutation {
  memix::tui::EditOperation operation = memix::tui::EditOperation::Edit;
  memix::MemoryObjectType type = memix::MemoryObjectType::Rules;
  fs::path path;
  std::optional<std::string> generation;
  fs::path temp_directory;
  fs::path temp_file;
};

struct DeleteConfirmation {
  memix::MemoryObjectType type = memix::MemoryObjectType::Rules;
  fs::path path;
  std::string blob_id;
  std::optional<std::string> generation;
};

struct ListScreen {
  explicit ListScreen(ListKind screen_kind = ListKind::Store)
      : kind(screen_kind) {}

  ListKind kind = ListKind::Store;
  std::string scope = "store";
  std::string count = "0 entries";
  std::vector<Entry> entries;
  std::vector<std::size_t> visible;
  std::size_t selected = 0;
  ScrollState scroll;
  FilterState filter;
  memix::MemoryObjectType type = memix::MemoryObjectType::Rules;
  fs::path path;
  std::optional<std::string> generation;
  bool pinned_to_non_latest = false;
  std::string environment_id;
};

enum class ContentKind : std::uint8_t {
  StoreFile,
  Object,
  EnvironmentFile,
  Quest,
};

struct SearchState {
  bool active = false;
  std::string input;
  std::string query;
  std::vector<std::size_t> matches;
  std::size_t match_index = 0;
  int origin_top_row = 0;
};

struct ContentScreen {
  explicit ContentScreen(ContentKind content_kind = ContentKind::StoreFile)
      : kind(content_kind) {}

  ContentKind kind = ContentKind::StoreFile;
  std::string title;
  std::string scope;
  std::string count = "0 lines";
  std::vector<std::string> lines;
  ScrollState scroll{.follow_focus = false};
  SearchState search;
  memix::MemoryObjectType type = memix::MemoryObjectType::Rules;
  fs::path path;
  std::optional<std::string> generation;
  bool pinned_to_non_latest = false;
  std::string blob_id;
  std::string object_id;
  std::string environment_id;
  std::optional<memix::QuestInfo> quest;
  std::optional<std::size_t> environment_line;
};

using TuiScreen = std::variant<ListScreen, ContentScreen>;

struct TuiState {
  explicit TuiState(fs::path store_root)
      : root(std::move(store_root)), store(root), views(store) {}

  fs::path root;
  memix::Store store;
  memix::ViewService views;
  std::vector<TuiScreen> stack;
  std::string status;
  std::vector<std::string> errors;
  bool show_errors = false;
  ScrollState error_scroll{.follow_focus = false};
  std::optional<FooterPrompt> prompt;
  std::optional<EditorRequest> editor_request;
  std::optional<PendingMutation> pending_mutation;
  std::optional<DeleteConfirmation> delete_confirmation;
};
// NOLINTEND(misc-non-private-member-variables-in-classes)

std::string clipped_label(std::string_view text) {
  constexpr std::size_t max_length = 60;
  return memix::tui::ellipsize(text, max_length);
}

using memix::tui::pluralize;

std::string expand_tabs(std::string_view value) {
  if (!value.contains('\t')) {
    return std::string(value);
  }
  std::string expanded;
  expanded.reserve(value.size());
  std::size_t column = 0;
  for (const char raw : value) {
    if (raw == '\t') {
      const std::size_t spaces = kTabWidth - (column % kTabWidth);
      expanded.append(spaces, ' ');
      column += spaces;
      continue;
    }
    expanded.push_back(raw);
    if (!memix::tui::detail::continuation_byte(
            static_cast<unsigned char>(raw))) {
      ++column;
    }
  }
  return expanded;
}

void record_error(TuiState &state, const std::string &message) {
  if (std::ranges::find(state.errors, message) == state.errors.end()) {
    state.errors.push_back(message);
  }
  state.status = "error: " + clipped_label(message);
}

std::string last_error(const TuiState &state) {
  if (!state.errors.empty()) {
    return state.errors.back();
  }
  return state.status.empty() ? "unable to open target" : state.status;
}

std::string list_query(const ListScreen &screen) {
  return screen.filter.active ? screen.filter.input : screen.filter.query;
}

bool entry_matches(const Entry &entry, std::string_view query) {
  return memix::tui::fields_match(entry.title, entry.summary, query);
}

void apply_filter(ListScreen &screen) {
  follow_selection(screen.scroll);
  screen.visible.clear();
  const std::string query = list_query(screen);
  for (std::size_t index = 0; index < screen.entries.size(); ++index) {
    if (entry_matches(screen.entries[index], query)) {
      screen.visible.push_back(index);
    }
  }
  if (screen.visible.empty()) {
    screen.selected = 0;
    return;
  }
  screen.selected = std::min(screen.selected, screen.visible.size() - 1);
}

const Entry *selected_entry(const ListScreen &screen) {
  if (screen.visible.empty()) {
    return nullptr;
  }
  return &screen.entries[screen.visible[screen.selected]];
}

void refresh_content_matches(ContentScreen &screen, std::string_view query) {
  screen.search.matches.clear();
  if (query.empty()) {
    screen.search.match_index = 0;
    return;
  }
  for (std::size_t index = 0; index < screen.lines.size(); ++index) {
    if (memix::tui::contains_case_insensitive(screen.lines[index], query)) {
      screen.search.matches.push_back(index);
    }
  }
  if (screen.search.matches.empty()) {
    screen.search.match_index = 0;
  } else {
    screen.search.match_index =
        std::min(screen.search.match_index, screen.search.matches.size() - 1);
  }
}

void focus_content_line(ContentScreen &screen, std::size_t line) {
  screen.scroll.follow_focus = false;
  const int viewport = std::max(screen.scroll.viewport_rows, 1);
  screen.scroll.top_row =
      std::max(0, static_cast<int>(line) - ((viewport - 1) / 2));
}

std::string type_scope(memix::MemoryObjectType type,
                       const std::optional<std::string> &generation) {
  std::string type_name(memix::to_string(type));
  if (!generation.has_value()) {
    return type_name;
  }
  return std::format("{}@{}", type_name, memix::tui::short_id(*generation));
}

bool load_store_screen(TuiState &state, ListScreen &screen) {
  struct Counts {
    std::size_t rules = 0;
    std::size_t chat = 0;
    std::size_t documents = 0;
    std::size_t environments = 0;
    std::size_t quests = 0;
    std::size_t objects = 0;
  } counts;

  const auto count_type = [&](memix::MemoryObjectType type,
                              std::size_t &count) {
    const memix::StoreResult<std::vector<memix::StoreFile>> files =
        state.store.list_files(type);
    if (!files) {
      if (files.error().kind == memix::ErrorKind::NotFound) {
        return true;
      }
      record_error(state, files.error().message);
      return false;
    }
    count = files->size();
    return true;
  };
  if (!count_type(memix::MemoryObjectType::Rules, counts.rules)
      || !count_type(memix::MemoryObjectType::Chat, counts.chat)
      || !count_type(memix::MemoryObjectType::Documents, counts.documents)) {
    return false;
  }

  const memix::ViewResult<std::vector<memix::ViewEnvironmentInfo>>
      environments = state.views.list_environments();
  if (environments) {
    counts.environments = environments->size();
  } else {
    record_error(state, environments.error().message);
    return false;
  }
  const memix::ViewResult<std::vector<memix::QuestInfo>> quests =
      state.views.list_quests();
  if (quests) {
    counts.quests = quests->size();
  } else {
    record_error(state, quests.error().message);
    return false;
  }
  const memix::StoreResult<std::vector<memix::StoredObject>> objects =
      state.store.list();
  if (objects) {
    counts.objects = objects->size();
  } else {
    record_error(state, objects.error().message);
    return false;
  }

  screen.entries = {
      Entry(EntryAction::OpenType, "Rules",
            pluralize(counts.rules, "file", "files"),
            memix::MemoryObjectType::Rules),
      Entry(EntryAction::OpenType, "Chat",
            pluralize(counts.chat, "file", "files"),
            memix::MemoryObjectType::Chat),
      Entry(EntryAction::OpenType, "Documents",
            pluralize(counts.documents, "file", "files"),
            memix::MemoryObjectType::Documents),
      Entry(EntryAction::OpenEnvironmentList, "Environments",
            pluralize(counts.environments, "environment", "environments")),
      Entry(EntryAction::OpenQuestList, "Quests",
            pluralize(counts.quests, "quest", "quests")),
      Entry(EntryAction::OpenObjects, "Objects",
            pluralize(counts.objects, "blob", "blobs")),
  };
  screen.kind = ListKind::Store;
  screen.scope = "store";
  screen.count = "6 sections";
  apply_filter(screen);
  return true;
}

bool load_type_tree(TuiState &state, ListScreen &screen) {
  const std::string ref_name =
      std::format("{}/latest", memix::to_string(screen.type));
  const memix::StoreResult<std::optional<std::string>> latest =
      state.store.read_ref(ref_name);
  if (!latest) {
    record_error(state, latest.error().message);
    return false;
  }
  screen.pinned_to_non_latest =
      screen.generation.has_value()
      && (!latest->has_value() || **latest != *screen.generation);

  std::optional<memix::ContextGeneration> generation;
  if (screen.generation.has_value()) {
    const memix::StoreResult<memix::ContextGeneration> read =
        state.store.read_generation(screen.type, *screen.generation);
    if (!read) {
      record_error(state, read.error().message);
      return false;
    }
    generation = *read;
  } else if (latest->has_value()) {
    const memix::StoreResult<memix::ContextGeneration> read =
        state.store.read_generation(screen.type, **latest);
    if (!read) {
      record_error(state, read.error().message);
      return false;
    }
    generation = *read;
  }

  std::vector<Entry> entries;
  std::size_t file_count = 0;
  if (generation.has_value()) {
    const memix::StoreResult<std::vector<memix::StoreCatalogEntry>> catalog =
        state.store.catalog(memix::StoreCatalogOptions{
            .type = screen.type,
            .path = screen.path,
            .depth = 1,
            .generation = generation->id,
        });
    if (!catalog) {
      record_error(state, catalog.error().message);
      return false;
    }
    const memix::tui::CatalogDirectoryResult directory =
        memix::tui::catalog_directory_file_count(*catalog, screen.path);
    if (!directory) {
      record_error(state, std::format("{} {}", memix::to_string(screen.type),
                                      directory.error()));
      return false;
    }
    file_count = *directory;
    // The generation hub row belongs to the type root only: the pin applies
    // to the whole tree, so nested directory screens do not repeat it. This
    // also means Generations is only reachable while the root TypeTree is on
    // top of the stack — no drilled descendants can exist at selection time.
    if (screen.path.empty()) {
      Entry hub(EntryAction::OpenGenerationList,
                memix::tui::generation_hub_text(
                    *generation, screen.generation.has_value()),
                "enter to browse generations", screen.type);
      hub.id = generation->id;
      entries.push_back(std::move(hub));
    }
    for (const memix::tui::ListRow &row :
         memix::tui::catalog_rows(*catalog, screen.path)) {
      Entry child(row.kind == memix::tui::RowKind::Directory
                      ? EntryAction::OpenTypeDirectory
                      : EntryAction::OpenTypeFile,
                  row.title, row.summary, screen.type);
      child.path = row.path;
      child.id = row.blob_id;
      entries.push_back(std::move(child));
    }
  }

  screen.entries = std::move(entries);
  screen.kind = ListKind::TypeTree;
  screen.scope = type_scope(screen.type, screen.generation);
  screen.count = pluralize(file_count, "file", "files");
  apply_filter(screen);
  return true;
}

bool load_generations(TuiState &state, ListScreen &screen) {
  const std::string ref_name =
      std::format("{}/latest", memix::to_string(screen.type));
  const memix::StoreResult<std::optional<std::string>> latest =
      state.store.read_ref(ref_name);
  if (!latest) {
    record_error(state, latest.error().message);
    return false;
  }

  std::vector<Entry> entries;
  std::set<std::string> visited;
  std::optional<std::string> current = *latest;
  bool first = true;
  while (current.has_value()) {
    if (!visited.insert(*current).second) {
      record_error(state, "generation history contains a cycle");
      return false;
    }
    const memix::StoreResult<memix::ContextGeneration> generation =
        state.store.read_generation(screen.type, *current);
    if (!generation) {
      record_error(state, generation.error().message);
      return false;
    }
    Entry row(
        EntryAction::SelectGeneration,
        std::format("generation {}{}", memix::tui::short_id(generation->id),
                    first ? " (latest)" : ""),
        std::format("{} · \"{}\"", generation->created_at, generation->message),
        screen.type);
    row.id = generation->id;
    entries.push_back(std::move(row));
    current = generation->parent;
    first = false;
  }
  screen.entries = std::move(entries);
  screen.kind = ListKind::Generations;
  screen.scope = std::format("{} generations", memix::to_string(screen.type));
  screen.count = pluralize(screen.entries.size(), "generation", "generations");
  apply_filter(screen);
  return true;
}

bool load_objects(TuiState &state, ListScreen &screen) {
  const memix::StoreResult<std::vector<memix::StoredObject>> objects =
      state.store.list();
  if (!objects) {
    record_error(state, objects.error().message);
    return false;
  }
  std::vector<memix::StoredObject> sorted = *objects;
  std::ranges::stable_sort(sorted, [](const memix::StoredObject &left,
                                      const memix::StoredObject &right) {
    return std::tuple{memix::to_string(left.type),
                      memix::tui::lowercase_copy(left.filename), left.id}
         < std::tuple{memix::to_string(right.type),
                      memix::tui::lowercase_copy(right.filename), right.id};
  });
  std::vector<Entry> entries;
  entries.reserve(sorted.size());
  for (const memix::StoredObject &object : sorted) {
    Entry row(EntryAction::OpenObject,
              object.filename.empty() ? memix::tui::short_id(object.id)
                                      : object.filename,
              std::format("{} · blob {} · {} · {}",
                          memix::to_string(object.type),
                          memix::tui::short_id(object.id),
                          memix::tui::format_size(object.size_bytes),
                          object.created_at),
              object.type);
    row.id = object.id;
    entries.push_back(std::move(row));
  }
  screen.entries = std::move(entries);
  screen.kind = ListKind::Objects;
  screen.scope = "objects";
  screen.count = pluralize(screen.entries.size(), "object", "objects");
  apply_filter(screen);
  return true;
}

bool load_environment_list(TuiState &state, ListScreen &screen) {
  const memix::ViewResult<std::vector<memix::ViewEnvironmentInfo>> result =
      state.views.list_environments();
  if (!result) {
    record_error(state, result.error().message);
    return false;
  }
  std::vector<Entry> entries;
  entries.reserve(result->size());
  for (const memix::ViewEnvironmentInfo &environment : *result) {
    Entry row(
        EntryAction::OpenEnvironment,
        std::format("environment {}", memix::tui::short_id(environment.id)),
        std::format("{} · {} · {} upstream",
                    pluralize(environment.file_count, "file", "files"),
                    memix::tui::format_size(environment.total_size_bytes),
                    environment.upstream_environment_count));
    row.id = environment.id;
    entries.push_back(std::move(row));
  }
  screen.entries = std::move(entries);
  screen.kind = ListKind::EnvironmentList;
  screen.scope = "environments";
  screen.count =
      pluralize(screen.entries.size(), "environment", "environments");
  apply_filter(screen);
  return true;
}

bool load_environment_tree(TuiState &state, ListScreen &screen) {
  const memix::ViewResult<memix::ViewEnvironment> environment =
      state.views.resolve_environment(screen.environment_id);
  if (!environment) {
    record_error(state, environment.error().message);
    return false;
  }
  const memix::tui::EnvironmentTreeSnapshot snapshot =
      memix::tui::environment_tree_snapshot(
          environment->derivation.resolved_files, screen.path);
  if (!snapshot.path_exists) {
    record_error(state, std::format("view path does not exist `{}`",
                                    screen.path.generic_string()));
    return false;
  }
  std::vector<Entry> entries;
  entries.reserve(snapshot.rows.size());
  for (const memix::tui::ListRow &row : snapshot.rows) {
    Entry child(row.kind == memix::tui::RowKind::Directory
                    ? EntryAction::OpenEnvironmentDirectory
                    : EntryAction::OpenEnvironmentFile,
                row.title, row.summary);
    child.path = row.path;
    child.id = row.blob_id;
    entries.push_back(std::move(child));
  }
  screen.entries = std::move(entries);
  screen.environment_id = environment->id;
  screen.kind = ListKind::EnvironmentTree;
  screen.scope = std::format("env {} locked",
                             memix::tui::short_id(screen.environment_id));
  screen.count = pluralize(snapshot.file_count, "file", "files");
  apply_filter(screen);
  return true;
}

bool load_quest_list(TuiState &state, ListScreen &screen) {
  const memix::ViewResult<std::vector<memix::QuestInfo>> quests =
      state.views.list_quests();
  if (!quests) {
    record_error(state, quests.error().message);
    return false;
  }
  std::vector<Entry> entries;
  entries.reserve(quests->size());
  for (const memix::QuestInfo &quest : *quests) {
    const std::string title =
        quest.name.empty()
            ? std::format("quest {}", memix::tui::short_id(quest.ir_hash))
            : memix::tui::normalize_line_breaks(quest.name);
    Entry row(EntryAction::OpenQuest, title,
              memix::tui::normalize_line_breaks(std::format(
                  "{} · {} · {} resolutions · {} reads", quest.intent,
                  memix::tui::short_id(quest.ir_hash),
                  quest.resolution_count, quest.read_count)));
    row.id = quest.ir_hash;
    row.quest = quest;
    entries.push_back(std::move(row));
  }
  screen.entries = std::move(entries);
  screen.kind = ListKind::QuestList;
  screen.scope = "quests";
  screen.count = pluralize(screen.entries.size(), "quest", "quests");
  apply_filter(screen);
  return true;
}

void append_provenance(std::vector<std::string> &lines,
                       const memix::tui::Provenance &provenance) {
  lines.emplace_back();
  lines.emplace_back("Provenance");
  std::vector<std::string> metadata =
      memix::tui::provenance_lines(provenance);
  std::ranges::move(metadata, std::back_inserter(lines));
}

memix::FsResult<std::string>
read_content_prefix(const fs::path &path, std::uintmax_t total_size_bytes) {
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    return memix::fs::fs_error(
        std::format("failed to open object content `{}`", path.string()));
  }
  std::string contents(
      memix::tui::bounded_content_read_size(total_size_bytes), '\0');
  if (!contents.empty()) {
    input.read(contents.data(), static_cast<std::streamsize>(contents.size()));
    contents.resize(static_cast<std::size_t>(input.gcount()));
  }
  if (input.bad()) {
    return memix::fs::fs_error(
        std::format("failed to read object content `{}`", path.string()));
  }
  return contents;
}

bool finish_object_content(TuiState &state, ContentScreen &screen,
                           const memix::StoredObject &object,
                           memix::tui::Provenance provenance) {
  const memix::FsResult<std::string> contents = read_content_prefix(
      object.object_dir / object.content_path, object.size_bytes);
  if (!contents) {
    record_error(state, contents.error().message);
    return false;
  }
  memix::tui::DecodedContent decoded =
      memix::tui::decode_content(*contents, object.size_bytes);
  std::vector<std::string> lines = std::move(decoded.lines);
  provenance.blob_id = object.id;
  provenance.sha256 = object.hash;
  provenance.size_bytes = object.size_bytes;
  provenance.created_at = object.created_at;
  append_provenance(lines, provenance);
  screen.blob_id = object.id;
  screen.lines = std::move(lines);
  screen.count = pluralize(screen.lines.size(), "line", "lines");
  refresh_content_matches(screen, screen.search.query);
  return true;
}

bool load_store_file_content(TuiState &state, ContentScreen &screen) {
  const memix::StoreResult<std::optional<std::string>> latest =
      state.store.read_ref(
          std::format("{}/latest", memix::to_string(screen.type)));
  if (!latest) {
    record_error(state, latest.error().message);
    return false;
  }
  screen.pinned_to_non_latest =
      screen.generation.has_value()
      && (!latest->has_value() || **latest != *screen.generation);

  std::optional<std::string> generation_id = screen.generation;
  if (!generation_id.has_value()) {
    if (!latest->has_value()) {
      record_error(state, std::format("{} has no latest generation",
                                      memix::to_string(screen.type)));
      return false;
    }
    generation_id = **latest;
  }
  const std::optional<std::string_view> resolved_generation =
      std::string_view(*generation_id);
  const memix::StoreResult<memix::StoredObject> object =
      state.store.read_file(screen.type, screen.path, resolved_generation);
  if (!object) {
    record_error(state, object.error().message);
    return false;
  }

  std::optional<std::string> parent;
  const memix::StoreResult<memix::ContextGeneration> metadata =
      state.store.read_generation(screen.type, *generation_id);
  if (!metadata) {
    record_error(state, metadata.error().message);
    return false;
  }
  parent = metadata->parent;
  screen.kind = ContentKind::StoreFile;
  screen.title = screen.path.generic_string();
  screen.scope = type_scope(screen.type, screen.generation);
  memix::tui::Provenance provenance;
  provenance.generation = std::move(generation_id);
  provenance.parent_generation = std::move(parent);
  return finish_object_content(state, screen, *object, std::move(provenance));
}

bool load_object_content(TuiState &state, ContentScreen &screen) {
  const memix::StoreResult<memix::StoredObject> object =
      state.store.read_blob(screen.type, screen.object_id);
  if (!object) {
    record_error(state, object.error().message);
    return false;
  }
  screen.kind = ContentKind::Object;
  screen.title = object->filename.empty() ? object->id : object->filename;
  screen.scope = "objects";
  return finish_object_content(state, screen, *object,
                               memix::tui::Provenance{});
}

bool
load_environment_file_content(TuiState &state, ContentScreen &screen) {
  const memix::ViewResult<memix::ViewEnvironment> environment =
      state.views.resolve_environment(screen.environment_id);
  if (!environment) {
    record_error(state, environment.error().message);
    return false;
  }
  const auto file =
      std::ranges::find(environment->derivation.resolved_files, screen.path,
                        &memix::ResolvedViewFile::output_path);
  if (file == environment->derivation.resolved_files.end()) {
    record_error(state, std::format("view file does not exist `{}`",
                                    screen.path.generic_string()));
    return false;
  }
  std::optional<std::string> parent;
  if (!file->source_generation.empty()) {
    const memix::StoreResult<memix::ContextGeneration> generation =
        state.store.read_generation(file->type, file->source_generation);
    if (generation) {
      parent = generation->parent;
    } else {
      record_error(state, generation.error().message);
    }
  }
  screen.kind = ContentKind::EnvironmentFile;
  screen.title = screen.path.generic_string();
  screen.environment_id = environment->id;
  screen.scope = std::format("env {} locked",
                             memix::tui::short_id(screen.environment_id));
  memix::tui::Provenance provenance;
  provenance.generation =
      file->source_generation.empty()
          ? std::nullopt
          : std::optional<std::string>(file->source_generation);
  provenance.parent_generation = std::move(parent);
  provenance.locator = file->locator.empty()
                         ? std::nullopt
                         : std::optional<std::string>(file->locator);
  provenance.source_generation =
      file->source_generation.empty()
          ? std::nullopt
          : std::optional<std::string>(file->source_generation);
  return finish_object_content(state, screen, file->blob,
                               std::move(provenance));
}

// The quest screen intentionally assembles all freshness and usage sections in
// one place so their displayed ordering remains obvious.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
bool load_quest_content(TuiState &state, ContentScreen &screen) {
  if (!screen.quest.has_value()) {
    record_error(state, "quest detail has no quest metadata");
    return false;
  }
  const memix::ViewResult<std::vector<memix::QuestInfo>> quests =
      state.views.list_quests();
  if (!quests) {
    record_error(state, quests.error().message);
    return false;
  }
  const auto found = std::ranges::find(*quests, screen.quest->ir_hash,
                                       &memix::QuestInfo::ir_hash);
  if (found == quests->end()) {
    record_error(state, "quest no longer exists");
    return false;
  }
  const memix::QuestInfo &quest = *found;
  const memix::ViewResult<memix::QuestStats> stats =
      state.views.quest_stats(quest.ir_hash);
  if (!stats) {
    record_error(state, stats.error().message);
    return false;
  }
  std::optional<memix::QuestFreshness> freshness;
  if (quest.resolution_count > 0) {
    const memix::ViewResult<memix::QuestFreshness> result =
        state.views.quest_freshness(quest.ir_hash);
    if (!result) {
      record_error(state, result.error().message);
      return false;
    }
    freshness = *result;
  }

  std::vector<std::string> lines{"Definition"};
  std::vector<std::string> intent_lines = memix::tui::split_labeled_lines(
      {.label = "intent", .value = quest.intent});
  std::ranges::move(intent_lines, std::back_inserter(lines));
  std::vector<std::string> name_lines = memix::tui::split_labeled_lines(
      {.label = "name", .value = quest.name});
  std::ranges::move(name_lines, std::back_inserter(lines));
  lines.insert(
      lines.end(),
      {
          std::format("ir_hash: {}", quest.ir_hash),
          std::format("created_at: {}", quest.created_at),
          std::format("resolutions: {}", quest.resolution_count),
          std::format("last_resolved_at: {}", quest.last_resolved_at),
          std::format("latest_derivation: {}", quest.latest_derivation_id),
          std::format("latest_environment: {}", quest.latest_environment_id),
          "",
          "Freshness",
      });
  if (!freshness.has_value()) {
    lines.emplace_back("overall: UNAVAILABLE (quest has no resolution)");
  } else {
    lines.push_back(
        std::format("overall: {}", freshness->stale ? "STALE" : "FRESH"));
    lines.push_back(std::format("resolved_at: {}", freshness->resolved_at));
    for (const memix::QuestFreshnessInput &input : freshness->inputs) {
      std::vector<std::string> input_lines =
          memix::tui::quest_freshness_input_lines(input);
      std::ranges::move(input_lines, std::back_inserter(lines));
    }
  }
  lines.emplace_back();
  lines.emplace_back("Stats");
  lines.push_back(std::format("resolutions: {} · last {}",
                              stats->resolution_count,
                              stats->last_resolved_at));
  lines.push_back(std::format(
      "activations: {} · first {} · last {}", stats->activation_count,
      stats->first_activated_at, stats->last_activated_at));
  for (const memix::QuestProfileActivations &activation :
       stats->activations_by_profile) {
    lines.push_back(std::format("  profile {}: {} activations",
                                activation.profile,
                                activation.activation_count));
  }
  lines.push_back(
      std::format("reads: {} · cat {} · tree {} · first {} · last {}",
                  stats->read_count, stats->cat_count, stats->tree_count,
                  stats->first_read_at, stats->last_read_at));
  for (const memix::QuestEnvironmentReads &reads :
       stats->reads_by_environment) {
    lines.push_back(std::format("  environment {}: {} reads",
                                memix::tui::short_id(reads.environment_id),
                                reads.read_count));
  }

  std::optional<std::size_t> environment_line;
  if (!quest.latest_environment_id.empty()) {
    lines.emplace_back();
    environment_line = lines.size();
    lines.push_back(
        std::format("Open latest environment {}",
                    memix::tui::short_id(quest.latest_environment_id)));
  }
  screen.quest = quest;
  screen.kind = ContentKind::Quest;
  screen.title = quest.name.empty() ? quest.ir_hash : quest.name;
  screen.scope = "quests";
  screen.lines = std::move(lines);
  screen.environment_line = environment_line;
  screen.count = pluralize(screen.lines.size(), "line", "lines");
  refresh_content_matches(screen, screen.search.query);
  return true;
}

bool refresh_screen(TuiState &state, TuiScreen &screen) {
  if (auto *list = std::get_if<ListScreen>(&screen)) {
    switch (list->kind) {
    case ListKind::Store:
      return load_store_screen(state, *list);
    case ListKind::TypeTree:
      return load_type_tree(state, *list);
    case ListKind::Generations:
      return load_generations(state, *list);
    case ListKind::Objects:
      return load_objects(state, *list);
    case ListKind::EnvironmentList:
      return load_environment_list(state, *list);
    case ListKind::EnvironmentTree:
      return load_environment_tree(state, *list);
    case ListKind::QuestList:
      return load_quest_list(state, *list);
    }
  }
  auto &content = std::get<ContentScreen>(screen);
  switch (content.kind) {
  case ContentKind::StoreFile:
    return load_store_file_content(state, content);
  case ContentKind::Object:
    return load_object_content(state, content);
  case ContentKind::EnvironmentFile:
    return load_environment_file_content(state, content);
  case ContentKind::Quest:
    return load_quest_content(state, content);
  }
  return false;
}

bool push_type_tree(TuiState &state, memix::MemoryObjectType type,
                    fs::path path,
                    std::optional<std::string> generation = std::nullopt) {
  ListScreen screen(ListKind::TypeTree);
  screen.type = type;
  screen.path = std::move(path);
  screen.generation = std::move(generation);
  state.stack.emplace_back(std::move(screen));
  if (!load_type_tree(state, std::get<ListScreen>(state.stack.back()))) {
    state.stack.pop_back();
    return false;
  }
  return true;
}

bool push_generations(TuiState &state, memix::MemoryObjectType type) {
  ListScreen screen(ListKind::Generations);
  screen.type = type;
  state.stack.emplace_back(std::move(screen));
  if (!load_generations(state, std::get<ListScreen>(state.stack.back()))) {
    state.stack.pop_back();
    return false;
  }
  return true;
}

bool push_objects(TuiState &state) {
  ListScreen screen(ListKind::Objects);
  state.stack.emplace_back(std::move(screen));
  if (!load_objects(state, std::get<ListScreen>(state.stack.back()))) {
    state.stack.pop_back();
    return false;
  }
  return true;
}

bool push_environment_list(TuiState &state) {
  ListScreen screen(ListKind::EnvironmentList);
  state.stack.emplace_back(std::move(screen));
  if (!load_environment_list(state, std::get<ListScreen>(state.stack.back()))) {
    state.stack.pop_back();
    return false;
  }
  return true;
}

bool push_environment_tree(TuiState &state, std::string environment_id,
                           fs::path path = {}) {
  ListScreen screen(ListKind::EnvironmentTree);
  screen.path = std::move(path);
  screen.environment_id = std::move(environment_id);
  state.stack.emplace_back(std::move(screen));
  if (!load_environment_tree(state, std::get<ListScreen>(state.stack.back()))) {
    state.stack.pop_back();
    return false;
  }
  return true;
}

bool push_quest_list(TuiState &state) {
  ListScreen screen(ListKind::QuestList);
  state.stack.emplace_back(std::move(screen));
  if (!load_quest_list(state, std::get<ListScreen>(state.stack.back()))) {
    state.stack.pop_back();
    return false;
  }
  return true;
}

bool push_store_file(TuiState &state, memix::MemoryObjectType type,
                     fs::path path,
                     std::optional<std::string> generation = std::nullopt) {
  ContentScreen screen(ContentKind::StoreFile);
  screen.type = type;
  screen.path = std::move(path);
  screen.generation = std::move(generation);
  state.stack.emplace_back(std::move(screen));
  if (!load_store_file_content(state,
                               std::get<ContentScreen>(state.stack.back()))) {
    state.stack.pop_back();
    return false;
  }
  return true;
}

bool push_object(TuiState &state, memix::MemoryObjectType type,
                 std::string id) {
  ContentScreen screen(ContentKind::Object);
  screen.type = type;
  screen.object_id = std::move(id);
  state.stack.emplace_back(std::move(screen));
  if (!load_object_content(state,
                           std::get<ContentScreen>(state.stack.back()))) {
    state.stack.pop_back();
    return false;
  }
  return true;
}

bool push_environment_file(TuiState &state, std::string environment_id,
                           fs::path path) {
  ContentScreen screen(ContentKind::EnvironmentFile);
  screen.path = std::move(path);
  screen.environment_id = std::move(environment_id);
  state.stack.emplace_back(std::move(screen));
  if (!load_environment_file_content(
          state, std::get<ContentScreen>(state.stack.back()))) {
    state.stack.pop_back();
    return false;
  }
  return true;
}

bool push_quest(TuiState &state, memix::QuestInfo quest) {
  ContentScreen screen(ContentKind::Quest);
  screen.quest = std::move(quest);
  state.stack.emplace_back(std::move(screen));
  if (!load_quest_content(state, std::get<ContentScreen>(state.stack.back()))) {
    state.stack.pop_back();
    return false;
  }
  return true;
}

template <typename Matcher>
bool select_visible_entry(ListScreen &screen, const Matcher &matches) {
  for (std::size_t visible = 0; visible < screen.visible.size(); ++visible) {
    if (matches(screen.entries[screen.visible[visible]])) {
      screen.selected = visible;
      follow_selection(screen.scroll);
      return true;
    }
  }
  return false;
}

bool select_visible_id(ListScreen &screen, std::string_view id) {
  return select_visible_entry(
      screen, [id](const Entry &entry) { return entry.id == id; });
}

bool select_generation(TuiState &state, const Entry &entry) {
  if (state.stack.size() <= 1) {
    return false;
  }
  // The hub row exists only on the type-root TypeTree, so the Generations
  // screen is always pushed directly above the root TypeTree: popping it
  // leaves that root on top, with no drilled descendants to repair.
  std::vector<TuiScreen> refreshed_stack = state.stack;
  refreshed_stack.pop_back();
  auto *list = std::get_if<ListScreen>(&refreshed_stack.back());
  if (list == nullptr
      || list->kind != ListKind::TypeTree
      || list->type != entry.type
      || !list->path.empty()) {
    record_error(state, "generation selection has no context tree");
    return false;
  }
  ListScreen refreshed = *list;
  refreshed.generation = entry.id;
  if (!load_type_tree(state, refreshed)) {
    return false;
  }
  select_visible_entry(refreshed, [](const Entry &candidate) {
    return candidate.action == EntryAction::OpenGenerationList;
  });
  *list = std::move(refreshed);
  state.stack = std::move(refreshed_stack);
  state.status = "pinned generation " + memix::tui::short_id(entry.id);
  return true;
}

bool open_selected(TuiState &state) {
  const auto *screen = std::get_if<ListScreen>(&state.stack.back());
  if (screen == nullptr) {
    return false;
  }
  const Entry *selected = selected_entry(*screen);
  if (selected == nullptr) {
    state.status = "nothing selected";
    return true;
  }
  const Entry entry = *selected;
  state.status.clear();
  switch (entry.action) {
  case EntryAction::OpenType:
    return push_type_tree(state, entry.type, {});
  case EntryAction::OpenObjects:
    return push_objects(state);
  case EntryAction::OpenEnvironmentList:
    return push_environment_list(state);
  case EntryAction::OpenQuestList:
    return push_quest_list(state);
  case EntryAction::OpenGenerationList:
    return push_generations(state, entry.type);
  case EntryAction::OpenTypeDirectory:
    return push_type_tree(state, entry.type, entry.path, screen->generation);
  case EntryAction::OpenTypeFile:
    return push_store_file(state, entry.type, entry.path, screen->generation);
  case EntryAction::SelectGeneration:
    return select_generation(state, entry);
  case EntryAction::OpenObject:
    return push_object(state, entry.type, entry.id);
  case EntryAction::OpenEnvironment:
    return push_environment_tree(state, entry.id);
  case EntryAction::OpenEnvironmentDirectory:
    return push_environment_tree(state, screen->environment_id, entry.path);
  case EntryAction::OpenEnvironmentFile:
    return push_environment_file(state, screen->environment_id, entry.path);
  case EntryAction::OpenQuest:
    if (entry.quest.has_value()) {
      return push_quest(state, *entry.quest);
    }
    record_error(state, "selected quest has no metadata");
    return false;
  }
  return false;
}

std::expected<void, std::string>
open_quest_environment(TuiState &state, ContentScreen &screen) {
  if (!screen.quest.has_value()
      || screen.quest->latest_environment_id.empty()) {
    return std::unexpected("quest has no resolved environment");
  }
  const memix::ViewResult<memix::ViewEnvironment> environment =
      state.views.resolve_environment(screen.quest->latest_environment_id);
  if (!environment) {
    record_error(state, environment.error().message);
    return std::unexpected(environment.error().message);
  }
  if (!push_environment_tree(state, environment->id)) {
    return std::unexpected(last_error(state));
  }
  return {};
}

std::vector<fs::path> parent_scopes(const fs::path &path, bool include_path) {
  std::vector<fs::path> components =
      path | std::ranges::to<std::vector<fs::path>>();
  if (!include_path && !components.empty()) {
    components.pop_back();
  }
  std::vector<fs::path> scopes;
  fs::path current;
  for (const fs::path &component : components) {
    current /= component;
    scopes.push_back(current);
  }
  return scopes;
}

std::expected<std::optional<std::string>, std::string>
resolve_locator_generation(const TuiState &state,
                           const memix::tui::Locator &locator) {
  if (locator.version.kind == memix::tui::ContextVersionKind::Latest) {
    return std::optional<std::string>{};
  }
  if (locator.version.kind == memix::tui::ContextVersionKind::Generation
      && locator.version.direct_generation) {
    return std::optional<std::string>{locator.version.value};
  }
  const memix::StoreResult<std::optional<std::string>> ref =
      state.store.read_ref(memix::tui::context_ref_name(locator));
  if (!ref) {
    return std::unexpected(ref.error().message);
  }
  if (!ref->has_value()) {
    return std::unexpected(std::format("ref does not exist `{}`",
                                       memix::tui::context_ref_name(locator)));
  }
  return *ref;
}

std::expected<void, std::string>
open_context_locator(TuiState &state,
                     const memix::tui::Locator &locator) {
  if (!locator.type.has_value()) {
    return std::unexpected("context locator has no object type");
  }
  const std::expected<std::optional<std::string>, std::string> generation =
      resolve_locator_generation(state, locator);
  if (!generation) {
    return std::unexpected(generation.error());
  }
  select_visible_entry(std::get<ListScreen>(state.stack.front()),
                       [&locator](const Entry &entry) {
                         return entry.action == EntryAction::OpenType
                             && entry.type == *locator.type;
                       });
  if (!push_type_tree(state, *locator.type, {}, *generation)) {
    return std::unexpected(last_error(state));
  }
  const bool directory =
      locator.kind == memix::tui::LocatorKind::ContextDirectory;
  for (const fs::path &scope : parent_scopes(locator.path, directory)) {
    select_visible_entry(std::get<ListScreen>(state.stack.back()),
                         [&scope](const Entry &entry) {
                           return entry.action == EntryAction::OpenTypeDirectory
                               && entry.path == scope;
                         });
    if (!push_type_tree(state, *locator.type, scope, *generation)) {
      return std::unexpected(last_error(state));
    }
  }
  if (!directory) {
    select_visible_entry(std::get<ListScreen>(state.stack.back()),
                         [&locator](const Entry &entry) {
                           return entry.action == EntryAction::OpenTypeFile
                               && entry.path == locator.path;
                         });
    if (!push_store_file(state, *locator.type, locator.path, *generation)) {
      return std::unexpected(last_error(state));
    }
  }
  return {};
}

std::expected<void, std::string>
open_object_locator(TuiState &state, std::string_view prefix) {
  select_visible_entry(std::get<ListScreen>(state.stack.front()),
                       [](const Entry &entry) {
                         return entry.action == EntryAction::OpenObjects;
                       });
  if (!push_objects(state)) {
    return std::unexpected(last_error(state));
  }
  auto &list = std::get<ListScreen>(state.stack.back());
  std::vector<const Entry *> matches;
  for (const Entry &entry : list.entries) {
    if (entry.id.starts_with(prefix)) {
      matches.push_back(&entry);
    }
  }
  if (matches.empty()) {
    return std::unexpected(std::format("object prefix not found `{}`", prefix));
  }
  if (matches.size() > 1) {
    return std::unexpected(
        std::format("object prefix is ambiguous `{}`", prefix));
  }
  const Entry match = *matches.front();
  select_visible_id(list, match.id);
  if (!push_object(state, match.type, match.id)) {
    return std::unexpected(last_error(state));
  }
  return {};
}

std::expected<void, std::string>
open_environment_locator(TuiState &state, std::string_view prefix) {
  const memix::ViewResult<memix::ViewEnvironment> environment =
      state.views.resolve_environment(prefix);
  if (!environment) {
    return std::unexpected(environment.error().message);
  }
  select_visible_entry(
      std::get<ListScreen>(state.stack.front()), [](const Entry &entry) {
        return entry.action == EntryAction::OpenEnvironmentList;
      });
  if (!push_environment_list(state)) {
    return std::unexpected(last_error(state));
  }
  select_visible_id(std::get<ListScreen>(state.stack.back()), environment->id);
  if (!push_environment_tree(state, environment->id)) {
    return std::unexpected(last_error(state));
  }
  return {};
}

std::expected<void, std::string>
open_quest_locator(TuiState &state, std::string_view name) {
  select_visible_entry(std::get<ListScreen>(state.stack.front()),
                       [](const Entry &entry) {
                         return entry.action == EntryAction::OpenQuestList;
                       });
  if (!push_quest_list(state)) {
    return std::unexpected(last_error(state));
  }
  auto &list = std::get<ListScreen>(state.stack.back());
  std::vector<const Entry *> matches;
  for (const Entry &entry : list.entries) {
    if ((entry.quest.has_value() && entry.quest->name == name)
        || entry.id.starts_with(name)) {
      matches.push_back(&entry);
    }
  }
  if (matches.empty()) {
    return std::unexpected(std::format("quest not found `{}`", name));
  }
  if (matches.size() > 1) {
    return std::unexpected(std::format("quest is ambiguous `{}`", name));
  }
  const Entry match = *matches.front();
  select_visible_id(list, match.id);
  if (!match.quest.has_value() || !push_quest(state, *match.quest)) {
    return std::unexpected(last_error(state));
  }
  return {};
}

std::expected<void, std::string>
open_locator(TuiState &state, const memix::tui::Locator &locator) {
  switch (locator.kind) {
  case memix::tui::LocatorKind::ContextFile:
  case memix::tui::LocatorKind::ContextDirectory:
    return open_context_locator(state, locator);
  case memix::tui::LocatorKind::Object:
    return open_object_locator(state, locator.value);
  case memix::tui::LocatorKind::Environment:
    return open_environment_locator(state, locator.value);
  case memix::tui::LocatorKind::Quest:
    return open_quest_locator(state, locator.value);
  }
  return std::unexpected("unsupported locator");
}

struct EditorTempFile {
  fs::path directory;
  fs::path file;
};

void remove_temp_directory(const fs::path &path) {
  if (!path.empty()) {
    std::ignore = fs::remove_all(path);
  }
}

void discard_pending_mutation(TuiState &state) {
  if (state.pending_mutation.has_value()) {
    remove_temp_directory(state.pending_mutation->temp_directory);
    state.pending_mutation.reset();
  }
}

#ifdef _WIN32
constexpr char kCmdLiteralPercentVariable[] =
    "MEMIX_TUI_INTERNAL_LITERAL_PERCENT";
#endif

std::string shell_quote(std::string_view value) {
#ifdef _WIN32
  std::string quoted{"\""};
  for (const char character : value) {
    if (character == '"') {
      quoted += "\\\"";
    } else if (character == '%') {
      // cmd.exe expands variables inside double quotes. Expand this internal
      // variable once to recover a literal percent; cmd substitution is not
      // recursive, so a resulting %NAME% path segment remains unchanged.
      quoted.push_back('%');
      quoted += kCmdLiteralPercentVariable;
      quoted.push_back('%');
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

std::string editor_command() {
  // The TUI is single-threaded, and editor variables intentionally name the
  // user-selected process launched by this explicit editing action.
  if (const char *visual =
          std::getenv("VISUAL"); // NOLINT(concurrency-mt-unsafe)
      visual != nullptr && *visual != '\0') {
    return visual;
  }
  if (const char *editor =
          std::getenv("EDITOR"); // NOLINT(concurrency-mt-unsafe)
      editor != nullptr && *editor != '\0') {
    return editor;
  }
  return "vi";
}

std::expected<EditorTempFile, std::string>
create_editor_temp_file(const fs::path &logical_path,
                        std::string_view contents) {
  const memix::FsResult<fs::path> system_temp = fs::temp_directory_path();
  if (!system_temp) {
    return std::unexpected(system_temp.error().message);
  }
  const memix::FsResult<fs::path> unique = fs::unique_temp_dir(
      *system_temp,
      {
          .prefix = "memix-tui",
          .failure_message = "failed to allocate editor temp path",
      });
  if (!unique) {
    return std::unexpected(unique.error().message);
  }
  const memix::FsResult<bool> created = fs::create_directory(*unique);
  if (!created) {
    return std::unexpected(created.error().message);
  }
  if (!*created) {
    return std::unexpected(
        std::format("editor temp path already exists `{}`", unique->string()));
  }
  memix::scope_exit cleanup(
      [directory = *unique] { remove_temp_directory(directory); });

  const memix::FsVoid directory_permissions =
      fs::permissions(*unique, fs::perms::owner_all);
  if (!directory_permissions) {
    return std::unexpected(directory_permissions.error().message);
  }
  const fs::path filename = logical_path.filename().empty()
                              ? fs::path("content")
                              : logical_path.filename();
  const fs::path temp_file = *unique / filename;
  const memix::FsVoid written = fs::write_text_file(temp_file, contents);
  if (!written) {
    return std::unexpected(written.error().message);
  }
  const memix::FsVoid file_permissions = fs::permissions(
      temp_file, fs::perms::owner_read | fs::perms::owner_write);
  if (!file_permissions) {
    return std::unexpected(file_permissions.error().message);
  }

  cleanup.release();
  return EditorTempFile{.directory = *unique, .file = temp_file};
}

std::expected<std::string, std::string>
editor_initial_content(const TuiState &state, const EditorRequest &request) {
  if (request.operation == memix::tui::EditOperation::Add) {
    return std::string{};
  }
  const std::optional<std::string_view> generation =
      request.generation.transform(
          [](const std::string &value) { return std::string_view(value); });
  const memix::StoreResult<memix::StoredObject> object =
      state.store.read_file(request.type, request.path, generation);
  if (!object) {
    return std::unexpected(object.error().message);
  }
  const memix::FsResult<std::string> contents = fs::read_text_file(
      object->object_dir / object->content_path, "editor source");
  if (!contents) {
    return std::unexpected(contents.error().message);
  }
  return *contents;
}

void run_editor_request(TuiState &state) {
  if (!state.editor_request.has_value()) {
    return;
  }
  const EditorRequest request = std::move(*state.editor_request);
  state.editor_request.reset();
  const std::expected<std::string, std::string> original =
      editor_initial_content(state, request);
  if (!original) {
    record_error(state, original.error());
    return;
  }
  std::expected<EditorTempFile, std::string> temp =
      create_editor_temp_file(request.path, *original);
  if (!temp) {
    record_error(state, temp.error());
    return;
  }
  memix::scope_exit cleanup(
      [directory = temp->directory] { remove_temp_directory(directory); });

  const std::string temp_path = temp->file.string();
#ifdef _WIN32
  // The TUI is single-threaded, and this temporary process variable is scoped
  // to the synchronous editor handoff below.
  // NOLINTBEGIN(concurrency-mt-unsafe)
  const bool escaped_percent = temp_path.contains('%');
  const char *previous_percent =
      escaped_percent ? std::getenv(kCmdLiteralPercentVariable) : nullptr;
  const std::optional<std::string> previous_percent_value =
      previous_percent == nullptr
          ? std::nullopt
          : std::optional<std::string>(previous_percent);
  if (escaped_percent && _putenv_s(kCmdLiteralPercentVariable, "%") != 0) {
    record_error(state, "failed to escape editor temp path for cmd.exe");
    return;
  }
  memix::scope_exit restore_percent([escaped_percent, previous_percent_value] {
    if (escaped_percent) {
      const char *value = previous_percent_value.has_value()
                            ? previous_percent_value->c_str()
                            : "";
      static_cast<void>(_putenv_s(kCmdLiteralPercentVariable, value));
    }
  });
  // NOLINTEND(concurrency-mt-unsafe)
#endif
  const std::string command =
      std::format("{} {}", editor_command(), shell_quote(temp_path));
  // NOLINTNEXTLINE(bugprone-command-processor,cert-env33-c,concurrency-mt-unsafe)
  const int editor_status = std::system(command.c_str());
  if (editor_status != 0) {
    record_error(state,
                 std::format("editor exited with status {}", editor_status));
    return;
  }
  const memix::FsResult<std::string> edited =
      fs::read_text_file(temp->file, "edited content");
  if (!edited) {
    record_error(state, edited.error().message);
    return;
  }
  if (memix::tui::content_unchanged(*original, *edited)) {
    state.status = "unchanged";
    return;
  }

  discard_pending_mutation(state);
  state.pending_mutation = PendingMutation{
      .operation = request.operation,
      .type = request.type,
      .path = request.path,
      .generation = request.generation,
      .temp_directory = temp->directory,
      .temp_file = temp->file,
  };
  const std::string default_message = memix::tui::default_edit_message(
      request.operation, request.type, request.path);
  state.prompt = FooterPrompt{
      .kind = FooterPromptKind::GenerationMessage,
      .input = default_message,
      .fallback = default_message,
      .type = request.type,
      .directory = {},
      .generation = request.generation,
  };
  state.status.clear();
  cleanup.release();
}

bool refuse_non_latest_pin(TuiState &state, memix::MemoryObjectType type,
                           const std::optional<std::string> &generation) {
  if (!generation.has_value()) {
    return false;
  }
  const memix::StoreResult<std::optional<std::string>> latest =
      state.store.read_ref(std::format("{}/latest", memix::to_string(type)));
  if (!latest) {
    record_error(state, latest.error().message);
    return true;
  }
  if (!latest->has_value() || **latest != *generation) {
    state.status = "editing applies to latest; unpin first";
    return true;
  }
  return false;
}

bool
new_file_path_available(TuiState &state, memix::MemoryObjectType type,
                        const fs::path &path) {
  const memix::StoreResult<memix::StoredObject> existing =
      state.store.read_file(type, path);
  if (existing) {
    state.status =
        std::format("file already exists `{}`", path.generic_string());
    return false;
  }
  if (existing.error().kind == memix::ErrorKind::NotFound
      || existing.error().kind == memix::ErrorKind::ContextPathNotFound) {
    return true;
  }
  record_error(state, existing.error().message);
  return false;
}

bool select_path_entry(ListScreen &screen, const fs::path &path,
                       EntryAction action) {
  const auto matches = [&path, action](const Entry &entry) {
    return entry.action == action && entry.path == path;
  };
  if (select_visible_entry(screen, matches)) {
    return true;
  }
  if (screen.filter.active || !screen.filter.query.empty()) {
    screen.filter.active = false;
    screen.filter.input.clear();
    screen.filter.query.clear();
    apply_filter(screen);
    return select_visible_entry(screen, matches);
  }
  return false;
}

bool rebuild_type_stack_after_mutation(TuiState &state,
                                       memix::tui::EditOperation operation,
                                       memix::MemoryObjectType type,
                                       const fs::path &path) {
  const auto root = std::ranges::find_if(
      state.stack, [type](const TuiScreen &candidate) {
        const auto *list = std::get_if<ListScreen>(&candidate);
        return list != nullptr
            && list->kind == ListKind::TypeTree
            && list->type == type
            && list->path.empty();
      });
  if (root == state.stack.end()) {
    record_error(state, "edit has no type-root tree to refresh");
    return false;
  }
  const bool reopen_content =
      operation == memix::tui::EditOperation::Edit
      && std::holds_alternative<ContentScreen>(state.stack.back())
      && std::get<ContentScreen>(state.stack.back()).kind
             == ContentKind::StoreFile
      && std::get<ContentScreen>(state.stack.back()).type == type
      && std::get<ContentScreen>(state.stack.back()).path == path;
  const std::size_t root_index =
      static_cast<std::size_t>(root - state.stack.begin());
  state.stack.resize(root_index + 1);
  auto &root_screen = std::get<ListScreen>(state.stack.back());
  root_screen.generation.reset();
  root_screen.pinned_to_non_latest = false;
  if (!load_type_tree(state, root_screen)) {
    return false;
  }

  for (const fs::path &scope : parent_scopes(path, false)) {
    auto &parent = std::get<ListScreen>(state.stack.back());
    if (!select_path_entry(parent, scope, EntryAction::OpenTypeDirectory)) {
      if (operation == memix::tui::EditOperation::Remove) {
        return true;
      }
      record_error(state, std::format("edited directory disappeared `{}`",
                                      scope.generic_string()));
      return false;
    }
    if (!push_type_tree(state, type, scope)) {
      return false;
    }
  }

  auto &parent = std::get<ListScreen>(state.stack.back());
  const bool selected =
      select_path_entry(parent, path, EntryAction::OpenTypeFile);
  if (operation == memix::tui::EditOperation::Remove) {
    return true;
  }
  if (!selected) {
    record_error(state, std::format("edited file disappeared `{}`",
                                    path.generic_string()));
    return false;
  }
  if (reopen_content && !push_store_file(state, type, path)) {
    return false;
  }
  return true;
}

void record_projection_warning(TuiState &state,
                               const std::optional<std::string> &warning) {
  if (warning.has_value()) {
    record_error(state, *warning);
  }
}

void
commit_pending_mutation(TuiState &state, const std::string &message) {
  if (!state.pending_mutation.has_value()) {
    record_error(state, "edit message has no pending operation");
    return;
  }
  const PendingMutation mutation = std::move(*state.pending_mutation);
  state.pending_mutation.reset();
  const memix::scope_exit cleanup([directory = mutation.temp_directory] {
    remove_temp_directory(directory);
  });
  if (refuse_non_latest_pin(state, mutation.type, mutation.generation)) {
    return;
  }
  if (mutation.operation == memix::tui::EditOperation::Add
      && !new_file_path_available(state, mutation.type, mutation.path)) {
    return;
  }

  std::string generation_id;
  if (mutation.operation == memix::tui::EditOperation::Remove) {
    const memix::StoreResult<memix::FileRemoveResult> removed =
        state.store.remove_file({
            .type = mutation.type,
            .context_path = mutation.path,
            .message = message,
        });
    if (!removed) {
      record_error(state, removed.error().message);
      return;
    }
    generation_id = removed->generation.id;
    record_projection_warning(state, removed->projection_warning);
  } else {
    const memix::StoreResult<memix::FileIngestResult> ingested =
        state.store.ingest_file({
            .type = mutation.type,
            .source_path = mutation.temp_file,
            .context_path = mutation.path,
            .message = message,
        });
    if (!ingested) {
      record_error(state, ingested.error().message);
      return;
    }
    generation_id = ingested->generation.id;
    record_projection_warning(state, ingested->projection_warning);
  }

  static_cast<void>(rebuild_type_stack_after_mutation(
      state, mutation.operation, mutation.type, mutation.path));
  state.status =
      "generation " + memix::tui::short_id(generation_id) + " created";
}

void begin_generation_message_prompt(TuiState &state,
                                     PendingMutation mutation) {
  discard_pending_mutation(state);
  const std::string default_message = memix::tui::default_edit_message(
      mutation.operation, mutation.type, mutation.path);
  FooterPrompt prompt{
      .kind = FooterPromptKind::GenerationMessage,
      .input = default_message,
      .fallback = default_message,
      .type = mutation.type,
      .directory = {},
      .generation = mutation.generation,
  };
  state.pending_mutation = std::move(mutation);
  state.prompt = std::move(prompt);
  state.status.clear();
}

bool
handle_footer_prompt_event(TuiState &state, const ftxui::Event &event,
                           const ftxui::Closure &open_editor) {
  if (!state.prompt.has_value()) {
    return false;
  }
  if (event == ftxui::Event::Escape) {
    const FooterPromptKind kind = state.prompt->kind;
    state.prompt.reset();
    discard_pending_mutation(state);
    state.status = kind == FooterPromptKind::NewFileName ? "add canceled"
                                                         : "edit canceled";
    return true;
  }
  if (event == ftxui::Event::Backspace) {
    memix::tui::erase_last_utf8_code_point(state.prompt->input);
    state.status.clear();
    return true;
  }
  if (event == ftxui::Event::CtrlU) {
    state.prompt->input.clear();
    state.status.clear();
    return true;
  }
  if (event == ftxui::Event::Return) {
    const FooterPrompt prompt = *state.prompt;
    if (prompt.kind == FooterPromptKind::NewFileName) {
      const memix::tui::NewFileNameResult name =
          memix::tui::validate_new_file_name(prompt.input);
      if (!name) {
        state.status = "invalid name: " + name.error();
        return true;
      }
      if (refuse_non_latest_pin(state, prompt.type, prompt.generation)) {
        state.prompt.reset();
        return true;
      }
      const fs::path path = prompt.directory / *name;
      if (!new_file_path_available(state, prompt.type, path)) {
        return true;
      }
      state.editor_request = EditorRequest{
          .operation = memix::tui::EditOperation::Add,
          .type = prompt.type,
          .path = path,
          .generation = prompt.generation,
      };
      state.prompt.reset();
      state.status.clear();
      open_editor();
      return true;
    }

    const std::string message =
        prompt.input.empty() ? prompt.fallback : prompt.input;
    state.prompt.reset();
    commit_pending_mutation(state, message);
    return true;
  }
  if (event.is_character()) {
    state.prompt->input += event.character();
    state.status.clear();
  }
  return true;
}

bool handle_delete_confirmation_event(TuiState &state,
                                      const ftxui::Event &event) {
  if (!state.delete_confirmation.has_value()) {
    return false;
  }
  if (event == ftxui::Event::Escape || event == ftxui::Event::q) {
    state.delete_confirmation.reset();
    state.status = "delete canceled";
    return true;
  }
  if (event == ftxui::Event::Return) {
    const DeleteConfirmation confirmation = *state.delete_confirmation;
    state.delete_confirmation.reset();
    begin_generation_message_prompt(
        state, PendingMutation{
                   .operation = memix::tui::EditOperation::Remove,
                   .type = confirmation.type,
                   .path = confirmation.path,
                   .generation = confirmation.generation,
                   .temp_directory = {},
                   .temp_file = {},
               });
    return true;
  }
  return true;
}

bool is_mutation_event(const ftxui::Event &event) {
  return event == ftxui::Event::e
      || event == ftxui::Event::a
      || event == ftxui::Event::d;
}

bool handle_mutation_event(TuiState &state, const ftxui::Event &event,
                           const ftxui::Closure &open_editor) {
  if (const auto *list = std::get_if<ListScreen>(&state.stack.back())) {
    if (list->kind == ListKind::EnvironmentTree) {
      state.status = "environments are locked artifacts";
      return true;
    }
    if (list->kind != ListKind::TypeTree) {
      return false;
    }
    if (refuse_non_latest_pin(state, list->type, list->generation)) {
      return true;
    }
    if (event == ftxui::Event::a) {
      state.prompt = FooterPrompt{
          .kind = FooterPromptKind::NewFileName,
          .input = {},
          .fallback = {},
          .type = list->type,
          .directory = list->path,
          .generation = list->generation,
      };
      state.status.clear();
      return true;
    }
    const Entry *entry = selected_entry(*list);
    if (entry == nullptr || entry->action != EntryAction::OpenTypeFile) {
      state.status = "select a file first";
      return true;
    }
    if (event == ftxui::Event::e) {
      state.editor_request = EditorRequest{
          .operation = memix::tui::EditOperation::Edit,
          .type = list->type,
          .path = entry->path,
          .generation = list->generation,
      };
      state.status.clear();
      open_editor();
      return true;
    }
    state.delete_confirmation = DeleteConfirmation{
        .type = list->type,
        .path = entry->path,
        .blob_id = entry->id,
        .generation = list->generation,
    };
    return true;
  }

  const auto &content = std::get<ContentScreen>(state.stack.back());
  if (content.kind == ContentKind::EnvironmentFile) {
    state.status = "environments are locked artifacts";
    return true;
  }
  if (content.kind != ContentKind::StoreFile) {
    return false;
  }
  if (refuse_non_latest_pin(state, content.type, content.generation)) {
    return true;
  }
  if (event == ftxui::Event::a) {
    state.status = "add files from a directory column";
    return true;
  }
  if (event == ftxui::Event::e) {
    state.editor_request = EditorRequest{
        .operation = memix::tui::EditOperation::Edit,
        .type = content.type,
        .path = content.path,
        .generation = content.generation,
    };
    state.status.clear();
    open_editor();
    return true;
  }
  state.delete_confirmation = DeleteConfirmation{
      .type = content.type,
      .path = content.path,
      .blob_id = content.blob_id,
      .generation = content.generation,
  };
  return true;
}

int wheel_rows(ftxui::Event &event) {
  if (!event.is_mouse()) {
    return 0;
  }
  if (event.mouse().button == ftxui::Mouse::WheelUp) {
    return -1;
  }
  if (event.mouse().button == ftxui::Mouse::WheelDown) {
    return 1;
  }
  return 0;
}

void move_list_up(ListScreen &screen, std::size_t amount) {
  follow_selection(screen.scroll);
  screen.selected -= std::min(screen.selected, amount);
}

void move_list_down(ListScreen &screen, std::size_t amount) {
  follow_selection(screen.scroll);
  if (screen.visible.empty()) {
    screen.selected = 0;
    return;
  }
  const std::size_t last = screen.visible.size() - 1;
  screen.selected += std::min(last - screen.selected, amount);
}

void resolve_pending_list_recenter(ListScreen &screen) {
  if (!screen.scroll.follow_focus
      || screen.scroll.viewport_rows <= 0
      || screen.visible.empty()) {
    return;
  }
  screen.selected = std::min(screen.selected, screen.visible.size() - 1);
  const int first_row = static_cast<int>(screen.selected) * kRowsPerEntry;
  const int total_rows =
      static_cast<int>(screen.visible.size()) * kRowsPerEntry;
  screen.scroll.max_top_row =
      max_top_row_for(total_rows, screen.scroll.viewport_rows);
  screen.scroll.top_row = std::clamp(
      centered_top_row(first_row, first_row + 1, screen.scroll.viewport_rows),
      0, screen.scroll.max_top_row);
}

void sync_list_selection_to_viewport(ListScreen &screen) {
  if (screen.visible.empty() || screen.scroll.viewport_rows <= 0) {
    return;
  }
  const int top = std::max(screen.scroll.top_row, 0);
  const int index = (top + kRowsPerEntry - 1) / kRowsPerEntry;
  screen.selected =
      std::min(static_cast<std::size_t>(index), screen.visible.size() - 1);
}

void begin_filter(TuiState &state, ListScreen &screen) {
  screen.filter.active = true;
  screen.filter.input.clear();
  state.status.clear();
  apply_filter(screen);
}

void cancel_filter(TuiState &state, ListScreen &screen) {
  screen.filter.active = false;
  screen.filter.input.clear();
  state.status = "filter canceled";
  apply_filter(screen);
}

void commit_filter(TuiState &state, ListScreen &screen) {
  screen.filter.active = false;
  screen.filter.query = screen.filter.input;
  apply_filter(screen);
  if (screen.filter.query.empty()) {
    state.status = "empty filter";
  } else {
    state.status = screen.visible.empty() ? "no matches" : "";
  }
}

bool handle_filter_event(TuiState &state, ListScreen &screen,
                         const ftxui::Event &event) {
  if (event == ftxui::Event::Escape || event == ftxui::Event::q) {
    cancel_filter(state, screen);
    return true;
  }
  if (event == ftxui::Event::Return) {
    commit_filter(state, screen);
    return true;
  }
  if (event == ftxui::Event::Backspace) {
    memix::tui::erase_last_utf8_code_point(screen.filter.input);
    state.status.clear();
    apply_filter(screen);
    return true;
  }
  if (event == ftxui::Event::CtrlU) {
    screen.filter.input.clear();
    state.status.clear();
    apply_filter(screen);
    return true;
  }
  if (event.is_character()) {
    screen.filter.input += event.character();
    state.status.clear();
    apply_filter(screen);
    return true;
  }
  return true;
}

void begin_search(TuiState &state, ContentScreen &screen) {
  screen.search.active = true;
  screen.search.input.clear();
  screen.search.origin_top_row = screen.scroll.top_row;
  screen.search.matches.clear();
  state.status.clear();
}

void preview_search(TuiState &state, ContentScreen &screen) {
  refresh_content_matches(screen, screen.search.input);
  state.status.clear();
  if (screen.search.matches.empty()) {
    screen.scroll.top_row = screen.search.origin_top_row;
    return;
  }
  const auto next =
      std::ranges::find_if(screen.search.matches, [&screen](std::size_t line) {
        return std::cmp_greater_equal(line, screen.search.origin_top_row);
      });
  screen.search.match_index =
      next == screen.search.matches.end()
          ? 0
          : static_cast<std::size_t>(next - screen.search.matches.begin());
  focus_content_line(screen, screen.search.matches[screen.search.match_index]);
}

void cancel_search(TuiState &state, ContentScreen &screen) {
  screen.search.active = false;
  screen.search.input.clear();
  screen.scroll.top_row = screen.search.origin_top_row;
  refresh_content_matches(screen, screen.search.query);
  state.status = "search canceled";
}

void commit_search(TuiState &state, ContentScreen &screen) {
  screen.search.active = false;
  screen.search.query = screen.search.input;
  refresh_content_matches(screen, screen.search.query);
  if (screen.search.query.empty()) {
    state.status = "empty search";
    screen.scroll.top_row = screen.search.origin_top_row;
    return;
  }
  if (screen.search.matches.empty()) {
    state.status = "no matches";
    screen.scroll.top_row = screen.search.origin_top_row;
    return;
  }
  state.status.clear();
  focus_content_line(screen, screen.search.matches[screen.search.match_index]);
}

bool handle_search_event(TuiState &state, ContentScreen &screen,
                         const ftxui::Event &event) {
  if (event == ftxui::Event::Escape || event == ftxui::Event::q) {
    cancel_search(state, screen);
    return true;
  }
  if (event == ftxui::Event::Return) {
    commit_search(state, screen);
    return true;
  }
  if (event == ftxui::Event::Backspace) {
    memix::tui::erase_last_utf8_code_point(screen.search.input);
    preview_search(state, screen);
    return true;
  }
  if (event == ftxui::Event::CtrlU) {
    screen.search.input.clear();
    preview_search(state, screen);
    return true;
  }
  if (event.is_character()) {
    screen.search.input += event.character();
    preview_search(state, screen);
    return true;
  }
  return true;
}

void jump_search(TuiState &state, ContentScreen &screen, bool forward) {
  if (screen.search.query.empty()) {
    state.status = "no active search";
    return;
  }
  refresh_content_matches(screen, screen.search.query);
  if (screen.search.matches.empty()) {
    state.status = "no matches";
    return;
  }
  if (forward) {
    screen.search.match_index =
        (screen.search.match_index + 1) % screen.search.matches.size();
  } else {
    screen.search.match_index = screen.search.match_index == 0
                                  ? screen.search.matches.size() - 1
                                  : screen.search.match_index - 1;
  }
  focus_content_line(screen, screen.search.matches[screen.search.match_index]);
  state.status.clear();
}

bool handle_errors_event(TuiState &state, const ftxui::Event &event) {
  if (event == ftxui::Event::E
      || event == ftxui::Event::q
      || event == ftxui::Event::Escape) {
    state.show_errors = false;
    return true;
  }
  if (event == ftxui::Event::j || event == ftxui::Event::ArrowDown) {
    scroll_by_rows(state.error_scroll, 1);
    return true;
  }
  if (event == ftxui::Event::k || event == ftxui::Event::ArrowUp) {
    scroll_by_rows(state.error_scroll, -1);
    return true;
  }
  if (event == ftxui::Event::PageDown) {
    scroll_by_rows(state.error_scroll, static_cast<int>(kPageMove));
    return true;
  }
  if (event == ftxui::Event::PageUp) {
    scroll_by_rows(state.error_scroll, -static_cast<int>(kPageMove));
    return true;
  }
  if (event == ftxui::Event::g || event == ftxui::Event::Home) {
    scroll_to_top(state.error_scroll);
    return true;
  }
  if (event == ftxui::Event::G || event == ftxui::Event::End) {
    scroll_to_bottom(state.error_scroll);
    return true;
  }
  return true;
}

bool handle_list_event(TuiState &state, ListScreen &screen,
                       ftxui::Event event, const ftxui::Closure &quit) {
  if (const int rows = wheel_rows(event); rows != 0) {
    resolve_pending_list_recenter(screen);
    const int previous = screen.scroll.top_row;
    scroll_by_rows(screen.scroll, rows);
    if (screen.scroll.top_row != previous) {
      sync_list_selection_to_viewport(screen);
    }
    return true;
  }
  if (screen.filter.active) {
    return handle_filter_event(state, screen, event);
  }
  if (event == ftxui::Event::Character('/')) {
    begin_filter(state, screen);
    return true;
  }
  if (event == ftxui::Event::Return) {
    static_cast<void>(open_selected(state));
    return true;
  }
  if (event == ftxui::Event::j || event == ftxui::Event::ArrowDown) {
    move_list_down(screen, 1);
    state.status.clear();
    return true;
  }
  if (event == ftxui::Event::k || event == ftxui::Event::ArrowUp) {
    move_list_up(screen, 1);
    state.status.clear();
    return true;
  }
  if (event == ftxui::Event::PageDown) {
    move_list_down(screen, kPageMove);
    return true;
  }
  if (event == ftxui::Event::PageUp) {
    move_list_up(screen, kPageMove);
    return true;
  }
  if (event == ftxui::Event::g || event == ftxui::Event::Home) {
    move_list_up(screen, screen.visible.size());
    return true;
  }
  if (event == ftxui::Event::G || event == ftxui::Event::End) {
    move_list_down(screen, screen.visible.size());
    return true;
  }
  if (event == ftxui::Event::r) {
    if (refresh_screen(state, state.stack.back())) {
      state.status = "refreshed";
    }
    return true;
  }
  if (event == ftxui::Event::q || event == ftxui::Event::Escape) {
    quit();
    return true;
  }
  return false;
}

bool handle_content_event(TuiState &state, ContentScreen &screen,
                          ftxui::Event event, const ftxui::Closure &quit) {
  if (const int rows = wheel_rows(event); rows != 0) {
    scroll_by_rows(screen.scroll, rows);
    return true;
  }
  if (screen.search.active) {
    return handle_search_event(state, screen, event);
  }
  if (event == ftxui::Event::Character('/')) {
    begin_search(state, screen);
    return true;
  }
  if (event == ftxui::Event::n) {
    jump_search(state, screen, true);
    return true;
  }
  if (event == ftxui::Event::N) {
    jump_search(state, screen, false);
    return true;
  }
  if (event == ftxui::Event::Return
      && screen.kind == ContentKind::Quest
      && screen.environment_line.has_value()) {
    const std::expected<void, std::string> opened =
        open_quest_environment(state, screen);
    if (!opened) {
      record_error(state, opened.error());
    }
    return true;
  }
  if (event == ftxui::Event::j || event == ftxui::Event::ArrowDown) {
    scroll_by_rows(screen.scroll, 1);
    return true;
  }
  if (event == ftxui::Event::k || event == ftxui::Event::ArrowUp) {
    scroll_by_rows(screen.scroll, -1);
    return true;
  }
  if (event == ftxui::Event::PageDown) {
    scroll_by_rows(screen.scroll, static_cast<int>(kPageMove));
    return true;
  }
  if (event == ftxui::Event::PageUp) {
    scroll_by_rows(screen.scroll, -static_cast<int>(kPageMove));
    return true;
  }
  if (event == ftxui::Event::g || event == ftxui::Event::Home) {
    scroll_to_top(screen.scroll);
    return true;
  }
  if (event == ftxui::Event::G || event == ftxui::Event::End) {
    scroll_to_bottom(screen.scroll);
    return true;
  }
  if (event == ftxui::Event::r) {
    if (refresh_screen(state, state.stack.back())) {
      state.status = "refreshed";
    }
    return true;
  }
  if (event == ftxui::Event::q || event == ftxui::Event::Escape) {
    quit();
    return true;
  }
  return false;
}

// NOLINTBEGIN(bugprone-easily-swappable-parameters)
bool
handle_event(TuiState &state, ftxui::Event event,
             const ftxui::Closure &quit, const ftxui::Closure &open_editor) {
  if (state.show_errors) {
    if (const int rows = wheel_rows(event); rows != 0) {
      scroll_by_rows(state.error_scroll, rows);
      return true;
    }
    return handle_errors_event(state, event);
  }
  if (state.delete_confirmation.has_value()) {
    return handle_delete_confirmation_event(state, event);
  }
  if (state.prompt.has_value()) {
    return handle_footer_prompt_event(state, event, open_editor);
  }

  bool input_active = false;
  if (const auto *list = std::get_if<ListScreen>(&state.stack.back())) {
    input_active = list->filter.active;
  } else {
    input_active = std::get<ContentScreen>(state.stack.back()).search.active;
  }
  if (!input_active && is_mutation_event(event)) {
    return handle_mutation_event(state, event, open_editor);
  }
  if (!input_active && event == ftxui::Event::E && !state.errors.empty()) {
    state.show_errors = true;
    state.error_scroll.top_row = 0;
    state.error_scroll.follow_focus = false;
    return true;
  }
  if (!input_active
      && (event == ftxui::Event::Character('-')
          || event == ftxui::Event::h
          || event == ftxui::Event::ArrowLeft)) {
    if (state.stack.size() > 1) {
      state.stack.pop_back();
      state.status.clear();
    }
    return true;
  }
  if (!input_active && event == ftxui::Event::b) {
    if (state.stack.size() > 1) {
      state.stack.resize(1);
      state.status.clear();
    }
    return true;
  }
  if (!input_active
      && (event == ftxui::Event::l || event == ftxui::Event::ArrowRight)) {
    event = ftxui::Event::Return;
  }

  if (auto *list = std::get_if<ListScreen>(&state.stack.back())) {
    return handle_list_event(state, *list, std::move(event), quit);
  }
  return handle_content_event(state,
                              std::get<ContentScreen>(state.stack.back()),
                              std::move(event), quit);
}
// NOLINTEND(bugprone-easily-swappable-parameters)

ftxui::Element row_spacer(std::size_t rows) {
  return ftxui::text("")
       | ftxui::size(ftxui::HEIGHT, ftxui::EQUAL, static_cast<int>(rows));
}

ftxui::Color entry_title_color(const Entry &entry, bool selected) {
  if (selected) {
    return ftxui::Color::MagentaLight;
  }
  switch (entry.action) {
  case EntryAction::OpenTypeDirectory:
  case EntryAction::OpenEnvironmentDirectory:
    return ftxui::Color::CyanLight;
  case EntryAction::OpenGenerationList:
    return ftxui::Color::YellowLight;
  default:
    return ftxui::Color::GrayLight;
  }
}

ftxui::Element
render_entry(const Entry &entry, bool selected, bool drilled, int width) {
  using namespace ftxui;
  const Color title_color = entry_title_color(entry, selected);
  // Budget: 2 cells for the selection prefix, 1 for the scroll indicator
  // the list body always reserves on the right.
  const std::size_t text_width =
      static_cast<std::size_t>(std::max(0, width - 3));
  Element title;
  if (drilled) {
    title = hbox({
        text(memix::tui::ellipsize(entry.title,
                                       text_width > 2 ? text_width - 2 : 0))
            | color(title_color)
            | xflex_shrink,
        filler(),
        text(" ▶") | color(Color::MagentaLight),
    });
  } else {
    title = text(memix::tui::ellipsize(entry.title, text_width))
          | color(title_color)
          | xflex_shrink;
  }
  Element block = vbox({
                      std::move(title),
                      text(memix::tui::ellipsize(entry.summary, text_width))
                          | color(Color::GrayDark)
                          | xflex_shrink,
                  })
                | xflex;
  if (selected) {
    return hbox({
               text("▌") | color(Color::MagentaLight),
               text(" "),
               std::move(block),
           })
         | focus;
  }
  return hbox({text("  "), std::move(block)});
}

ftxui::Element render_list_empty(const ListScreen &screen) {
  using namespace ftxui;
  Elements lines;
  if (!list_query(screen).empty() && !screen.entries.empty()) {
    lines.push_back(text("No matches") | bold | color(Color::YellowLight));
    lines.push_back(separatorEmpty());
    lines.push_back(paragraph("No entry matches \""
                              + clipped_label(list_query(screen))
                              + "\"")
                    | color(Color::GrayLight));
    return vbox(std::move(lines)) | flex;
  }
  std::string title = "Nothing here";
  std::string explanation = "Refresh the store to try again.";
  switch (screen.kind) {
  case ListKind::TypeTree:
    title = "No files";
    explanation = "Run memix-store ingest with --dest to add context files.";
    break;
  case ListKind::Generations:
    title = "No generations";
    explanation = "Ingest a context file to create the first generation.";
    break;
  case ListKind::Objects:
    title = "No objects";
    explanation = "Run memix-store ingest to add blobs to this store.";
    break;
  case ListKind::EnvironmentList:
    title = "No environments";
    explanation = "Build a view to create a locked environment.";
    break;
  case ListKind::EnvironmentTree:
    title = "Empty environment";
    explanation = "This locked environment contains no files.";
    break;
  case ListKind::QuestList:
    title = "No quests";
    explanation = "Define a quest with memix-view quest define.";
    break;
  case ListKind::Store:
    title = "Empty store";
    explanation = "Run memix-store ingest to add content.";
    break;
  }
  lines.push_back(text(title) | bold | color(Color::RedLight));
  lines.push_back(separatorEmpty());
  lines.push_back(paragraph(explanation) | color(Color::YellowLight));
  return vbox(std::move(lines)) | flex;
}

std::string list_cursor(const ListScreen &screen) {
  if (screen.visible.empty()) {
    return "0/0";
  }
  return std::format("{}/{}", screen.selected + 1, screen.visible.size());
}

struct StatusBarFields {
  std::string count;
  std::string cursor;
  std::string scope;
  std::string query;
  std::string query_label;
};

ftxui::Element status_bar(const TuiState &state, StatusBarFields fields) {
  using namespace ftxui;
  Elements items{
      text(" memix ")
          | bold
          | color(Color::Black)
          | bgcolor(Color::MagentaLight),
      text(" · " + std::move(fields.count)) | color(Color::GrayLight),
      text(" · " + std::move(fields.cursor)) | color(Color::GrayLight),
      text(" · " + std::move(fields.scope)) | bold | color(Color::White),
      text(" · " + state.root.string()) | color(Color::GrayDark) | xflex_shrink,
  };
  if (!state.errors.empty()) {
    items.push_back(text(" "));
    Element errors =
        text(" " + pluralize(state.errors.size(), "error", "errors") + " ")
        | color(Color::YellowLight);
    if (state.show_errors) {
      errors = errors | inverted;
    }
    items.push_back(std::move(errors));
  }
  if (!state.status.empty()) {
    items.push_back(
        text("  " + state.status) | color(Color::YellowLight) | xflex_shrink);
  }
  if (!fields.query.empty()) {
    items.push_back(text("  "
                         + fields.query_label
                         + " \""
                         + clipped_label(fields.query)
                         + "\"")
                    | color(Color::CyanLight)
                    | xflex_shrink);
  }
  return hbox(std::move(items));
}

ftxui::Element footer_prompt(const FooterPrompt &prompt) {
  using namespace ftxui;
  const bool name_prompt = prompt.kind == FooterPromptKind::NewFileName;
  return hbox({
      text(name_prompt ? "name: " : "message: ")
          | bold
          | color(Color::CyanLight),
      text(prompt.input) | color(Color::White),
      text(name_prompt ? "  enter open editor  esc cancel  backspace edit"
                       : "  enter save  esc cancel  backspace edit")
          | color(Color::GrayDark),
  });
}

ftxui::Element
list_footer(const TuiState &state, const ListScreen &screen) {
  using namespace ftxui;
  Element help;
  if (state.prompt.has_value()) {
    help = footer_prompt(*state.prompt);
  } else if (screen.filter.active) {
    help = hbox({
        text("/") | bold | color(Color::CyanLight),
        text(screen.filter.input) | color(Color::White),
        text("  enter filter  esc cancel  backspace edit")
            | color(Color::GrayDark),
    });
  } else {
    std::string text_value =
        "h/l columns  j/k move  PgUp/Dn  g/G first/last  / filter  "
        "enter open  r refresh";
    if (screen.kind == ListKind::TypeTree && !screen.pinned_to_non_latest) {
      text_value += "  a add";
      const Entry *entry = selected_entry(screen);
      if (entry != nullptr && entry->action == EntryAction::OpenTypeFile) {
        text_value += "  e edit  d delete";
      }
    }
    if (state.stack.size() > 1) {
      text_value += "  - up  b store";
    }
    if (!state.errors.empty()) {
      text_value += "  E errors";
    }
    text_value += "  q quit";
    help = text(std::move(text_value)) | color(Color::GrayDark);
  }
  return hbox({
      std::move(help) | flex,
      text(" "),
      scroll_progress_indicator(screen.scroll) | color(Color::GrayLight),
  });
}

ftxui::Element render_list_body(ListScreen &screen, bool ancestor, int width) {
  using namespace ftxui;
  Elements rows;
  if (screen.visible.empty()) {
    rows.push_back(render_list_empty(screen));
    return line_frame(vbox(std::move(rows)) | vscroll_indicator, screen.scroll)
         | flex;
  }

  const int viewport =
      screen.scroll.viewport_rows > 0 ? screen.scroll.viewport_rows : 60;
  const int total_rows =
      static_cast<int>(screen.visible.size()) * kRowsPerEntry;
  const int max_top = max_top_row_for(total_rows, viewport);
  // Resolve a pending follow-focus recenter here with the same geometry
  // LineFrame applies during layout; the virtualized window must contain the
  // selected entry or the scroller would render spacers over the focus.
  if (screen.scroll.follow_focus) {
    const int first_row = static_cast<int>(screen.selected) * kRowsPerEntry;
    screen.scroll.top_row = std::clamp(
        centered_top_row(first_row, first_row + 1, viewport), 0, max_top);
  }
  constexpr std::size_t overscan_rows =
      std::size_t{2} * static_cast<std::size_t>(kRowsPerEntry);
  const memix::tui::RowWindow window =
      memix::tui::virtualized_row_window({
          .total_rows = static_cast<std::size_t>(total_rows),
          .viewport_rows = static_cast<std::size_t>(viewport),
          .requested_top_row = screen.scroll.top_row,
          .overscan = overscan_rows,
      });
  screen.scroll.top_row = static_cast<int>(window.top);
  screen.scroll.max_top_row = max_top;

  const auto entry_rows = static_cast<std::size_t>(kRowsPerEntry);
  const std::size_t first_entry = window.first / entry_rows;
  const std::size_t last_entry = std::min(
      screen.visible.size(), (window.last + entry_rows - 1) / entry_rows);
  if (first_entry > 0) {
    rows.push_back(row_spacer(first_entry * entry_rows));
  }
  for (std::size_t visible = first_entry; visible < last_entry; ++visible) {
    const bool selected = visible == screen.selected;
    rows.push_back(render_entry(screen.entries[screen.visible[visible]],
                                selected, ancestor && selected, width));
    rows.push_back(separatorEmpty());
  }
  if (last_entry < screen.visible.size()) {
    rows.push_back(
        row_spacer((screen.visible.size() - last_entry) * entry_rows));
  }
  return line_frame(vbox(std::move(rows)) | vscroll_indicator, screen.scroll)
       | flex;
}

ftxui::Element render_list(const TuiState &state, ListScreen &screen) {
  using namespace ftxui;
  return vbox({
             status_bar(state,
                        StatusBarFields{
                            .count = screen.count,
                            .cursor = list_cursor(screen),
                            .scope = screen.scope,
                            .query = list_query(screen),
                            .query_label = "filter",
                        }),
             separatorEmpty(),
             render_list_body(screen, false, Terminal::Size().dimx),
             separatorEmpty(),
             list_footer(state, screen),
         })
       | flex;
}

std::string visible_search_query(const ContentScreen &screen) {
  return screen.search.active ? screen.search.input : screen.search.query;
}

struct MatchRequest {
  std::string_view text;
  std::string_view query;
};

std::vector<std::size_t> match_offsets(MatchRequest request) {
  std::vector<std::size_t> offsets;
  if (request.query.empty()) {
    return offsets;
  }
  const std::string lower_text = memix::tui::lowercase_copy(request.text);
  const std::string lower_query = memix::tui::lowercase_copy(request.query);
  std::size_t offset = 0;
  while (offset <= lower_text.size()) {
    const std::size_t found = lower_text.find(lower_query, offset);
    if (found == std::string::npos) {
      break;
    }
    offsets.push_back(found);
    offset = found + std::max<std::size_t>(lower_query.size(), 1);
  }
  return offsets;
}

ftxui::Element render_content_line(std::string_view line,
                                   std::string_view query, bool current) {
  using namespace ftxui;
  const std::string expanded = expand_tabs(line);
  line = expanded;
  const std::vector<std::size_t> offsets =
      match_offsets({.text = line, .query = query});
  if (offsets.empty()) {
    return text(line) | color(Color::GrayLight);
  }
  Elements pieces;
  std::size_t cursor = 0;
  for (const std::size_t offset : offsets) {
    if (offset > cursor) {
      pieces.push_back(text(line.substr(cursor, offset - cursor))
                       | color(Color::GrayLight));
    }
    pieces.push_back(
        text(line.substr(offset, query.size()))
        | color(Color::Black)
        | bgcolor(current ? Color::MagentaLight : Color::YellowLight));
    cursor = offset + query.size();
  }
  if (cursor < line.size()) {
    pieces.push_back(text(line.substr(cursor)) | color(Color::GrayLight));
  }
  return hbox(std::move(pieces));
}

ftxui::Element
content_footer(const TuiState &state, const ContentScreen &screen) {
  using namespace ftxui;
  Element help;
  if (state.prompt.has_value()) {
    help = footer_prompt(*state.prompt);
  } else if (screen.search.active) {
    help = hbox({
        text("/") | bold | color(Color::CyanLight),
        text(screen.search.input) | color(Color::White),
        text("  enter search  esc cancel  backspace edit")
            | color(Color::GrayDark),
    });
  } else {
    std::string value =
        "h/l columns  j/k scroll  PgUp/Dn  g/G top/bottom  / search  n/N";
    if (screen.kind == ContentKind::Quest
        && screen.environment_line.has_value()) {
      value += "  enter open env";
    }
    if (screen.kind == ContentKind::StoreFile && !screen.pinned_to_non_latest) {
      value += "  e edit  d delete";
    }
    value += "  - back";
    if (!state.errors.empty()) {
      value += "  E errors";
    }
    value += "  q quit";
    help = text(std::move(value)) | color(Color::GrayDark);
  }
  return hbox({
      std::move(help) | flex,
      text(" "),
      scroll_progress_indicator(screen.scroll) | color(Color::GrayLight),
  });
}

std::string content_cursor(const ContentScreen &screen) {
  if (screen.lines.empty()) {
    return "0/0";
  }
  const std::size_t top = static_cast<std::size_t>(std::clamp(
      screen.scroll.top_row, 0, static_cast<int>(screen.lines.size() - 1)));
  return std::format("{}/{}", top + 1, screen.lines.size());
}

ftxui::Element render_content_body(ContentScreen &screen) {
  using namespace ftxui;
  const std::string query = visible_search_query(screen);
  const std::size_t current_match =
      screen.search.matches.empty()
          ? std::numeric_limits<std::size_t>::max()
          : screen.search.matches[screen.search.match_index];
  const int viewport =
      screen.scroll.viewport_rows > 0 ? screen.scroll.viewport_rows : 60;
  const std::size_t total = screen.lines.size();
  constexpr std::size_t overscan = 2;
  const memix::tui::RowWindow window =
      memix::tui::virtualized_row_window({
          .total_rows = total,
          .viewport_rows = static_cast<std::size_t>(viewport),
          .requested_top_row = screen.scroll.top_row,
          .overscan = overscan,
      });
  screen.scroll.top_row = static_cast<int>(window.top);
  screen.scroll.max_top_row = static_cast<int>(memix::tui::max_scroll_top(
      total, static_cast<std::size_t>(viewport)));
  const std::size_t first = window.first;
  const std::size_t last = window.last;

  Elements rows;
  if (first > 0) {
    rows.push_back(row_spacer(first));
  }
  for (std::size_t index = first; index < last; ++index) {
    Element line =
        render_content_line(screen.lines[index], query, index == current_match);
    if (screen.environment_line == index) {
      line = hbox({
          text("▌") | color(Color::MagentaLight),
          text(" "),
          std::move(line) | color(Color::MagentaLight),
      });
    } else {
      line = hbox({text("  "), std::move(line)});
    }
    rows.push_back(std::move(line));
  }
  if (last < total) {
    rows.push_back(row_spacer(total - last));
  }
  return line_frame(vbox(std::move(rows)) | vscroll_indicator, screen.scroll)
       | flex;
}

ftxui::Element render_content(const TuiState &state, ContentScreen &screen) {
  using namespace ftxui;
  const std::string query = visible_search_query(screen);
  return vbox({
             status_bar(state,
                        StatusBarFields{
                            .count = screen.count,
                            .cursor = content_cursor(screen),
                            .scope = screen.scope,
                            .query = query,
                            .query_label = "search",
                        }),
             separatorEmpty(),
             render_content_body(screen),
             separatorEmpty(),
             content_footer(state, screen),
         })
       | flex;
}

std::string list_column_title(const ListScreen &screen) {
  switch (screen.kind) {
  case ListKind::Store:
    return "Store";
  case ListKind::TypeTree:
    if (screen.path.empty()) {
      return std::format("{}/", memix::to_string(screen.type));
    }
    return memix::tui::normalize_line_breaks(
               screen.path.filename().generic_string())
         + "/";
  case ListKind::Generations:
    return std::format("{} generations", memix::to_string(screen.type));
  case ListKind::Objects:
    return "Objects";
  case ListKind::EnvironmentList:
    return "Environments";
  case ListKind::EnvironmentTree:
    if (screen.path.empty()) {
      return std::format("env {}/",
                         memix::tui::short_id(screen.environment_id));
    }
    return memix::tui::normalize_line_breaks(
               screen.path.filename().generic_string())
         + "/";
  case ListKind::QuestList:
    return "Quests";
  }
  return {};
}

ftxui::Element render_stack_column(TuiScreen &screen, bool active,
                                   bool ancestors_dropped, int width) {
  using namespace ftxui;
  std::string title;
  Element body;
  if (auto *list = std::get_if<ListScreen>(&screen)) {
    title = list_column_title(*list);
    if (list_query(*list).empty()) {
      title = std::format("{} · {}", title, list->entries.size());
    } else {
      title = std::format("{} · {}/{}", title, list->visible.size(),
                          list->entries.size());
    }
    body = render_list_body(*list, !active, width);
  } else {
    auto &content = std::get<ContentScreen>(screen);
    title = memix::tui::normalize_line_breaks(content.title);
    body = render_content_body(content);
  }
  if (ancestors_dropped) {
    title = "… " + title;
  }
  title = memix::tui::ellipsize(
      title, static_cast<std::size_t>(std::max(0, width)));
  Element column = vbox({
      text(std::move(title))
          | bold
          | color(active ? Color::MagentaLight : Color::GrayLight)
          | xflex_shrink,
      separatorEmpty(),
      std::move(body),
  });
  if (!active) {
    column = column | dim;
  }
  return column;
}

ftxui::Element
render_column_layout(TuiState &state,
                     const memix::tui::ColumnWindow &window) {
  using namespace ftxui;
  Elements columns;
  for (std::size_t offset = 0; offset < window.widths.size(); ++offset) {
    const std::size_t stack_index = window.first_visible + offset;
    const bool active = stack_index + 1 == state.stack.size();
    const bool ancestors_dropped = offset == 0 && window.first_visible > 0;
    TuiScreen &screen = state.stack[stack_index];
    columns.push_back(
        render_stack_column(screen, active, ancestors_dropped,
                            static_cast<int>(window.widths[offset]))
        | size(WIDTH, EQUAL, static_cast<int>(window.widths[offset])));
    if (std::holds_alternative<ListScreen>(screen)) {
      columns.push_back(
          hbox({text(" "), separator() | color(Color::GrayDark)}));
    }
  }
  columns.push_back(filler() | flex);

  TuiScreen &active = state.stack.back();
  Element status;
  Element footer;
  if (const auto *list = std::get_if<ListScreen>(&active)) {
    status = status_bar(state, StatusBarFields{
                                   .count = list->count,
                                   .cursor = list_cursor(*list),
                                   .scope = list->scope,
                                   .query = list_query(*list),
                                   .query_label = "filter",
                               });
    footer = list_footer(state, *list);
  } else {
    const auto &content = std::get<ContentScreen>(active);
    status = status_bar(state, StatusBarFields{
                                   .count = content.count,
                                   .cursor = content_cursor(content),
                                   .scope = content.scope,
                                   .query = visible_search_query(content),
                                   .query_label = "search",
                               });
    footer = content_footer(state, content);
  }

  return vbox({
             std::move(status),
             separatorEmpty(),
             hbox(std::move(columns)) | flex,
             separatorEmpty(),
             std::move(footer),
         })
       | flex;
}

ftxui::Element render_errors(TuiState &state) {
  using namespace ftxui;
  Elements rows{
      text(pluralize(state.errors.size(), "error", "errors"))
          | bold
          | color(Color::YellowLight),
      separatorEmpty(),
  };
  for (const std::string &error : state.errors) {
    rows.push_back(paragraph(error) | color(Color::YellowLight));
    rows.push_back(separatorEmpty());
  }
  const Element footer = hbox({
      text("j/k scroll  PgUp/Dn  g/G top/bottom  E close  q close")
          | color(Color::GrayDark)
          | flex,
      text(" "),
      scroll_progress_indicator(state.error_scroll) | color(Color::GrayLight),
  });
  return vbox({
             status_bar(
                 state,
                 StatusBarFields{
                     .count = pluralize(state.errors.size(), "error", "errors"),
                     .cursor = "0/0",
                     .scope = "errors",
                     .query = {},
                     .query_label = {},
                 }),
             separatorEmpty(),
             line_frame(vbox(std::move(rows)) | vscroll_indicator,
                        state.error_scroll)
                 | flex,
             separatorEmpty(),
             footer,
         })
       | flex;
}

ftxui::Element render_main_app(TuiState &state) {
  std::vector<memix::tui::ColumnKind> shape;
  shape.reserve(state.stack.size());
  for (const TuiScreen &screen : state.stack) {
    shape.push_back(std::holds_alternative<ListScreen>(screen)
                        ? memix::tui::ColumnKind::List
                        : memix::tui::ColumnKind::Content);
  }
  const int terminal_columns = ftxui::Terminal::Size().dimx;
  const memix::tui::ColumnWindow window = memix::tui::column_window(
      shape, static_cast<std::size_t>(std::max(terminal_columns, 0)));
  if (!window.single_screen) {
    return render_column_layout(state, window);
  }
  if (auto *list = std::get_if<ListScreen>(&state.stack.back())) {
    return render_list(state, *list);
  }
  return render_content(state, std::get<ContentScreen>(state.stack.back()));
}

ftxui::Element
render_delete_confirmation(ftxui::Element background,
                           const DeleteConfirmation &confirmation) {
  using namespace ftxui;
  const std::string logical_path =
      std::format("{}/{}", memix::to_string(confirmation.type),
                  confirmation.path.generic_string());
  Element dialog =
      window(text(" Confirm delete ") | bold | color(Color::YellowLight),
             vbox({
                 paragraph("Remove this file from the latest generation?"),
                 separatorEmpty(),
                 text("path: " + logical_path) | color(Color::White),
                 text("blob: " + memix::tui::short_id(confirmation.blob_id))
                     | color(Color::GrayLight),
                 separatorEmpty(),
                 text("enter confirm  esc cancel") | color(Color::GrayDark),
             }))
      | size(WIDTH, LESS_THAN, kDeleteDialogWidth)
      | clear_under
      | center;
  return dbox({std::move(background), std::move(dialog)});
}

ftxui::Element render_app(TuiState &state) {
  if (state.show_errors) {
    return render_errors(state);
  }
  ftxui::Element app = render_main_app(state);
  if (state.delete_confirmation.has_value()) {
    return render_delete_confirmation(std::move(app),
                                      *state.delete_confirmation);
  }
  return app;
}

std::expected<fs::path, std::string>
resolve_store_root(const fs::path &override_root) {
  if (!override_root.empty()) {
    return override_root;
  }
  const memix::StoreResult<fs::path> root = memix::default_store_root();
  if (!root) {
    return std::unexpected(root.error().message);
  }
  return *root;
}

} // namespace

int main(int argc, char **argv) {
  const CliParseResult cli = parse_cli_args(argc, argv);
  if (!cli.error.empty()) {
    std::cerr << "memix-tui: " << cli.error << '\n' << cli.help << '\n';
    return EXIT_FAILURE;
  }
  if (cli.options.help_requested) {
    std::cout << cli.help;
    return EXIT_SUCCESS;
  }
  if (cli.options.version_requested) {
    std::cout << "memix " << MEMIX_VERSION << '\n';
    return EXIT_SUCCESS;
  }

  const std::expected<fs::path, std::string> root =
      resolve_store_root(cli.options.store_root);
  if (!root) {
    std::cerr << "memix-tui: " << root.error() << '\n';
    return EXIT_FAILURE;
  }
  TuiState state(*root);
  const memix::scope_exit pending_temp_cleanup(
      [&state] { discard_pending_mutation(state); });
  state.stack.emplace_back(ListScreen(ListKind::Store));
  static_cast<void>(
      load_store_screen(state, std::get<ListScreen>(state.stack.back())));

  if (!cli.options.locator.empty()) {
    const memix::tui::LocatorResult locator =
        memix::tui::parse_locator(cli.options.locator);
    if (!locator) {
      std::cerr << "memix-tui: " << locator.error() << '\n';
      return EXIT_FAILURE;
    }
    const std::expected<void, std::string> opened =
        open_locator(state, *locator);
    if (!opened) {
      std::cerr << "memix-tui: " << opened.error() << '\n';
      return EXIT_FAILURE;
    }
  }

  auto app = ftxui::App::Fullscreen();
  app.TrackMouse(true);
  const ftxui::Closure quit = app.ExitLoopClosure();
  const ftxui::Closure open_editor =
      app.WithRestoredIO([&state] { run_editor_request(state); });
  auto component = ftxui::Renderer([&state] { return render_app(state); });
  component |=
      ftxui::CatchEvent([&state, &quit, &open_editor](ftxui::Event event) {
        return handle_event(state, std::move(event), quit, open_editor);
      });
  app.Loop(component);
  return EXIT_SUCCESS;
}
