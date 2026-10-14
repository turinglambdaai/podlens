// PodLens Linux host — GTK4 window over one embedded Racket CS backend.
//
// Interaction contract mirrors the Windows host (windows/MainWindow.xaml.cpp)
// element for element: subscriptions in the sidebar, episode cards with
// status chips, one-click Understand with a cost estimate, read-only
// transcript/summary panels, and resume-on-launch. UI text is English,
// matching the Windows host's current state (no i18n layer on this host).
//
// Threading: boot the runtime off the UI thread; every completion/event is
// dispatched back to the main loop (g_idle_add) before touching widgets.
#include <gtk/gtk.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <regex>
#include <string>
#include <thread>
#include <type_traits>
#include <unistd.h>
#include <vector>

#include "GeneratedBackend.hpp"
#include "system_services.hpp"

namespace {

void set_status(char const* message);
void set_status_error(std::string const& message);

// ---------------------------------------------------------------------------
// UI dispatch (completions/events arrive on the backend reader thread)
// ---------------------------------------------------------------------------

struct UiTask {
  std::function<void()> run;
};

int run_ui_task(gpointer user_data) {
  auto* task = static_cast<UiTask*>(user_data);
  task->run();
  delete task;
  return G_SOURCE_REMOVE;
}

void dispatch_ui(std::function<void()> run) {
  g_idle_add(run_ui_task, new UiTask{std::move(run)});
}

template <typename T, typename Then>
void on_result(rivet_app::Result<T> result, Then then) {
  dispatch_ui([result = std::move(result),
               then = std::move(then)]() mutable {
    if (!result.succeeded()) {
      try {
        std::rethrow_exception(result.error);
      } catch (std::exception const& e) {
        set_status_error(e.what());
      } catch (...) {
        set_status_error("unknown backend failure");
      }
      return;
    }
    try {
      if constexpr (std::is_void_v<T>) {
        then();
      } else {
        then(result.get());
      }
    } catch (std::exception const& e) {
      set_status_error(e.what());
    }
  });
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

std::filesystem::path executable_path() {
  return std::filesystem::read_symlink("/proc/self/exe");
}

std::string format_seconds(double seconds) {
  if (seconds < 0 || !std::isfinite(seconds)) seconds = 0;
  char buffer[16];
  std::snprintf(buffer, sizeof(buffer), "%02d:%02d",
                static_cast<int>(seconds) / 60,
                static_cast<int>(seconds) % 60);
  return buffer;
}

// The estimate RPC answers with a small JSON object; picking the numbers out
// with a regex beats carrying a JSON parser in the host (same rule as the
// Windows host). Absent fields surface as -1.
int estimate_int(std::string const& json, char const* key) {
  std::regex const re(std::string("\"") + key + "\":([0-9]+)");
  std::smatch m;
  if (std::regex_search(json, m, re)) return std::stoi(m[1].str());
  return -1;
}

// One line summarizing what a pipeline run would do (the BYOK bill).
std::string estimate_line(std::string const& json) {
  std::string text;
  auto const append = [&text](std::string piece) {
    if (!text.empty()) text += " · ";
    text += piece;
  };
  if (!json.empty()) {
    int const minutes = estimate_int(json, "durationSec") / 60;
    if (minutes > 0) append("~" + std::to_string(minutes) + " min");
    int const sentences = estimate_int(json, "sentences");
    if (sentences >= 0) append(std::to_string(sentences) + " sentences");
    int const chars = estimate_int(json, "chars");
    if (chars >= 0) {
      append("will translate ~" + std::to_string(chars) + " characters");
    }
    if (json.find("\"translated\":true") != std::string::npos) {
      append("translated");
    }
    if (json.find("\"summarized\":true") != std::string::npos) {
      append("summarized");
    }
  }
  return text;
}

// ---------------------------------------------------------------------------
// View model + state
// ---------------------------------------------------------------------------

struct FeedRow {
  std::string id;
  std::string title;
  std::string count;
  int unplayed{0};
};

struct EpisodeRow {
  std::string id;
  std::string title;
  std::string pub;
  double duration_sec{0};
  bool downloaded{false};
  std::string transcript_status;
  std::string summary_status;
  double position_sec{0};
  bool done{false};
  bool has_translation{false};
  std::string description;
};

struct AppState {
  GtkWindow* window{nullptr};
  GtkButton* pipeline_button{nullptr};
  GtkLabel* feeds_caption{nullptr};
  GtkListBox* feed_list{nullptr};
  GtkLabel* episodes_caption{nullptr};
  GtkListBox* episode_list{nullptr};
  GtkStack* list_stack{nullptr};
  GtkLabel* empty_icon{nullptr};
  GtkLabel* empty_text{nullptr};

  // Detail pane
  GtkBox* detail_box{nullptr};
  GtkLabel* detail_title{nullptr};
  GtkLabel* detail_meta{nullptr};
  GtkLabel* detail_notes{nullptr};
  GtkProgressBar* job_bar{nullptr};
  GtkLabel* job_label{nullptr};
  GtkLabel* estimate_label{nullptr};
  GtkStack* reading_stack{nullptr};
  GtkBox* transcript_box{nullptr};
  GtkBox* summary_box{nullptr};

  GtkLabel* status_text{nullptr};

  std::unique_ptr<rivet::linux_runtime::Backend> backend;
  std::unique_ptr<rivet_app::API> api;
  std::mutex startup_mutex;
  std::thread startup_thread;
  std::unique_ptr<rivet::linux_runtime::Backend> startup_backend;
  std::string startup_error;
  std::atomic<bool> shutting_down{false};
  // Arbitrates on_shutdown between the GTK shutdown signal and the
  // SIGTERM/SIGINT hook — both paths must tear down exactly once.
  std::atomic<bool> teardown_started{false};

  std::vector<FeedRow> feeds;
  std::vector<EpisodeRow> episodes;
  std::vector<std::vector<std::string>> transcript;
  std::string summary_json;
  std::string selected_feed_id;
  std::string selected_episode_id;
  std::string pending_resume_feed_id;
  std::string pending_resume_episode_id;
  bool suppress_feed_selection{false};
  bool suppress_episode_selection{false};

  // The one job whose progress the pipeline bar is tracking.
  std::string active_job_id;
  std::string active_job_episode;
};

AppState g_state;

// Forward declarations.
void rebuild_episode_list();
void reload_feeds();
void reload_episodes();
void load_transcript();
void load_summary();
void load_estimate();
EpisodeRow const* selected_episode();
void update_action_buttons();
void render_transcript_public();
void render_summary_public();

// ---------------------------------------------------------------------------
// Status bar
// ---------------------------------------------------------------------------

void set_status(char const* message) {
  gtk_label_set_text(g_state.status_text, message);
}

void set_status_error(std::string const& message) {
  set_status(message.c_str());
}

// ---------------------------------------------------------------------------
// CSS — light custom styling over the theme defaults; alpha-based fills keep
// the cards legible under both light and dark system palettes.
// ---------------------------------------------------------------------------

void apply_css() {
  auto* provider = gtk_css_provider_new();
  gtk_css_provider_load_from_string(
      provider,
      ".pl-title { font-weight: 600; font-size: 15px; }\n"
      ".pl-meta { font-size: 12px; opacity: 0.75; }\n"
      ".pl-caption { font-size: 12px; font-weight: 700; opacity: 0.65; "
      "padding: 8px 12px 4px 12px; }\n"
      ".pl-card { padding: 8px 10px; border-radius: 8px; }\n"
      ".pl-card:hover { background: alpha(currentColor, 0.06); }\n"
      ".pl-chip { font-size: 11px; padding: 1px 7px; border-radius: 9px; "
      "background: alpha(currentColor, 0.10); }\n"
      ".pl-empty { font-size: 26px; }\n"
      ".pl-seg { padding: 6px 10px; border-radius: 6px; }\n"
      ".pl-seg:hover { background: alpha(currentColor, 0.05); }\n"
      ".pl-seg-time { font-family: monospace; font-size: 12px; "
      "opacity: 0.65; }\n"
      ".pl-section { font-size: 13px; font-weight: 700; opacity: 0.85; "
      "margin-top: 10px; }\n"
      ".pl-statusbar { border-top: 1px solid alpha(currentColor, 0.15); "
      "padding: 3px 12px; }\n");
  gtk_style_context_add_provider_for_display(
      gdk_display_get_default(), GTK_STYLE_PROVIDER(provider),
      GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
  g_object_unref(provider);
}

// ---------------------------------------------------------------------------
// Context menu helper — a transient popover menu anchored to a widget.
// ---------------------------------------------------------------------------

void popup_context_menu(GtkWidget* anchor, GMenuModel* model, double x,
                        double y) {
  auto* popover = gtk_popover_menu_new_from_model(model);
  gtk_widget_set_parent(popover, anchor);
  GdkRectangle rect{static_cast<int>(x), static_cast<int>(y), 0, 0};
  gtk_popover_set_pointing_to(GTK_POPOVER(popover), &rect);
  gtk_popover_set_position(GTK_POPOVER(popover), GTK_POS_BOTTOM);
  g_signal_connect(popover, "closed",
                   G_CALLBACK(+[](GtkPopover* popover, gpointer) {
                     gtk_widget_unparent(GTK_WIDGET(popover));
                   }),
                   nullptr);
  gtk_popover_present(GTK_POPOVER(popover));
}

// Carries a C++ value on a GObject for the duration of one dialog.
template <typename T>
void carry_data(GObject* object, char const* key, T value) {
  g_object_set_data_full(object, key, new T(std::move(value)),
                         +[](gpointer data) { delete static_cast<T*>(data); });
}

template <typename T>
T const* carried_data(GObject* object, char const* key) {
  return static_cast<T const*>(g_object_get_data(object, key));
}

// ---------------------------------------------------------------------------
// Sidebar — feeds
// ---------------------------------------------------------------------------

GtkWidget* make_feed_row(FeedRow const& feed) {
  auto* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
  auto* title = gtk_label_new(feed.title.c_str());
  gtk_label_set_ellipsize(GTK_LABEL(title), PANGO_ELLIPSIZE_END);
  gtk_label_set_xalign(GTK_LABEL(title), 0.0f);
  gtk_widget_add_css_class(title, "pl-title");
  gtk_box_append(GTK_BOX(box), title);
  std::string meta = feed.count + " episodes";
  if (feed.unplayed > 0) {
    meta += " · " + std::to_string(feed.unplayed) + " unplayed";
  }
  auto* meta_label = gtk_label_new(meta.c_str());
  gtk_label_set_ellipsize(GTK_LABEL(meta_label), PANGO_ELLIPSIZE_END);
  gtk_label_set_xalign(GTK_LABEL(meta_label), 0.0f);
  gtk_widget_add_css_class(meta_label, "pl-meta");
  gtk_box_append(GTK_BOX(box), meta_label);

  auto* row = gtk_list_box_row_new();
  gtk_widget_set_margin_top(box, 4);
  gtk_widget_set_margin_bottom(box, 4);
  gtk_widget_set_margin_start(box, 6);
  gtk_widget_set_margin_end(box, 6);
  gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), box);
  carry_data<std::string>(G_OBJECT(row), "feed-id", feed.id);
  return row;
}

void render_feeds(std::vector<std::vector<std::string>> const& rows) {
  g_state.feeds.clear();
  auto* list = g_state.feed_list;
  g_state.suppress_feed_selection = true;
  while (GtkWidget* child = gtk_widget_get_first_child(GTK_WIDGET(list))) {
    gtk_list_box_remove(list, child);
  }
  g_state.suppress_feed_selection = false;

  for (auto const& row : rows) {
    if (row.size() < 5) continue;
    FeedRow feed;
    feed.id = row[0];
    feed.title = row[1];
    feed.count = row[4];
    feed.unplayed = row.size() > 7 ? std::atoi(row[7].c_str()) : 0;
    g_state.feeds.push_back(feed);
    gtk_list_box_append(list, make_feed_row(feed));
  }

  bool const empty = g_state.feeds.empty();
  gtk_stack_set_visible_child_name(GTK_STACK(g_state.list_stack),
                                   empty ? "empty" : "list");
  if (empty) {
    gtk_label_set_text(g_state.empty_icon, "📻");
    gtk_label_set_text(
        g_state.empty_text,
        "No subscriptions yet — add a feed with ＋ Add Feed.");
  }

  // Keep or restore the selection: resume-on-launch selects its feed once
  // the list exists; otherwise the previously selected feed survives a
  // refresh.
  GtkListBoxRow* select = nullptr;
  std::string const wanted = !g_state.pending_resume_feed_id.empty()
                                 ? g_state.pending_resume_feed_id
                                 : g_state.selected_feed_id;
  g_state.pending_resume_feed_id.clear();
  if (!wanted.empty()) {
    int index = 0;
    for (auto const& feed : g_state.feeds) {
      if (feed.id == wanted) {
        select = gtk_list_box_get_row_at_index(list, index);
        break;
      }
      ++index;
    }
  }
  if (select != nullptr) {
    gtk_list_box_select_row(list, select);
  } else {
    g_state.selected_feed_id.clear();
    g_state.episodes.clear();
    g_state.selected_episode_id.clear();
    gtk_label_set_text(g_state.episodes_caption, "Episodes");
    rebuild_episode_list();
    gtk_label_set_text(g_state.detail_title, "");
    gtk_label_set_text(g_state.detail_meta, "");
    gtk_label_set_text(g_state.detail_notes, "");
    gtk_widget_set_visible(GTK_WIDGET(g_state.detail_box), FALSE);
  }
  update_action_buttons();
}

void reload_feeds() {
  if (g_state.api == nullptr) return;
  (void)g_state.api->feed_list_async(
      [](rivet_app::Result<std::vector<std::vector<std::string>>> result) {
        on_result(std::move(result),
                  [](std::vector<std::vector<std::string>> const& rows) {
                    render_feeds(rows);
                  });
      });
}

void on_feed_row_selected(GtkListBox*, GtkListBoxRow* row, gpointer) {
  if (g_state.suppress_feed_selection) return;
  if (row == nullptr) return;
  auto const* id = carried_data<std::string>(G_OBJECT(row), "feed-id");
  if (id == nullptr) return;
  if (g_state.selected_feed_id == *id) return;  // already showing this feed
  g_state.selected_feed_id = *id;
  for (auto const& feed : g_state.feeds) {
    if (feed.id == *id) {
      gtk_label_set_text(g_state.episodes_caption, feed.title.c_str());
      break;
    }
  }
  reload_episodes();
}

// Walks up from a gesture widget to the row carrying `key`.
GtkWidget* ancestor_row(GtkWidget* widget) {
  GtkWidget* row = widget;
  while (row != nullptr && !GTK_IS_LIST_BOX_ROW(row)) {
    row = gtk_widget_get_parent(row);
  }
  return row;
}

void on_feed_row_pressed(GtkGestureClick* gesture, int, double x, double y,
                         gpointer) {
  if (gtk_gesture_single_get_current_button(GTK_GESTURE_SINGLE(gesture)) !=
      GDK_BUTTON_SECONDARY) {
    return;
  }
  GtkWidget* row = ancestor_row(gtk_event_controller_get_widget(
      GTK_EVENT_CONTROLLER(gesture)));
  if (row == nullptr) return;
  auto const* id = carried_data<std::string>(G_OBJECT(row), "feed-id");
  if (id == nullptr) return;

  auto* item = g_menu_item_new("Remove This Feed", nullptr);
  g_menu_item_set_action_and_target_value(
      item, "win.feed-remove", g_variant_new_string(id->c_str()));
  auto* menu = g_menu_new();
  g_menu_append_item(menu, item);
  g_object_unref(item);
  popup_context_menu(row, G_MENU_MODEL(menu), x, y);
  g_object_unref(menu);
}

// ---------------------------------------------------------------------------
// Episodes
// ---------------------------------------------------------------------------

GtkWidget* make_chip(char const* text) {
  auto* chip = gtk_label_new(text);
  gtk_widget_add_css_class(chip, "pl-chip");
  return chip;
}

GtkWidget* make_episode_row(EpisodeRow const& episode) {
  auto* lines = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
  auto* title = gtk_label_new(episode.title.c_str());
  gtk_label_set_ellipsize(GTK_LABEL(title), PANGO_ELLIPSIZE_END);
  gtk_label_set_xalign(GTK_LABEL(title), 0.0f);
  gtk_widget_add_css_class(title, "pl-title");
  gtk_box_append(GTK_BOX(lines), title);

  std::string meta = episode.pub;
  if (episode.duration_sec > 0) {
    meta += " · " + format_seconds(episode.duration_sec);
  }
  if (episode.position_sec > 5 && !episode.done) {
    meta += " · at " + format_seconds(episode.position_sec);
  }
  auto* meta_label = gtk_label_new(meta.c_str());
  gtk_label_set_ellipsize(GTK_LABEL(meta_label), PANGO_ELLIPSIZE_END);
  gtk_label_set_xalign(GTK_LABEL(meta_label), 0.0f);
  gtk_widget_add_css_class(meta_label, "pl-meta");
  gtk_box_append(GTK_BOX(lines), meta_label);

  std::vector<GtkWidget*> chips;
  if (episode.downloaded) chips.push_back(make_chip("Downloaded"));
  if (episode.transcript_status == "done") {
    chips.push_back(make_chip("Transcribed"));
  }
  if (episode.transcript_status == "running") {
    chips.push_back(make_chip("Transcribing"));
  }
  if (episode.transcript_status == "error") {
    chips.push_back(make_chip("Transcribe failed"));
  }
  if (episode.summary_status == "done") chips.push_back(make_chip("Summarized"));
  if (episode.has_translation) chips.push_back(make_chip("Translated"));
  if (episode.done) chips.push_back(make_chip("Played"));
  if (!chips.empty()) {
    auto* chip_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    for (auto* chip : chips) {
      gtk_box_append(GTK_BOX(chip_row), chip);
    }
    gtk_box_append(GTK_BOX(lines), chip_row);
  }

  auto* row = gtk_list_box_row_new();
  gtk_widget_add_css_class(lines, "pl-card");
  gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), lines);
  carry_data<std::string>(G_OBJECT(row), "episode-id", episode.id);
  return row;
}

void render_episodes(std::vector<std::vector<std::string>> const& rows) {
  g_state.episodes.clear();
  auto* list = g_state.episode_list;
  g_state.suppress_episode_selection = true;
  while (GtkWidget* child = gtk_widget_get_first_child(GTK_WIDGET(list))) {
    gtk_list_box_remove(list, child);
  }
  g_state.suppress_episode_selection = false;

  for (auto const& row : rows) {
    if (row.size() < 11) continue;
    EpisodeRow episode;
    episode.id = row[0];
    episode.title = row[1];
    episode.pub = row[2];
    episode.duration_sec = std::atof(row[3].c_str());
    episode.downloaded = row[4] == "1";
    episode.transcript_status = row[5];
    episode.summary_status = row[6];
    episode.position_sec = std::atof(row[7].c_str());
    episode.done = row[8] == "1";
    episode.has_translation = row[9] == "1";
    episode.description = row[10];
    g_state.episodes.push_back(episode);
    gtk_list_box_append(list, make_episode_row(episode));
  }

  // resume-on-launch: jump to the recorded episode once it exists
  GtkListBoxRow* select = nullptr;
  if (!g_state.pending_resume_episode_id.empty()) {
    int index = 0;
    for (auto const& episode : g_state.episodes) {
      if (episode.id == g_state.pending_resume_episode_id) {
        select = gtk_list_box_get_row_at_index(list, index);
        set_status(("Resuming: " + episode.title).c_str());
        break;
      }
      ++index;
    }
    g_state.pending_resume_episode_id.clear();
  }
  if (select != nullptr) {
    gtk_list_box_select_row(list, select);
  } else if (!g_state.selected_episode_id.empty()) {
    // Keep the detail pane pointing at a row that still exists; a removed
    // episode clears it.
    bool exists = false;
    for (auto const& episode : g_state.episodes) {
      if (episode.id == g_state.selected_episode_id) {
        exists = true;
        break;
      }
    }
    if (!exists) {
      g_state.selected_episode_id.clear();
      g_state.transcript.clear();
      g_state.summary_json.clear();
      gtk_label_set_text(g_state.detail_title, "");
      gtk_label_set_text(g_state.detail_meta, "");
      gtk_label_set_text(g_state.detail_notes, "");
      gtk_widget_set_visible(GTK_WIDGET(g_state.detail_box), FALSE);
      render_transcript_public();
      render_summary_public();
    }
  }
  update_action_buttons();
}

void reload_episodes() {
  if (g_state.api == nullptr || g_state.selected_feed_id.empty()) return;
  auto const feed_id = g_state.selected_feed_id;
  (void)g_state.api->episode_list_async(
      feed_id,
      [](rivet_app::Result<std::vector<std::vector<std::string>>> result) {
        on_result(std::move(result),
                  [](std::vector<std::vector<std::string>> const& rows) {
                    render_episodes(rows);
                  });
      });
}

void on_episode_row_selected(GtkListBox*, GtkListBoxRow* row, gpointer) {
  if (g_state.suppress_episode_selection) return;
  if (row == nullptr) return;
  auto const* id = carried_data<std::string>(G_OBJECT(row), "episode-id");
  if (id == nullptr) return;
  g_state.selected_episode_id = *id;
  g_state.transcript.clear();
  g_state.summary_json.clear();

  if (auto const* episode = selected_episode()) {
    gtk_label_set_text(g_state.detail_title, episode->title.c_str());
    std::string meta = episode->pub;
    if (episode->duration_sec > 0) {
      meta += " · " + format_seconds(episode->duration_sec);
    }
    if (episode->position_sec > 5 && !episode->done) {
      meta += " · at " + format_seconds(episode->position_sec);
    }
    gtk_label_set_text(g_state.detail_meta, meta.c_str());
    gtk_label_set_text(g_state.detail_notes, episode->description.c_str());
    gtk_widget_set_visible(GTK_WIDGET(g_state.detail_box), TRUE);
  }
  gtk_stack_set_visible_child_name(g_state.reading_stack, "transcript");
  load_transcript();
  load_summary();
  load_estimate();
  update_action_buttons();
}

void on_episode_row_pressed(GtkGestureClick* gesture, int, double x, double y,
                            gpointer) {
  if (gtk_gesture_single_get_current_button(GTK_GESTURE_SINGLE(gesture)) !=
      GDK_BUTTON_SECONDARY) {
    return;
  }
  GtkWidget* row = ancestor_row(gtk_event_controller_get_widget(
      GTK_EVENT_CONTROLLER(gesture)));
  if (row == nullptr) return;
  auto const* id = carried_data<std::string>(G_OBJECT(row), "episode-id");
  if (id == nullptr) return;

  auto* played = g_menu_item_new("Mark played", nullptr);
  g_menu_item_set_action_and_target_value(
      played, "win.episode-done", g_variant_new("(ss)", id->c_str(), "1"));
  auto* unplayed = g_menu_item_new("Mark unplayed", nullptr);
  g_menu_item_set_action_and_target_value(
      unplayed, "win.episode-done", g_variant_new("(ss)", id->c_str(), "0"));
  auto* menu = g_menu_new();
  g_menu_append_item(menu, played);
  g_menu_append_item(menu, unplayed);
  g_object_unref(played);
  g_object_unref(unplayed);
  popup_context_menu(row, G_MENU_MODEL(menu), x, y);
  g_object_unref(menu);
}

EpisodeRow const* selected_episode() {
  for (auto const& episode : g_state.episodes) {
    if (episode.id == g_state.selected_episode_id) return &episode;
  }
  return nullptr;
}

void update_action_buttons() {
  auto const* episode = selected_episode();
  gtk_widget_set_sensitive(GTK_WIDGET(g_state.pipeline_button),
                           episode != nullptr);
}

// ---------------------------------------------------------------------------
// Transcript + summary (read-only panels)
// ---------------------------------------------------------------------------

GtkWidget* make_transcript_segment(std::vector<std::string> const& seg) {
  auto* grid = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
  auto* time_text =
      gtk_label_new(format_seconds(std::atof(seg[0].c_str())).c_str());
  gtk_widget_add_css_class(time_text, "pl-seg-time");
  gtk_widget_set_valign(time_text, GTK_ALIGN_START);
  gtk_widget_set_size_request(time_text, 52, -1);
  gtk_box_append(GTK_BOX(grid), time_text);

  auto* lines = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
  if (!seg[2].empty()) {
    auto* original = GTK_LABEL(gtk_label_new(seg[2].c_str()));
    gtk_label_set_wrap(original, TRUE);
    gtk_label_set_xalign(original, 0.0f);
    gtk_label_set_selectable(original, TRUE);
    gtk_box_append(GTK_BOX(lines), GTK_WIDGET(original));
  }
  if (!seg[3].empty()) {
    auto* translation = GTK_LABEL(gtk_label_new(seg[3].c_str()));
    gtk_label_set_wrap(translation, TRUE);
    gtk_label_set_xalign(translation, 0.0f);
    gtk_label_set_selectable(translation, TRUE);
    gtk_widget_add_css_class(GTK_WIDGET(translation), "pl-meta");
    gtk_box_append(GTK_BOX(lines), GTK_WIDGET(translation));
  }
  gtk_widget_set_hexpand(lines, TRUE);
  gtk_box_append(GTK_BOX(grid), lines);
  gtk_widget_add_css_class(grid, "pl-seg");
  return grid;
}

void render_transcript_public() {
  while (GtkWidget* child =
             gtk_widget_get_first_child(GTK_WIDGET(g_state.transcript_box))) {
    gtk_box_remove(g_state.transcript_box, child);
  }
  if (g_state.transcript.empty()) {
    auto* hint = GTK_LABEL(gtk_label_new(
        g_state.selected_episode_id.empty()
            ? "No episode selected."
            : "No transcript yet — press Understand (needs an API key in "
              "Settings)."));
    gtk_label_set_wrap(hint, TRUE);
    gtk_label_set_xalign(hint, 0.0f);
    gtk_widget_add_css_class(GTK_WIDGET(hint), "pl-meta");
    gtk_box_append(g_state.transcript_box, GTK_WIDGET(hint));
    return;
  }
  for (auto const& seg : g_state.transcript) {
    if (seg.size() < 4) continue;
    if (seg[2].empty() && seg[3].empty()) continue;
    gtk_box_append(g_state.transcript_box, make_transcript_segment(seg));
  }
}

void load_transcript() {
  render_transcript_public();
  if (g_state.api == nullptr || g_state.selected_episode_id.empty()) return;
  auto const episode_id = g_state.selected_episode_id;
  (void)g_state.api->episode_transcript_async(
      episode_id,
      [episode_id](
          rivet_app::Result<std::vector<std::vector<std::string>>> result) {
        on_result(std::move(result),
                  [episode_id](std::vector<
                               std::vector<std::string>> const& rows) {
                    if (g_state.selected_episode_id != episode_id) return;
                    g_state.transcript = rows;
                    render_transcript_public();
                  });
      });
}

GtkWidget* make_section_label(char const* text) {
  auto* label = gtk_label_new(text);
  gtk_label_set_xalign(GTK_LABEL(label), 0.0f);
  gtk_widget_add_css_class(label, "pl-section");
  return label;
}

GtkWidget* make_body_label(std::string const& text) {
  auto* label = gtk_label_new(text.c_str());
  gtk_label_set_wrap(GTK_LABEL(label), TRUE);
  gtk_label_set_xalign(GTK_LABEL(label), 0.0f);
  gtk_label_set_selectable(GTK_LABEL(label), TRUE);
  return label;
}

// Tiny JSON readers for the summary payload — the backend emits flat,
// regular JSON so a scanner beats pulling in a JSON library (same rule as
// the Windows host).
std::string json_string_value(std::string const& json,
                              std::string const& key) {
  auto const key_pos = json.find("\"" + key + "\"");
  if (key_pos == std::string::npos) return {};
  auto const colon = json.find(':', key_pos + key.size() + 2);
  if (colon == std::string::npos) return {};
  auto const open = json.find('"', colon + 1);
  if (open == std::string::npos) return {};
  std::string out;
  for (auto i = open + 1; i < json.size(); ++i) {
    if (json[i] == '\\' && i + 1 < json.size()) {
      char const next = json[i + 1];
      if (next == 'n') out += '\n';
      else if (next == 't') out += '\t';
      else out += next;
      ++i;
      continue;
    }
    if (json[i] == '"') break;
    out += json[i];
  }
  return out;
}

bool json_array_span(std::string const& json, std::string const& key,
                     size_t& begin, size_t& end) {
  auto const key_pos = json.find("\"" + key + "\"");
  if (key_pos == std::string::npos) return false;
  auto const colon = json.find(':', key_pos + key.size() + 2);
  if (colon == std::string::npos) return false;
  auto const open = json.find('[', colon + 1);
  if (open == std::string::npos) return false;
  int depth = 0;
  bool in_string = false;
  for (auto i = open; i < json.size(); ++i) {
    char const c = json[i];
    if (in_string) {
      if (c == '\\') { ++i; continue; }
      if (c == '"') in_string = false;
      continue;
    }
    if (c == '"') in_string = true;
    else if (c == '[') ++depth;
    else if (c == ']') {
      --depth;
      if (depth == 0) {
        begin = open + 1;
        end = i;
        return true;
      }
    }
  }
  return false;
}

std::vector<std::string> json_array_strings(std::string const& json,
                                            std::string const& key) {
  std::vector<std::string> out;
  size_t begin = 0, end = 0;
  if (!json_array_span(json, key, begin, end)) return out;
  bool in_string = false;
  size_t string_start = 0;
  for (auto i = begin; i < end; ++i) {
    char const c = json[i];
    if (in_string) {
      if (c == '\\') { ++i; continue; }
      if (c == '"') {
        in_string = false;
        out.push_back(json.substr(string_start, i - string_start));
      }
      continue;
    }
    if (c == '"') {
      in_string = true;
      string_start = i + 1;
    }
  }
  return out;
}

std::vector<std::pair<std::string, std::string>> json_array_objects(
    std::string const& json, std::string const& key) {
  std::vector<std::pair<std::string, std::string>> out;
  size_t begin = 0, end = 0;
  if (!json_array_span(json, key, begin, end)) return out;
  int depth = 0;
  bool in_string = false;
  size_t object_start = 0;
  for (auto i = begin; i < end; ++i) {
    char const c = json[i];
    if (in_string) {
      if (c == '\\') { ++i; continue; }
      if (c == '"') in_string = false;
      continue;
    }
    if (c == '"') in_string = true;
    else if (c == '{') {
      if (depth == 0) object_start = i;
      ++depth;
    } else if (c == '}') {
      --depth;
      if (depth == 0) {
        std::string const object =
            json.substr(object_start, i - object_start + 1);
        out.emplace_back(json_string_value(object, "text"),
                         json_string_value(object, "translation"));
      }
    }
  }
  return out;
}

void render_summary_public() {
  auto* box = g_state.summary_box;
  while (GtkWidget* child = gtk_widget_get_first_child(GTK_WIDGET(box))) {
    gtk_box_remove(box, child);
  }
  if (g_state.summary_json.empty()) {
    auto* hint = GTK_LABEL(gtk_label_new(
        g_state.selected_episode_id.empty() ? "No episode selected."
                                            : "No summary yet — press Understand."));
    gtk_label_set_wrap(hint, TRUE);
    gtk_label_set_xalign(hint, 0.0f);
    gtk_widget_add_css_class(GTK_WIDGET(hint), "pl-meta");
    gtk_box_append(box, GTK_WIDGET(hint));
    return;
  }
  auto const& json = g_state.summary_json;

  auto const tldr = json_string_value(json, "tldr");
  if (!tldr.empty()) {
    gtk_box_append(box, make_section_label("TL;DR"));
    gtk_box_append(box, make_body_label(tldr));
  }
  auto const points = json_array_strings(json, "key-points");
  if (!points.empty()) {
    gtk_box_append(box, make_section_label("Key points"));
    for (auto const& point : points) {
      gtk_box_append(box, make_body_label("· " + point));
    }
  }
  auto const quotes = json_array_objects(json, "quotes");
  if (!quotes.empty()) {
    gtk_box_append(box, make_section_label("Memorable quotes"));
    for (auto const& [text, translation] : quotes) {
      auto* quote_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
      gtk_widget_set_margin_start(quote_box, 12);
      gtk_box_append(GTK_BOX(quote_box), make_body_label("“" + text + "”"));
      if (!translation.empty()) {
        auto* translated = make_body_label(translation);
        gtk_widget_add_css_class(translated, "pl-meta");
        gtk_box_append(GTK_BOX(quote_box), translated);
      }
      gtk_box_append(box, quote_box);
    }
  }
  auto const topics = json_array_strings(json, "topics");
  if (!topics.empty()) {
    gtk_box_append(box, make_section_label("Topics"));
    auto* chip_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    for (auto const& topic : topics) {
      gtk_box_append(GTK_BOX(chip_row), make_chip(topic.c_str()));
    }
    gtk_box_append(box, chip_row);
  }
}

void load_summary() {
  render_summary_public();
  if (g_state.api == nullptr || g_state.selected_episode_id.empty()) return;
  auto const episode_id = g_state.selected_episode_id;
  (void)g_state.api->episode_summary_async(
      episode_id, [episode_id](rivet_app::Result<std::string> result) {
        on_result(std::move(result), [episode_id](std::string const& json) {
          if (g_state.selected_episode_id != episode_id) return;
          g_state.summary_json = json;
          render_summary_public();
        });
      });
}

// ---------------------------------------------------------------------------
// Understand — estimate → confirm → pipeline (progress via job-progress)
// ---------------------------------------------------------------------------

void set_job_ui(std::string const& message, double fraction, bool running) {
  gtk_widget_set_visible(GTK_WIDGET(g_state.job_bar), running);
  gtk_widget_set_visible(GTK_WIDGET(g_state.job_label), running);
  if (running) {
    gtk_progress_bar_set_fraction(g_state.job_bar,
                                  std::clamp(fraction, 0.0, 1.0));
    gtk_label_set_text(g_state.job_label, message.c_str());
  }
}

void start_pipeline(std::string const& episode_id) {
  if (g_state.api == nullptr || episode_id.empty()) return;
  set_status("Job started — the list refreshes when it finishes");
  (void)g_state.api->episode_pipeline_async(
      episode_id, [episode_id](rivet_app::Result<std::string> result) {
        on_result(std::move(result), [episode_id](std::string const& job_id) {
          g_state.active_job_id = job_id;
          g_state.active_job_episode = episode_id;
          set_job_ui("pipeline 0%", 0.0, true);
        });
      });
}

void confirm_pipeline(std::string const& episode_id,
                      std::string const& json) {
  std::string body =
      "Download → transcribe → translate → summarize with your API key.";
  auto const estimate = estimate_line(json);
  if (!estimate.empty()) {
    body += "\n\n" + estimate;
  }
  auto* dialog = gtk_dialog_new_with_buttons(
      "Understand", g_state.window,
      static_cast<GtkDialogFlags>(GTK_DIALOG_MODAL |
                                  GTK_DIALOG_DESTROY_WITH_PARENT),
      "Cancel", GTK_RESPONSE_CANCEL, "Start", GTK_RESPONSE_ACCEPT, nullptr);
  auto* area = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
  auto* text = GTK_LABEL(gtk_label_new(body.c_str()));
  gtk_label_set_wrap(text, TRUE);
  gtk_label_set_max_width_chars(text, 56);
  gtk_widget_set_margin_top(GTK_WIDGET(text), 12);
  gtk_widget_set_margin_bottom(GTK_WIDGET(text), 6);
  gtk_widget_set_margin_start(GTK_WIDGET(text), 14);
  gtk_widget_set_margin_end(GTK_WIDGET(text), 14);
  gtk_box_append(GTK_BOX(area), GTK_WIDGET(text));
  carry_data<std::string>(G_OBJECT(dialog), "episode-id", episode_id);
  g_signal_connect(
      dialog, "response",
      G_CALLBACK(+[](GtkDialog* dialog, gint response, gpointer) {
        auto const* id =
            carried_data<std::string>(G_OBJECT(dialog), "episode-id");
        std::string const episode_id = id != nullptr ? *id : std::string();
        gtk_window_destroy(GTK_WINDOW(dialog));
        if (response == GTK_RESPONSE_ACCEPT) {
          start_pipeline(episode_id);
        }
      }),
      nullptr);
  gtk_window_present(GTK_WINDOW(dialog));
}

void on_pipeline_clicked(GtkButton*, gpointer) {
  auto const* episode = selected_episode();
  if (episode == nullptr || g_state.api == nullptr) return;
  auto const episode_id = episode->id;
  // Cost confirmation first (the BYOK bill), then the pipeline run. A failed
  // estimate must not block the run — confirm without numbers instead.
  (void)g_state.api->episode_estimate_async(
      episode_id, [episode_id](rivet_app::Result<std::string> result) {
        std::string json;
        if (result.succeeded()) {
          try {
            json = result.get();
          } catch (...) {
          }
        }
        dispatch_ui([episode_id, json] {
          confirm_pipeline(episode_id, json);
        });
      });
}

void load_estimate() {
  gtk_label_set_text(g_state.estimate_label, "");
  if (g_state.api == nullptr || g_state.selected_episode_id.empty()) return;
  auto const episode_id = g_state.selected_episode_id;
  (void)g_state.api->episode_estimate_async(
      episode_id, [episode_id](rivet_app::Result<std::string> result) {
        on_result(std::move(result), [episode_id](std::string const& json) {
          if (g_state.selected_episode_id != episode_id) return;
          gtk_label_set_text(g_state.estimate_label,
                             estimate_line(json).c_str());
        });
      });
}

// ---------------------------------------------------------------------------
// Actions — add feed / refresh all / remove feed / mark done / about
// ---------------------------------------------------------------------------

void add_feed_with_url(std::string const& url) {
  if (g_state.api == nullptr || url.empty()) return;
  (void)g_state.api->feed_add_async(
      url, [](rivet_app::Result<std::string> result) {
        on_result(std::move(result), [](std::string const&) {
          set_status("Subscription added");
          reload_feeds();
        });
      });
}

void on_add_feed_activated(GSimpleAction*, GVariant*, gpointer) {
  if (g_state.api == nullptr) return;
  auto* dialog = gtk_dialog_new_with_buttons(
      "Add Feed", g_state.window,
      static_cast<GtkDialogFlags>(GTK_DIALOG_MODAL |
                                  GTK_DIALOG_DESTROY_WITH_PARENT),
      "Cancel", GTK_RESPONSE_CANCEL, "Add", GTK_RESPONSE_ACCEPT, nullptr);
  auto* entry = gtk_entry_new();
  gtk_entry_set_placeholder_text(GTK_ENTRY(entry),
                                 "https://example.com/feed.xml");
  gtk_widget_set_size_request(entry, 420, -1);
  gtk_widget_set_margin_top(entry, 12);
  gtk_widget_set_margin_bottom(entry, 6);
  gtk_widget_set_margin_start(entry, 14);
  gtk_widget_set_margin_end(entry, 14);
  auto* area = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
  gtk_box_append(GTK_BOX(area), entry);
  // Enter in the field carries the URL and closes the dialog in one step.
  g_signal_connect(
      entry, "activate",
      G_CALLBACK(+[](GtkEntry* entry, gpointer data) {
        auto* dialog = GTK_DIALOG(data);
        carry_data<std::string>(
            G_OBJECT(dialog), "feed-url",
            gtk_editable_get_text(GTK_EDITABLE(entry)));
        gtk_dialog_response(dialog, GTK_RESPONSE_ACCEPT);
      }),
      dialog);
  g_signal_connect(
      dialog, "response",
      G_CALLBACK(+[](GtkDialog* dialog, gint response, gpointer) {
        std::string url;
        if (auto const* carried =
                carried_data<std::string>(G_OBJECT(dialog), "feed-url")) {
          url = *carried;
        } else if (response == GTK_RESPONSE_ACCEPT) {
          auto* content = gtk_dialog_get_content_area(dialog);
          if (auto* entry =
                  GTK_ENTRY(gtk_widget_get_first_child(content))) {
            url = gtk_editable_get_text(GTK_EDITABLE(entry));
          }
        }
        gtk_window_destroy(GTK_WINDOW(dialog));
        if (response == GTK_RESPONSE_ACCEPT) {
          add_feed_with_url(url);
        }
      }),
      nullptr);
  gtk_window_present(GTK_WINDOW(dialog));
  gtk_widget_grab_focus(entry);
}

void on_refresh_all_activated(GSimpleAction*, GVariant*, gpointer) {
  if (g_state.api == nullptr) return;
  set_status("Refreshing all feeds…");
  (void)g_state.api->feed_refresh_all_async(
      [](rivet_app::Result<std::int64_t> result) {
        on_result(std::move(result), [](std::int64_t added) {
          if (added > 0) {
            set_status(("Refresh complete, " + std::to_string(added) +
                        " new episodes")
                           .c_str());
          } else {
            set_status("Refresh complete");
          }
          reload_feeds();
          reload_episodes();
        });
      });
}

void on_feed_remove_activated(GSimpleAction*, GVariant* parameter,
                              gpointer) {
  if (g_state.api == nullptr || parameter == nullptr) return;
  std::string const feed_id(g_variant_get_string(parameter, nullptr));
  auto* dialog = gtk_dialog_new_with_buttons(
      "Remove Subscription", g_state.window,
      static_cast<GtkDialogFlags>(GTK_DIALOG_MODAL |
                                  GTK_DIALOG_DESTROY_WITH_PARENT),
      "Cancel", GTK_RESPONSE_CANCEL, "Remove", GTK_RESPONSE_ACCEPT, nullptr);
  auto* text = GTK_LABEL(
      gtk_label_new("Remove this subscription and all its episodes?"));
  gtk_label_set_wrap(text, TRUE);
  gtk_widget_set_margin_top(GTK_WIDGET(text), 12);
  gtk_widget_set_margin_bottom(GTK_WIDGET(text), 6);
  gtk_widget_set_margin_start(GTK_WIDGET(text), 14);
  gtk_widget_set_margin_end(GTK_WIDGET(text), 14);
  gtk_box_append(GTK_BOX(gtk_dialog_get_content_area(GTK_DIALOG(dialog))),
                 GTK_WIDGET(text));
  carry_data<std::string>(G_OBJECT(dialog), "feed-id", feed_id);
  g_signal_connect(
      dialog, "response",
      G_CALLBACK(+[](GtkDialog* dialog, gint response, gpointer) {
        auto const* id =
            carried_data<std::string>(G_OBJECT(dialog), "feed-id");
        std::string const feed_id = id != nullptr ? *id : std::string();
        gtk_window_destroy(GTK_WINDOW(dialog));
        if (response != GTK_RESPONSE_ACCEPT || feed_id.empty()) return;
        (void)g_state.api->feed_remove_async(
            feed_id, [](rivet_app::Result<bool> result) {
              on_result(std::move(result), [](bool) {
                set_status("Subscription removed");
                reload_feeds();
              });
            });
      }),
      nullptr);
  gtk_window_present(GTK_WINDOW(dialog));
}

void on_episode_done_activated(GSimpleAction*, GVariant* parameter,
                               gpointer) {
  if (g_state.api == nullptr || parameter == nullptr) return;
  gsize length = 0;
  auto** strings = g_variant_get_strv(parameter, &length);
  if (strings == nullptr || length < 2) {
    g_strfreev(const_cast<gchar**>(strings));
    return;
  }
  std::string const episode_id = strings[0];
  std::string const done = strings[1];
  g_strfreev(const_cast<gchar**>(strings));
  (void)g_state.api->episode_set_done_async(
      episode_id, done, [](rivet_app::Result<bool>) {
        dispatch_ui([] {
          reload_feeds();
          reload_episodes();
        });
      });
}

void on_about_activated(GSimpleAction*, GVariant*, gpointer) {
  auto* dialog = gtk_about_dialog_new();
  gtk_about_dialog_set_program_name(GTK_ABOUT_DIALOG(dialog), "PodLens");
  gtk_about_dialog_set_version(GTK_ABOUT_DIALOG(dialog),
                               rivet_app::kVersion);
  gtk_about_dialog_set_comments(
      GTK_ABOUT_DIALOG(dialog),
      "Podcasts, in your language. Transcribes, translates and summarizes "
      "English episodes with your own LLM API key.");
  gtk_about_dialog_set_license_type(GTK_ABOUT_DIALOG(dialog),
                                    GTK_LICENSE_AGPL_3_0);
  gtk_about_dialog_set_website(GTK_ABOUT_DIALOG(dialog),
                               "https://podlens.jrtx.site");
  gtk_about_dialog_set_website_label(GTK_ABOUT_DIALOG(dialog), "Website");
  static char const* kIssueLinks[] = {
      "github.com/turinglambdaai/podlens/issues", nullptr};
  gtk_about_dialog_add_credit_section(
      GTK_ABOUT_DIALOG(dialog), "Report an Issue", kIssueLinks);
  gtk_window_set_transient_for(GTK_WINDOW(dialog), g_state.window);
  gtk_window_set_modal(GTK_WINDOW(dialog), TRUE);
  gtk_window_present(GTK_WINDOW(dialog));
}

// ---------------------------------------------------------------------------
// Actions + menu plumbing
// ---------------------------------------------------------------------------

void add_app_action(GtkApplication* app, char const* name,
                    void (*handler)(GSimpleAction*, GVariant*, gpointer)) {
  auto* action = g_simple_action_new(name, nullptr);
  g_signal_connect(action, "activate", G_CALLBACK(handler), nullptr);
  g_action_map_add_action(G_ACTION_MAP(app), G_ACTION(action));
  g_object_unref(action);
}

void add_window_action(GtkApplicationWindow* window, char const* name,
                       GVariantType const* parameter_type,
                       void (*handler)(GSimpleAction*, GVariant*, gpointer)) {
  auto* action = g_simple_action_new(name, parameter_type);
  g_signal_connect(action, "activate", G_CALLBACK(handler), nullptr);
  g_action_map_add_action(G_ACTION_MAP(window), G_ACTION(action));
  g_object_unref(action);
}

GtkWidget* build_primary_menu() {
  auto* menu = g_menu_new();
  g_menu_append(menu, "Add Feed…", "app.add-feed");
  g_menu_append(menu, "Refresh All Feeds", "app.refresh-all");
  auto* help = g_menu_new();
  g_menu_append(help, "About", "app.about");
  g_menu_append_submenu(menu, "Help", G_MENU_MODEL(help));
  g_object_unref(help);

  auto* menubutton = gtk_menu_button_new();
  gtk_menu_button_set_icon_name(GTK_MENU_BUTTON(menubutton),
                                "open-menu-symbolic");
  gtk_menu_button_set_menu_model(GTK_MENU_BUTTON(menubutton),
                                 G_MENU_MODEL(menu));
  gtk_menu_button_set_primary(GTK_MENU_BUTTON(menubutton), TRUE);
  g_object_unref(menu);
  return menubutton;
}

// ---------------------------------------------------------------------------
// Events
// ---------------------------------------------------------------------------

void subscribe_to_events() {
  g_state.backend->set_event_handler(
      [](std::string const& name, rivet::Value const& value) {
        try {
          auto const event = rivet_app::decode_event(name, value);
          if (std::holds_alternative<rivet_app::Episodes_changedEvent>(
                  event)) {
            dispatch_ui([] {
              reload_feeds();
              reload_episodes();
            });
          } else if (auto const* progress =
                         std::get_if<rivet_app::Job_progressEvent>(&event)) {
            // Row = (job-id kind pct message).
            std::string const job_id =
                progress->value.size() > 0 ? progress->value[0] : "";
            std::string const kind =
                progress->value.size() > 1 ? progress->value[1] : "";
            int pct = 0;
            if (progress->value.size() > 2) {
              pct = std::atoi(progress->value[2].c_str());
            }
            std::string const message =
                progress->value.size() > 3 ? progress->value[3] : "";
            dispatch_ui([job_id, kind, pct, message] {
              if (g_state.active_job_id.empty() ||
                  g_state.active_job_id != job_id) {
                return;  // some other episode's job
              }
              if (pct >= 100) {
                std::string const episode = g_state.active_job_episode;
                g_state.active_job_id.clear();
                g_state.active_job_episode.clear();
                set_job_ui("", 0.0, false);
                // A non-empty terminal message is the job's error text.
                if (!message.empty()) {
                  set_status(message.c_str());
                }
                reload_feeds();
                reload_episodes();
                if (!episode.empty() &&
                    g_state.selected_episode_id == episode) {
                  load_transcript();
                  load_summary();
                }
                return;
              }
              std::string label = kind;
              if (!message.empty()) label += " — " + message;
              set_job_ui(label + " · " + std::to_string(pct) + "%",
                         pct / 100.0, true);
            });
          } else if (auto const* notify =
                         std::get_if<rivet_app::NotifyEvent>(&event)) {
            std::string const text = notify->value;
            dispatch_ui([text] { set_status(text.c_str()); });
          } else if (auto const* open =
                         std::get_if<rivet_app::Open_urlEvent>(&event)) {
            std::string const url = open->value;
            dispatch_ui([url] {
              gtk_show_uri(g_state.window, url.c_str(), GDK_CURRENT_TIME);
            });
          }
          // update-available travels on the wire for compat; the update flow
          // surfaces through the update-check dialog (M-complete).
        } catch (std::exception const&) {
          // unknown event names are ignored — forward compatibility
        }
      });
}

// ---------------------------------------------------------------------------
// Startup (same contract as the Rivet template)
// ---------------------------------------------------------------------------

struct RuntimeLayout {
  std::filesystem::path petite_boot;
  std::filesystem::path scheme_boot;
  std::filesystem::path racket_boot;
  std::filesystem::path core;
};

std::optional<RuntimeLayout> discover_runtime_layout() {
  std::filesystem::path const exe = executable_path();
  std::filesystem::path const roots[] = {exe.parent_path()};
  for (auto const& root : roots) {
    RuntimeLayout layout{
        root / "runtime" / "petite.boot",
        root / "runtime" / "scheme.boot",
        root / "runtime" / "racket.boot",
        root / "res" / "core.zo",
    };
    if (std::filesystem::exists(layout.petite_boot) &&
        std::filesystem::exists(layout.scheme_boot) &&
        std::filesystem::exists(layout.racket_boot) &&
        std::filesystem::exists(layout.core)) {
      return layout;
    }
  }
  return std::nullopt;
}

void on_shutdown(GApplication*, gpointer);
void perform_orderly_shutdown();

int on_backend_finished(gpointer) {
  if (g_state.startup_thread.joinable()) {
    g_state.startup_thread.join();
  }

  std::unique_ptr<rivet::linux_runtime::Backend> backend;
  std::string error;
  {
    std::lock_guard lock(g_state.startup_mutex);
    backend = std::move(g_state.startup_backend);
    error = std::move(g_state.startup_error);
  }

  if (g_state.shutting_down.load(std::memory_order_acquire)) {
    if (backend != nullptr) {
      backend->stop();
    }
    return G_SOURCE_REMOVE;
  }

  if (!error.empty()) {
    set_status_error("Backend error: " + error);
    return G_SOURCE_REMOVE;
  }
  if (backend == nullptr) {
    set_status_error("Backend error: startup completed without a backend");
    return G_SOURCE_REMOVE;
  }

  g_state.backend = std::move(backend);
  g_state.api = std::make_unique<rivet_app::API>(*g_state.backend);
  subscribe_to_events();

  // The shutdown hook goes here, not in main(): the embedded Racket runtime
  // installs its own SIGTERM handler while booting and would leave ours
  // inert (taskly issue #3). Booting is done at this point, so installing
  // now is the last word. Still on the main thread, as the hook requires.
  try {
    rivet::system::InstallShutdownHook(perform_orderly_shutdown);
  } catch (std::exception const& hook_error) {
    std::fputs((std::string{"shutdown hook unavailable: "} +
                hook_error.what() + "\n")
                   .c_str(),
               stderr);
  }

  set_status("Ready");
  reload_feeds();
  (void)g_state.api->resume_last_async(
      [](rivet_app::Result<std::vector<std::string>> result) {
        on_result(std::move(result),
                  [](std::vector<std::string> const& row) {
                    if (row.size() < 3) return;
                    g_state.pending_resume_feed_id = row[0];
                    g_state.pending_resume_episode_id = row[1];
                    reload_feeds();
                  });
      });
  return G_SOURCE_REMOVE;
}

void start_backend() {
  auto layout = discover_runtime_layout();
  if (!layout.has_value()) {
    set_status(
        "Missing Rivet runtime layout (runtime/*.boot, res/core.zo) next to "
        "the executable. Build with raco rivet build/dev.");
    return;
  }

  rivet::linux_runtime::RacketRuntimeConfig config;
  config.executable_path = executable_path().string();
  config.petite_boot = layout->petite_boot.string();
  config.scheme_boot = layout->scheme_boot.string();
  config.racket_boot = layout->racket_boot.string();
  config.backend_bundle = layout->core.string();
  config.module_name = rivet_app::kModuleName;
  config.entry_symbol = rivet_app::kEntryName;

  // Booting the embedded runtime blocks on file I/O; only startup runs off
  // the main loop. Everything after completion dispatches back through
  // g_idle_add.
  g_state.startup_thread = std::thread([config = std::move(config)]() mutable {
    auto backend =
        std::make_unique<rivet::linux_runtime::Backend>(std::move(config));
    try {
      backend->start();
      {
        std::lock_guard lock(g_state.startup_mutex);
        g_state.startup_backend = std::move(backend);
      }
    } catch (std::exception const& e) {
      std::lock_guard lock(g_state.startup_mutex);
      g_state.startup_error = e.what();
    }
    g_idle_add(on_backend_finished, nullptr);
  });
}

// ---------------------------------------------------------------------------
// Window
// ---------------------------------------------------------------------------

GtkWidget* make_button_with_icon(char const* icon, char const* label) {
  auto* box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
  gtk_box_append(GTK_BOX(box), gtk_image_new_from_icon_name(icon));
  gtk_box_append(GTK_BOX(box), gtk_label_new(label));
  auto* button = gtk_button_new();
  gtk_button_set_child(GTK_BUTTON(button), box);
  return button;
}

void rebuild_episode_list() {
  g_state.suppress_episode_selection = true;
  while (GtkWidget* child =
             gtk_widget_get_first_child(GTK_WIDGET(g_state.episode_list))) {
    gtk_list_box_remove(g_state.episode_list, child);
  }
  g_state.suppress_episode_selection = false;
}

void on_shutdown(GApplication*, gpointer) {
  // Exactly-once teardown: the GTK shutdown signal and the signal hook can
  // both reach this; whichever lands first does the work.
  if (g_state.teardown_started.exchange(true, std::memory_order_acq_rel)) {
    return;
  }
  g_state.shutting_down.store(true, std::memory_order_release);
  if (g_state.startup_thread.joinable()) {
    g_state.startup_thread.join();
  }

  std::unique_ptr<rivet::linux_runtime::Backend> startup_backend;
  {
    std::lock_guard lock(g_state.startup_mutex);
    startup_backend = std::move(g_state.startup_backend);
  }
  if (startup_backend != nullptr) {
    startup_backend->stop();
  }
  if (g_state.backend != nullptr) {
    g_state.backend->stop();
  }
}

// SIGTERM/SIGINT bypass the GTK main loop, so the shutdown signal alone
// leaves the window alive over a dead backend. The hook's watcher thread
// runs this same teardown and then exits the process.
void perform_orderly_shutdown() { on_shutdown(nullptr, nullptr); }

void on_activate(GtkApplication* app, gpointer) {
  if (g_state.window != nullptr) {
    gtk_window_present(g_state.window);
    return;
  }

  apply_css();

  auto* window = gtk_application_window_new(app);
  gtk_window_set_title(GTK_WINDOW(window), "PodLens");
  gtk_window_set_default_size(GTK_WINDOW(window), 1160, 760);
  gtk_window_set_default_icon_name("podlens");

  // Header bar: Add Feed + Refresh All on the left, the primary menu on the
  // right.
  auto* header = gtk_header_bar_new();
  auto* add_feed = make_button_with_icon("list-add-symbolic", "Add Feed");
  gtk_actionable_set_action_name(GTK_ACTIONABLE(add_feed), "app.add-feed");
  gtk_header_bar_pack_start(GTK_HEADER_BAR(header), add_feed);
  auto* refresh_all =
      gtk_button_new_from_icon_name("view-refresh-symbolic");
  gtk_actionable_set_action_name(GTK_ACTIONABLE(refresh_all),
                                 "app.refresh-all");
  gtk_widget_set_tooltip_text(refresh_all, "Refresh All Feeds");
  gtk_header_bar_pack_start(GTK_HEADER_BAR(header), refresh_all);
  auto* menubutton = build_primary_menu();
  gtk_header_bar_pack_end(GTK_HEADER_BAR(header), menubutton);
  gtk_header_bar_set_show_title_buttons(GTK_HEADER_BAR(header), TRUE);
  gtk_window_set_titlebar(GTK_WINDOW(window), header);

  // Body: subscriptions sidebar | episodes + detail.
  auto* body = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
  gtk_widget_set_vexpand(body, TRUE);

  auto* sidebar = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_set_size_request(sidebar, 240, -1);
  auto* caption = gtk_label_new("Subscriptions");
  gtk_label_set_xalign(GTK_LABEL(caption), 0.0f);
  gtk_widget_add_css_class(caption, "pl-caption");
  gtk_box_append(GTK_BOX(sidebar), caption);
  auto* feed_scroll = gtk_scrolled_window_new();
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(feed_scroll),
                                 GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_widget_set_vexpand(feed_scroll, TRUE);
  auto* feed_list = gtk_list_box_new();
  gtk_list_box_set_selection_mode(GTK_LIST_BOX(feed_list),
                                  GTK_SELECTION_SINGLE);
  gtk_widget_add_css_class(feed_list, "navigation-sidebar");
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(feed_scroll), feed_list);
  gtk_box_append(GTK_BOX(sidebar), feed_scroll);
  gtk_paned_set_start_child(GTK_PANED(body), sidebar);
  gtk_paned_set_position(GTK_PANED(body), 260);

  auto* pane = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_set_hexpand(pane, TRUE);

  auto* episode_caption = gtk_label_new("Episodes");
  gtk_label_set_xalign(GTK_LABEL(episode_caption), 0.0f);
  gtk_widget_add_css_class(episode_caption, "pl-caption");
  gtk_box_append(GTK_BOX(pane), episode_caption);

  auto* episode_scroll = gtk_scrolled_window_new();
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(episode_scroll),
                                 GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  auto* episode_list = gtk_list_box_new();
  gtk_list_box_set_selection_mode(GTK_LIST_BOX(episode_list),
                                  GTK_SELECTION_SINGLE);
  gtk_widget_set_valign(GTK_WIDGET(episode_list), GTK_ALIGN_START);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(episode_scroll),
                                episode_list);

  auto* empty_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
  gtk_widget_set_valign(empty_box, GTK_ALIGN_CENTER);
  gtk_widget_set_halign(empty_box, GTK_ALIGN_CENTER);
  auto* empty_icon = gtk_label_new("📻");
  gtk_widget_add_css_class(empty_icon, "pl-empty");
  gtk_box_append(GTK_BOX(empty_box), empty_icon);
  auto* empty_text = gtk_label_new("");
  gtk_widget_add_css_class(empty_text, "pl-meta");
  gtk_box_append(GTK_BOX(empty_box), empty_text);

  auto* list_stack = gtk_stack_new();
  gtk_stack_add_named(GTK_STACK(list_stack), episode_scroll, "list");
  gtk_stack_add_named(GTK_STACK(list_stack), empty_box, "empty");
  gtk_stack_set_visible_child_name(GTK_STACK(list_stack), "empty");

  // Episodes on top, detail below; the divider starts half/half.
  auto* vertical = gtk_paned_new(GTK_ORIENTATION_VERTICAL);
  gtk_paned_set_start_child(GTK_PANED(vertical), list_stack);
  gtk_paned_set_position(GTK_PANED(vertical), 320);

  // Detail pane: header, action row (Understand + progress), reading stack.
  auto* detail = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
  gtk_widget_set_margin_top(detail, 6);
  gtk_widget_set_margin_start(detail, 14);
  gtk_widget_set_margin_end(detail, 14);
  gtk_widget_set_margin_bottom(detail, 6);
  gtk_widget_set_visible(detail, FALSE);

  auto* detail_title = GTK_LABEL(gtk_label_new(""));
  gtk_label_set_xalign(detail_title, 0.0f);
  gtk_label_set_wrap(detail_title, TRUE);
  gtk_widget_add_css_class(GTK_WIDGET(detail_title), "title-2");
  gtk_box_append(GTK_BOX(detail), GTK_WIDGET(detail_title));
  auto* detail_meta = gtk_label_new("");
  gtk_label_set_xalign(GTK_LABEL(detail_meta), 0.0f);
  gtk_widget_add_css_class(detail_meta, "pl-meta");
  gtk_box_append(GTK_BOX(detail), detail_meta);
  auto* detail_notes = GTK_LABEL(gtk_label_new(""));
  gtk_label_set_xalign(detail_notes, 0.0f);
  gtk_label_set_wrap(detail_notes, TRUE);
  gtk_label_set_max_width_chars(detail_notes, 100);
  gtk_label_set_ellipsize(detail_notes, PANGO_ELLIPSIZE_END);
  gtk_label_set_lines(detail_notes, 2);
  gtk_widget_add_css_class(GTK_WIDGET(detail_notes), "pl-meta");
  gtk_box_append(GTK_BOX(detail), GTK_WIDGET(detail_notes));

  auto* actions = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  auto* pipeline = make_button_with_icon("starred-symbolic", "Understand");
  gtk_widget_add_css_class(pipeline, "suggested-action");
  gtk_widget_set_sensitive(pipeline, FALSE);
  gtk_box_append(GTK_BOX(actions), pipeline);
  auto* job_bar = gtk_progress_bar_new();
  gtk_widget_set_hexpand(job_bar, TRUE);
  gtk_widget_set_valign(job_bar, GTK_ALIGN_CENTER);
  gtk_widget_set_visible(job_bar, FALSE);
  gtk_box_append(GTK_BOX(actions), job_bar);
  auto* job_label = gtk_label_new("");
  gtk_widget_add_css_class(job_label, "pl-meta");
  gtk_widget_set_visible(job_label, FALSE);
  gtk_box_append(GTK_BOX(actions), job_label);
  gtk_box_append(GTK_BOX(detail), actions);
  auto* estimate_label = GTK_LABEL(gtk_label_new(""));
  gtk_label_set_xalign(estimate_label, 0.0f);
  gtk_widget_add_css_class(GTK_WIDGET(estimate_label), "pl-meta");
  gtk_label_set_ellipsize(estimate_label, PANGO_ELLIPSIZE_END);
  gtk_box_append(GTK_BOX(detail), GTK_WIDGET(estimate_label));

  // Reading stack: transcript | summary.
  auto* reading = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  auto* reading_stack = gtk_stack_new();
  auto* switcher = gtk_stack_switcher_new();
  gtk_stack_switcher_set_stack(GTK_STACK_SWITCHER(switcher),
                               GTK_STACK(reading_stack));
  {
    auto* switcher_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_box_append(GTK_BOX(switcher_row), switcher);
    gtk_box_append(GTK_BOX(reading), switcher_row);
  }

  auto* transcript_scroll = gtk_scrolled_window_new();
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(transcript_scroll),
                                 GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  auto* transcript_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
  gtk_widget_set_margin_top(transcript_box, 6);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(transcript_scroll),
                                transcript_box);
  gtk_stack_add_named(GTK_STACK(reading_stack), transcript_scroll,
                      "transcript");
  gtk_stack_set_visible_child_name(GTK_STACK(reading_stack), "transcript");

  auto* summary_scroll = gtk_scrolled_window_new();
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(summary_scroll),
                                 GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  auto* summary_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
  gtk_widget_set_margin_top(summary_box, 6);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(summary_scroll),
                                summary_box);
  gtk_stack_add_named(GTK_STACK(reading_stack), summary_scroll, "summary");

  gtk_box_append(GTK_BOX(reading), reading_stack);
  gtk_widget_set_vexpand(reading, TRUE);
  gtk_box_append(GTK_BOX(detail), reading);
  gtk_paned_set_end_child(GTK_PANED(vertical), detail);

  gtk_box_append(GTK_BOX(pane), vertical);
  gtk_paned_set_end_child(GTK_PANED(body), pane);

  // Status bar at the bottom carries progress chatter quietly.
  auto* statusbar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_add_css_class(statusbar, "pl-statusbar");
  auto* status_text = gtk_label_new("Starting embedded Racket CS…");
  gtk_label_set_ellipsize(GTK_LABEL(status_text), PANGO_ELLIPSIZE_END);
  gtk_label_set_xalign(GTK_LABEL(status_text), 0.0f);
  gtk_widget_add_css_class(status_text, "pl-meta");
  gtk_box_append(GTK_BOX(statusbar), status_text);

  auto* root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_box_append(GTK_BOX(root), body);
  gtk_box_append(GTK_BOX(root), statusbar);
  gtk_window_set_child(GTK_WINDOW(window), root);

  g_state.window = GTK_WINDOW(window);
  g_state.pipeline_button = GTK_BUTTON(pipeline);
  g_state.feeds_caption = GTK_LABEL(caption);
  g_state.feed_list = GTK_LIST_BOX(feed_list);
  g_state.episodes_caption = GTK_LABEL(episode_caption);
  g_state.episode_list = GTK_LIST_BOX(episode_list);
  g_state.list_stack = GTK_STACK(list_stack);
  g_state.empty_icon = GTK_LABEL(empty_icon);
  g_state.empty_text = GTK_LABEL(empty_text);
  g_state.detail_box = GTK_BOX(detail);
  g_state.detail_title = GTK_LABEL(detail_title);
  g_state.detail_meta = GTK_LABEL(detail_meta);
  g_state.detail_notes = GTK_LABEL(detail_notes);
  g_state.job_bar = GTK_PROGRESS_BAR(job_bar);
  g_state.job_label = GTK_LABEL(job_label);
  g_state.estimate_label = GTK_LABEL(estimate_label);
  g_state.reading_stack = GTK_STACK(reading_stack);
  g_state.transcript_box = GTK_BOX(transcript_box);
  g_state.summary_box = GTK_BOX(summary_box);
  g_state.status_text = GTK_LABEL(status_text);

  add_app_action(app, "add-feed", on_add_feed_activated);
  add_app_action(app, "refresh-all", on_refresh_all_activated);
  add_app_action(app, "about", on_about_activated);
  add_window_action(GTK_APPLICATION_WINDOW(window), "feed-remove",
                    G_VARIANT_TYPE_STRING, on_feed_remove_activated);
  add_window_action(GTK_APPLICATION_WINDOW(window), "episode-done",
                    G_VARIANT_TYPE("(ss)"), on_episode_done_activated);

  g_signal_connect(feed_list, "row-selected",
                   G_CALLBACK(on_feed_row_selected), nullptr);
  g_signal_connect(episode_list, "row-selected",
                   G_CALLBACK(on_episode_row_selected), nullptr);
  {
    auto* feed_gesture = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(feed_gesture),
                                  GDK_BUTTON_SECONDARY);
    g_signal_connect(feed_gesture, "pressed",
                     G_CALLBACK(on_feed_row_pressed), nullptr);
    gtk_widget_add_controller(GTK_WIDGET(feed_list),
                              GTK_EVENT_CONTROLLER(feed_gesture));
    auto* episode_gesture = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(episode_gesture),
                                  GDK_BUTTON_SECONDARY);
    g_signal_connect(episode_gesture, "pressed",
                     G_CALLBACK(on_episode_row_pressed), nullptr);
    gtk_widget_add_controller(GTK_WIDGET(episode_list),
                              GTK_EVENT_CONTROLLER(episode_gesture));
  }
  g_signal_connect(pipeline, "clicked", G_CALLBACK(on_pipeline_clicked),
                   nullptr);

  gtk_window_present(GTK_WINDOW(window));
  start_backend();
}

}  // namespace

int main(int argc, char** argv) {
  auto* app =
      gtk_application_new("site.jrtx.podlens", G_APPLICATION_DEFAULT_FLAGS);
  g_signal_connect(app, "activate", G_CALLBACK(on_activate), nullptr);
  g_signal_connect(app, "shutdown", G_CALLBACK(on_shutdown), nullptr);
  int const status = g_application_run(G_APPLICATION(app), argc, argv);
  g_object_unref(app);
  return status;
}
