// PodLens Linux host — GTK4 window over one embedded Racket CS backend.
//
// Interaction contract mirrors the Windows host (windows/MainWindow.xaml.cpp)
// element for element: subscriptions in the sidebar, episode cards with
// status chips, one-click Understand with a cost estimate, a
// follow-along bilingual transcript, sleep timer, MPRIS media keys,
// continuous playback, and resume-on-launch. UI strings run through the
// zh/en table (tr(), zh default, PODLENS_LANG=en to opt out) — the same
// split as the Windows and macOS hosts, which both default to Chinese.
//
// Threading: boot the runtime off the UI thread; every completion/event is
// dispatched back to the main loop (g_idle_add) before touching widgets.
#include <gtk/gtk.h>
#include <gst/gst.h>

#include <algorithm>
#include <atomic>
#include <ctime>
#include <set>
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
#include <ctime>
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

// The backend caches audio under <data-dir>/audio/<episodeId>.<ext>; the
// host resolves the file by id prefix so playback needs no extra RPC. The
// PODLENS_DATA_DIR override must match the backend's (dev/test only).
std::string find_local_audio(std::string const& episode_id) {
  std::filesystem::path base;
  if (char const* override_dir = getenv("PODLENS_DATA_DIR");
      override_dir != nullptr && override_dir[0] != '\0') {
    base = override_dir;
  } else if (char const* home = getenv("HOME"); home != nullptr) {
    base = std::filesystem::path(home) / ".podlens";
  }
  if (base.empty()) return {};
  std::filesystem::path dir = base / "audio";
  std::error_code ec;
  if (!std::filesystem::exists(dir, ec)) return {};
  for (auto const& entry : std::filesystem::directory_iterator(dir, ec)) {
    auto const name = entry.path().filename().string();
    if (name.rfind(episode_id, 0) == 0) {
      return entry.path().string();
    }
  }
  return {};
}

// Install shape for the update flow (docs/UPDATE.md): the AppImage runtime
// exports APPIMAGE pointing at the running image; deb lands under
// /opt/podlens and rpm under /usr — those upgrade through the package
// manager; everything else is the portable tarball layout.
std::string install_shape() {
  char const* appimage = getenv("APPIMAGE");
  if (appimage != nullptr && appimage[0] != '\0') return "appimage";
  auto const exe = executable_path().string();
  if (exe.rfind("/opt/podlens/", 0) == 0 || exe.rfind("/usr/", 0) == 0) {
    return "package";
  }
  return "tarball";
}

// Dev trees (.rivet/stage) have no packaged layout story; auto-update stays
// off for them (docs/UPDATE.md silent mode).
bool is_dev_copy() {
  std::error_code ec;
  return !std::filesystem::exists(executable_path().parent_path() / "res" /
                                     "core.zo",
                                 ec);
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
// Host strings — zh default, en fallback (the same split as the Windows and
// macOS hosts; no runtime switch yet, PODLENS_LANG=en opts out).
// ---------------------------------------------------------------------------

char const* tr(char const* key) {
  struct Pair {
    char const* key;
    char const* zh;
    char const* en;
  };
  static constexpr Pair kTable[] = {
      {"nav.home", "主页", "Home"},
      {"nav.discover", "发现", "Discover"},
      {"nav.library", "资料库", "Library"},
      {"nav.recent", "最近更新", "Recently Updated"},
      {"nav.shows", "节目", "Shows"},
      {"nav.downloaded", "已下载", "Downloaded"},
      {"nav.subscriptions", "我的订阅", "Subscriptions"},
      {"page.upnext", "待播清单", "Up Next"},
      {"page.myshows", "我的节目", "My Shows"},
      {"page.latestepisodes", "最新单集", "Latest Episodes"},
      {"page.home", "主页", "Home"},
      {"page.discover", "发现", "Discover"},
      {"episode.count", " 集", " eps"},
      {"episode.new", " 新单集", " new"},
      {"action.add_feed", "添加订阅", "Add Feed"},
      {"action.understand", "一键听懂", "Understand"},
      {"action.mark_played", "标记已听", "Mark played"},
      {"action.mark_unplayed", "标记未听", "Mark unplayed"},
      {"action.delete_audio", "删除音频", "Delete Audio"},
      {"action.add", "添加", "Add"},
      {"action.added", "已订阅", "Added"},
      {"action.preview", "试听", "Preview"},
      {"mode.bilingual", "双语", "Bilingual"},
      {"focus.enter", "专注聆听", "Focus Listening"},
      {"focus.exit", "显示资料库", "Show Library"},
      {"focus.enter_hint", "收起导航与单集列表，专注逐句稿和播放器",
       "Hide navigation and episode lists to focus on the transcript and player"},
      {"transcript.follow", "跟随", "Follow"},
      {"transcript.follow_hint", "播放时自动将当前句保持在视野中",
       "Keep the current sentence in view during playback"},
      {"transcript.current", "当前句", "Current"},
      {"transcript.current_hint", "返回正在播放的句子并恢复跟随",
       "Return to the playing sentence and resume following"},
      {"transcript.seek_hint", "按回车或空格跳播到这句话",
       "Press Enter or Space to seek to this sentence"},
      {"transcript.sentence", "逐句稿句子", "Transcript sentence"},
      {"player.play_pause", "播放或暂停", "Play or pause"},
      {"player.back15", "后退 15 秒", "Back 15 seconds"},
      {"player.forward30", "前进 30 秒", "Forward 30 seconds"},
      {"player.chapter_prev", "上一章", "Previous chapter"},
      {"player.chapter_next", "下一章", "Next chapter"},
      {"player.rate", "倍速", "Speed"},
      {"sleep.title", "定时停止", "Sleep Timer"},
      {"sleep.off", "关闭", "Off"},
      {"sleep.minutes", "%d 分钟", "%d minutes"},
      {"sleep.fired", "定时已到，暂停播放", "Sleep timer: paused"},
      {"transcript.none_selected", "还没有选择单集。", "No episode selected."},
      {"transcript.none",
       "还没有逐句稿，点「一键听懂」开始（需先在设置里配置 API key）。",
       "No transcript yet — press Understand (needs an API key in Settings)."},
      {"status.starting", "正在启动嵌入式 Racket CS…",
       "Starting embedded Racket CS…"},
      {"status.ready", "就绪", "Ready"},
      {"status.playing", "播放中", "Playing"},
      {"status.finished", "播放结束，进度已保存",
       "Playback finished — position saved"},
      {"status.autoplay_next", "本集播完，自动播放下一集",
       "Episode finished — playing the next one"},
      {"status.need_download", "本地还没有音频，已开始下载",
       "No local audio yet — download started"},
      {"status.preview_fetch", "正在获取单集信息…", "Fetching episode info…"},
      {"status.preview_download", "正在下载试听音频…",
       "Downloading preview audio…"},
      {"status.preview_marker", "试听", "Preview"},
      {"status.preview_playing", "试听中：", "Previewing: "},
      {"status.preview_ended", "试听结束——添加订阅即可收听完整节目",
       "Preview ended — subscribe for the full show"},
      {"status.preview_failed", "试听失败：", "Preview failed: "},
      {"status.preview_no_enclosure", "最新一集没有音频附件",
       "The latest episode has no audio"},
      {"discover.search_placeholder", "搜索全部播客，回车搜索…",
       "Search all podcasts, press return…"},
      {"discover.search_failed", "搜索失败：", "Search failed: "},
      {"empty.no_feeds", "还没有订阅\n去发现页添加几档节目吧",
       "No subscriptions yet\nAdd a few shows from Discover"},
      {"empty.pick_episode", "选择一集开始", "Pick an episode to begin"},
      {"empty.recent", "暂无单集，点右上角刷新试试",
       "No episodes yet — try Refresh All"},
      {"menu.check_updates", "检查更新…", "Check for Updates…"},
      {"menu.refresh_all", "刷新全部订阅", "Refresh All Subscriptions"},
      {"menu.settings", "设置…", "Settings…"},
      {"menu.about", "关于 PodLens", "About PodLens"},
      {"menu.add_feed", "添加订阅", "Add Subscription"},
      {"menu.feed_refresh", "刷新", "Refresh"},
      {"menu.feed_remove", "取消订阅", "Unsubscribe"},
      {"dialog.cancel", "取消", "Cancel"},
      {"dialog.add", "添加", "Add"},
      {"dialog.ok", "好", "OK"},
      {"dialog.remove", "删除", "Remove"},
      {"dialog.settings_title", "设置", "Settings"},
      {"action.save", "保存", "Save"},
      {"action.start", "开始", "Start"},
      {"action.close", "关闭", "Close"},
      {"dialog.understand", "一键听懂", "Understand"},
      {"status.feed_added", "已添加订阅", "Subscription added"},
      {"status.saved", "已保存", "Settings saved"},
      {"status.settings_hint", "按「保存」后生效。", "Changes apply when you press Save."},
      {"menu.help", "帮助", "Help"},
      {"menu.options", "选项", "Options"},
      {"menu.main", "主菜单", "Main Menu"},
      {"menu.check_updates_done", "已是最新版本", "You're up to date"},
      {"menu.check_updates_failed", "检查更新失败：", "Update check failed: "},
      {"menu.feed_refresh_one", "刷新该节目", "Refresh This Feed"},
      {"menu.feed_remove_one", "取消订阅该节目", "Remove This Feed"},
      {"notify.new_episodes", "刷新完成，新增 %d 集", "Refreshed — %d new episode(s)"},
      {"update.available_title", "发现新版本", "Update available"},
      {"update.available_body", "新版本 %s 已发布，现在下载吗？",
       "Version %s is available — download it now?"},
      {"update.download", "下载", "Download"},
      {"update.dev", "当前是开发构建，无法自动更新。",
       "This is a development copy — auto-update is unavailable."},
      {"update.package",
       "此副本来自系统包管理器，请用包管理器升级 PodLens。",
       "This copy came from the system package manager — use it to "
       "upgrade PodLens."},
      {"update.open_releases", "前往发布页", "Open Releases Page"},
      {"update.checking", "正在检查更新…", "Checking for updates…"},
      {"about.website", "访问官网", "Website"},
      {"about.issues", "问题反馈", "Report an Issue"},
      {"about.tagline",
       "听得懂的英文播客：用你自己的 LLM API key 转写、翻译、总结英文单集。",
       "Podcasts, in your language. Transcribes, translates and summarizes "
       "English episodes with your own LLM API key."},
  };
  char const* lang = getenv("PODLENS_LANG");
  bool const english = lang != nullptr && std::strcmp(lang, "en") == 0;
  for (auto const& pair : kTable) {
    if (std::strcmp(pair.key, key) == 0) return english ? pair.en : pair.zh;
  }
  return key;
}

// ---------------------------------------------------------------------------
// View model + state
// ---------------------------------------------------------------------------

struct FeedRow {
  std::string id;
  std::string title;
  std::string artwork;
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

// episode-recent row: the episode-list projection prefixed with the owning
// feed's identity (feed-id feed-title feed-artwork <episode row…>)
struct RecentRow {
  std::string feed_id;
  std::string feed_title;
  std::string artwork;
  EpisodeRow episode;
};

struct ChapterMark {
  double start{0};
  std::string title;
};

struct AppState {
  GtkWindow* window{nullptr};
  GtkButton* pipeline_button{nullptr};
  GtkButton* download_button{nullptr};
  GtkButton* transcribe_button{nullptr};
  GtkButton* translate_button{nullptr};
  GtkButton* summarize_button{nullptr};
  GtkButton* mark_button{nullptr};
  GtkButton* remove_audio_button{nullptr};
  GtkListBox* feed_list{nullptr};  // the navigation sidebar
  GtkWidget* sidebar{nullptr};
  GtkLabel* episodes_caption{nullptr};
  GtkListBox* episode_list{nullptr};
  GtkStack* list_stack{nullptr};
  GtkLabel* empty_icon{nullptr};
  GtkLabel* empty_text{nullptr};

  // Page host + library pages (Podcasts-style home / library lanes).
  GtkStack* page_stack{nullptr};
  GtkBox* home_list{nullptr};
  GtkBox* recent_list{nullptr};
  GtkBox* downloaded_list{nullptr};
  GtkFlowBox* shows_flow{nullptr};
  GtkBox* discover_box{nullptr};
  GtkEntry* discover_entry{nullptr};
  GtkBox* discover_results{nullptr};
  bool discover_loaded{false};

  // Cross-feed lane data behind home / recent / downloaded.
  std::vector<RecentRow> recents;
  std::string current_page{"home"};

  // Detail pane
  GtkBox* detail_box{nullptr};
  GtkToggleButton* focus_button{nullptr};
  GtkLabel* detail_title{nullptr};
  GtkLabel* detail_meta{nullptr};
  GtkLabel* detail_notes{nullptr};
  GtkProgressBar* job_bar{nullptr};
  GtkLabel* job_label{nullptr};
  GtkBox* transcript_box{nullptr};
  GtkBox* summary_box{nullptr};

  // Player bar
  GtkBox* player_bar{nullptr};
  GtkImage* player_artwork{nullptr};
  GtkLabel* player_title{nullptr};
  GtkLabel* player_show{nullptr};
  GtkButton* play_button{nullptr};
  GtkButton* skip_back_button{nullptr};
  GtkButton* skip_forward_button{nullptr};
  GtkButton* chapter_back_button{nullptr};
  GtkButton* chapter_forward_button{nullptr};
  GtkScale* seek_scale{nullptr};
  GtkLabel* position_text{nullptr};
  GtkLabel* duration_text{nullptr};

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

  // Playback (GStreamer playbin over the local audio cache).
  GstElement* playbin{nullptr};
  guint bus_watch_id{0};
  guint tick_source_id{0};
  double playback_rate{1.0};
  gint64 media_duration{0};
  double pending_seek{-1.0};  // applied once the pipeline is clocked
  int ticks_since_save{0};
  bool user_seeking{false};
  std::vector<ChapterMark> chapters;

  // Transcript reading: show the line-by-line translation under the original.
  bool transcript_bilingual{true};
  // Segment widgets parallel to the rendered transcript rows; the player
  // tick highlights and follows the running sentence among them.
  std::vector<GtkWidget*> segment_widgets;
  int followed_segment{-1};
  bool transcript_follow{true};
  GtkToggleButton* follow_button{nullptr};
  GtkScrolledWindow* transcript_scroll{nullptr};

  // Try-before-subscribe audition owning the player (no library episode).
  bool preview_active{false};
  std::string preview_title;
  std::string preview_show;

  // Sleep timer (minutes; 0 = disarmed).
  guint sleep_source_id{0};
  int sleep_minutes{0};

  // Update flow state (UPDATE.md family contract).
  bool update_check_in_flight{false};
  bool update_download_in_flight{false};
  GtkWindow* update_progress_dialog{nullptr};
  GtkProgressBar* update_progress_bar{nullptr};
  GtkLabel* update_progress_label{nullptr};
  guint update_poll_source{0};
  std::string update_downloaded_path;
  bool check_updates_enabled{true};
  long long last_update_check{0};
  bool notifications_disabled{false};
};

AppState g_state;

// Forward declarations.
void reload_feeds();
void reload_episodes();
void reload_recent();
void show_page(char const* name);
void render_home();
void render_recent_page();
void render_downloaded_page();
void render_shows();
void select_recent(RecentRow const& row, bool autoplay);
void show_episode_details(EpisodeRow const* episode);
std::string format_pub_date(std::string const& raw);
void load_discover();
void start_playback();
void load_transcript();
void load_summary();
void load_chapters();
EpisodeRow const* selected_episode();
void update_action_buttons();
void render_transcript_public();
void render_summary_public();
void start_job(std::string const& kind, std::string const& episode_id);
void stop_playback(bool save);
void set_play_button_glyph(bool playing);
void apply_seek(double seconds);
void highlight_running_segment(double position);
void start_preview_playback(std::string const& path, std::string const& title,
                            std::string const& show);
void mpris_start();
void mpris_update();
void mpris_update(char const* const* properties);
void mpris_seeked(double seconds);

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
      ".pl-seg-active { background: alpha(currentColor, 0.12); }\n"
      ".pl-seg-time { font-family: monospace; font-size: 12px; "
      "opacity: 0.65; }\n"
      ".pl-section { font-size: 13px; font-weight: 700; opacity: 0.85; "
      "margin-top: 10px; }\n"
      ".pl-statusbar { border-top: 1px solid alpha(currentColor, 0.15); "
      "padding: 3px 12px; }\n"
      ".pl-page-title { font-size: 22px; font-weight: 700; }\n"
      ".pl-tile-label { font-size: 12px; }\n"
      ".pl-artwork { border-radius: 6px; background: "
      "alpha(currentColor, 0.10); }\n"
      ".pl-card-art { border-radius: 6px; }\n"
      ".pl-upnext { border-radius: 12px; overflow: hidden; }\n"
      ".pl-upnext:hover { filter: brightness(1.08); }\n"
      ".pl-lane { padding: 10px; border-radius: 8px; }\n"
      ".pl-lane:hover { background: alpha(currentColor, 0.06); }\n"
      ".pl-flow > child { margin: 6px; }\n"
      ".pl-progress { min-height: 4px; }\n");
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

// ---------------------------------------------------------------------------
// Cover art — streams the URL through the system curl into a pixbuf. Zero
// new dependencies (no libsoup); a missing/broken curl or URL just leaves
// the quiet placeholder. The GCancellable rides the GtkImage and is
// cancelled on destroy, so a torn-down page never writes into a dead
// widget.
// ---------------------------------------------------------------------------

void load_artwork(GtkImage* image, std::string const& url) {
  if (url.empty()) return;
  GError* error = nullptr;
  auto* cancellable = g_cancellable_new();
  // Destroying the image cancels the in-flight fetch, so the completion
  // callback never touches a dead widget.
  g_object_set_data_full(G_OBJECT(image), "artwork-cancel", cancellable,
                         +[](gpointer data) {
                           g_object_unref(static_cast<GCancellable*>(data));
                         });
  g_signal_connect(image, "destroy",
                   G_CALLBACK(+[](GtkWidget*, gpointer data) {
                     g_cancellable_cancel(static_cast<GCancellable*>(data));
                   }),
                   cancellable);
  auto* proc = g_subprocess_new(G_SUBPROCESS_FLAGS_STDOUT_PIPE, &error, "curl",
                                "-fsSL", "--max-time", "20", url.c_str(),
                                static_cast<void*>(nullptr));
  if (proc == nullptr) {
    g_error_free(error);
    return;
  }
  // The pixbuf callback unrefs the process through this slot; register it
  // before the fetch so even the fastest failure path finds it.
  g_object_set_data_full(G_OBJECT(image), "artwork-proc", proc,
                         +[](gpointer data) {
                           g_object_unref(static_cast<GSubprocess*>(data));
                         });
  GInputStream* stream = g_subprocess_get_stdout_pipe(proc);
  gdk_pixbuf_new_from_stream_async(
      stream, cancellable,
      reinterpret_cast<GAsyncReadyCallback>(+[](GObject*, GAsyncResult* result,
                                                gpointer data) {
        auto* image = GTK_IMAGE(data);
        GError* load_error = nullptr;
        auto* pixbuf = gdk_pixbuf_new_from_stream_finish(result, &load_error);
        if (load_error != nullptr) {
          g_error_free(load_error);
        }
        if (pixbuf == nullptr) return;
        if (gtk_widget_get_parent(GTK_WIDGET(image)) != nullptr) {
          gtk_image_set_from_pixbuf(image, pixbuf);
        }
        g_object_unref(pixbuf);
      }),
      image);
}

// "Fri, 16 Oct 2026 21:30:00 +0000" -> "Oct 16"; unparsable input passes
// through untouched. (The Linux host speaks English by design.)
std::string format_pub_date(std::string const& raw) {
  std::tm tm{};
  char const* const formats[] = {"%a, %d %b %Y %H:%M:%S %Z",
                                 "%a, %d %b %Y %H:%M:%S %z",
                                 "%Y-%m-%d %H:%M:%S %z",
                                 "%a, %d %b %Y"};
  for (char const* format : formats) {
    tm = {};
    if (strptime(raw.c_str(), format, &tm) != nullptr) {
      char buffer[32];
      strftime(buffer, sizeof(buffer), "%b %e", &tm);
      return buffer;
    }
  }
  return raw;
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
// Sidebar — destinations + subscriptions
// ---------------------------------------------------------------------------

GtkWidget* make_nav_row(char const* icon_name, char const* label,
                        char const* dest) {
  auto* box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
  auto* icon = gtk_image_new_from_icon_name(icon_name);
  gtk_box_append(GTK_BOX(box), icon);
  auto* text = gtk_label_new(label);
  gtk_label_set_xalign(GTK_LABEL(text), 0.0f);
  gtk_box_append(GTK_BOX(box), text);

  auto* row = gtk_list_box_row_new();
  gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), box);
  carry_data<std::string>(G_OBJECT(row), "dest", dest);
  return row;
}

GtkWidget* make_header_row(char const* label) {
  auto* text = gtk_label_new(label);
  gtk_label_set_xalign(GTK_LABEL(text), 0.0f);
  gtk_widget_add_css_class(text, "pl-caption");
  auto* row = gtk_list_box_row_new();
  gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), text);
  gtk_list_box_row_set_selectable(GTK_LIST_BOX_ROW(row), FALSE);
  gtk_widget_set_sensitive(GTK_WIDGET(row), FALSE);
  return row;
}

GtkWidget* make_feed_row(FeedRow const& feed) {
  auto* box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  auto* artwork = gtk_image_new();
  gtk_image_set_pixel_size(GTK_IMAGE(artwork), 28);
  gtk_widget_add_css_class(artwork, "pl-artwork");
  gtk_widget_set_valign(artwork, GTK_ALIGN_START);
  gtk_box_append(GTK_BOX(box), artwork);
  load_artwork(GTK_IMAGE(artwork), feed.artwork);

  auto* lines = gtk_box_new(GTK_ORIENTATION_VERTICAL, 1);
  auto* title = gtk_label_new(feed.title.c_str());
  gtk_label_set_ellipsize(GTK_LABEL(title), PANGO_ELLIPSIZE_END);
  gtk_label_set_xalign(GTK_LABEL(title), 0.0f);
  gtk_widget_add_css_class(title, "pl-title");
  gtk_box_append(GTK_BOX(lines), title);
  std::string meta = feed.count + tr("episode.count");
  if (feed.unplayed > 0) {
    auto* chip = gtk_label_new(
        (std::to_string(feed.unplayed) + tr("episode.new")).c_str());
    gtk_widget_add_css_class(chip, "pl-chip");
    gtk_label_set_xalign(GTK_LABEL(chip), 0.0f);
    gtk_box_append(GTK_BOX(lines), GTK_WIDGET(chip));
  }
  auto* meta_label = gtk_label_new(meta.c_str());
  gtk_label_set_ellipsize(GTK_LABEL(meta_label), PANGO_ELLIPSIZE_END);
  gtk_label_set_xalign(GTK_LABEL(meta_label), 0.0f);
  gtk_widget_add_css_class(meta_label, "pl-meta");
  gtk_box_append(GTK_BOX(lines), meta_label);
  gtk_box_append(GTK_BOX(box), lines);

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
  gtk_list_box_append(list,
                      make_nav_row("user-home-symbolic", tr("nav.home"), "home"));
  gtk_list_box_append(
      list, make_nav_row("application-rss+xml-symbolic", tr("nav.discover"),
                         "discover"));
  gtk_list_box_append(list, make_header_row(tr("nav.library")));
  gtk_list_box_append(list,
                      make_nav_row("document-open-recent-symbolic",
                                   tr("nav.recent"), "recent"));
  gtk_list_box_append(
      list, make_nav_row("view-grid-symbolic", tr("nav.shows"), "shows"));
  gtk_list_box_append(
      list, make_nav_row("folder-download-symbolic", tr("nav.downloaded"),
                         "downloaded"));
  gtk_list_box_append(list, make_header_row(tr("nav.subscriptions")));

  for (auto const& row : rows) {
    if (row.size() < 5) continue;
    FeedRow feed;
    feed.id = row[0];
    feed.title = row[1];
    feed.artwork = row.size() > 3 ? row[3] : "";
    feed.count = row[4];
    feed.unplayed = row.size() > 7 ? std::atoi(row[7].c_str()) : 0;
    g_state.feeds.push_back(feed);
    gtk_list_box_append(list, make_feed_row(feed));
  }

  // Re-select the row matching the current view (a refresh keeps the user
  // in place; the sidebar rows were just rebuilt).
  std::string wanted_dest = g_state.current_page;
  std::string wanted_feed;
  if (g_state.current_page == "episodes") wanted_feed = g_state.selected_feed_id;
  g_state.suppress_feed_selection = true;
  int index = 0;
  GtkListBoxRow* select = nullptr;
  while (auto* child = gtk_list_box_get_row_at_index(list, index)) {
    if (auto const* dest = carried_data<std::string>(G_OBJECT(child), "dest")) {
      if (*dest == wanted_dest) select = child;
    }
    if (auto const* feed_id =
            carried_data<std::string>(G_OBJECT(child), "feed-id")) {
      if (!wanted_feed.empty() && *feed_id == wanted_feed) select = child;
    }
    if (select != nullptr) {
      gtk_list_box_select_row(list, select);
      break;
    }
    ++index;
  }
  g_state.suppress_feed_selection = false;

  reload_recent();
  if (g_state.current_page == "home") render_home();
  if (g_state.current_page == "shows") render_shows();

  // Keep or restore the selection: resume-on-launch opens its feed's page;
  // otherwise the sidebar selection follows the page the user is on (the
  // list was just rebuilt, so re-select the current feed's row).
  if (!g_state.pending_resume_feed_id.empty()) {
    g_state.selected_feed_id = g_state.pending_resume_feed_id;
    g_state.pending_resume_feed_id.clear();
    show_page("episodes");
    reload_episodes();
  }
  if (!g_state.selected_feed_id.empty()) {
    int index = 0;
    g_state.suppress_feed_selection = true;
    while (auto* child = gtk_list_box_get_row_at_index(list, index)) {
      if (auto const* feed_id =
              carried_data<std::string>(G_OBJECT(child), "feed-id")) {
        if (*feed_id == g_state.selected_feed_id) {
          gtk_list_box_select_row(list, child);
          break;
        }
      }
      ++index;
    }
    g_state.suppress_feed_selection = false;
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

void on_nav_row_selected(GtkListBox*, GtkListBoxRow* row, gpointer) {
  if (g_state.suppress_feed_selection) return;
  if (row == nullptr) return;
  if (auto const* dest = carried_data<std::string>(G_OBJECT(row), "dest")) {
    show_page(dest->c_str());
    return;
  }
  auto const* id = carried_data<std::string>(G_OBJECT(row), "feed-id");
  if (id == nullptr) return;
  if (g_state.selected_feed_id == *id &&
      g_state.current_page == "episodes") {
    return;  // already showing this feed
  }
  g_state.selected_feed_id = *id;
  for (auto const& feed : g_state.feeds) {
    if (feed.id == *id) {
      gtk_label_set_text(g_state.episodes_caption, feed.title.c_str());
      break;
    }
  }
  reload_episodes();
  show_page("episodes");
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

  auto* refresh = g_menu_item_new(tr("menu.feed_refresh_one"), nullptr);
  g_menu_item_set_action_and_target_value(
      refresh, "win.feed-refresh", g_variant_new_string(id->c_str()));
  auto* remove = g_menu_item_new(tr("menu.feed_remove_one"), nullptr);
  g_menu_item_set_action_and_target_value(
      remove, "win.feed-remove", g_variant_new_string(id->c_str()));
  auto* menu = g_menu_new();
  g_menu_append_item(menu, refresh);
  g_menu_append_item(menu, remove);
  g_object_unref(refresh);
  g_object_unref(remove);
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

// ---------------------------------------------------------------------------
// Cross-feed lanes — home / recently updated / downloaded / shows
// ---------------------------------------------------------------------------

// The Up Next lane: in-progress episodes first, then each show's newest
// unplayed episode — Apple Podcasts' queue semantics for a library with
// no explicit queue.
std::vector<RecentRow> up_next() {
  std::vector<RecentRow> lane;
  std::set<std::string> seen_feeds;
  for (auto const& item : g_state.recents) {
    if (item.episode.done) continue;
    if (item.episode.position_sec > 5) {
      lane.push_back(item);
      seen_feeds.insert(item.feed_id);
    }
  }
  for (auto const& item : g_state.recents) {
    if (item.episode.done) continue;
    if (lane.size() >= 12) break;
    if (seen_feeds.count(item.feed_id) == 0) {
      lane.push_back(item);
      seen_feeds.insert(item.feed_id);
    }
  }
  return lane;
}

GtkWidget* make_card_artwork(std::string const& url) {
  auto* artwork = gtk_image_new();
  gtk_image_set_pixel_size(GTK_IMAGE(artwork), 56);
  gtk_widget_add_css_class(artwork, "pl-card-art");
  gtk_widget_set_valign(artwork, GTK_ALIGN_START);
  load_artwork(GTK_IMAGE(artwork), url);
  return artwork;
}

GtkWidget* make_chip_label(std::string const& text) {
  auto* chip = gtk_label_new(text.c_str());
  gtk_widget_add_css_class(chip, "pl-chip");
  gtk_widget_set_valign(chip, GTK_ALIGN_START);
  return chip;
}

GtkWidget* make_progress_line(RecentRow const& item) {
  auto* progress = gtk_progress_bar_new();
  gtk_progress_bar_set_fraction(
      GTK_PROGRESS_BAR(progress),
      item.episode.duration_sec > 0
          ? std::min(1.0, item.episode.position_sec / item.episode.duration_sec)
          : 0.0);
  gtk_widget_add_css_class(progress, "pl-progress");
  return progress;
}

GtkWidget* make_episode_card(RecentRow const& item, bool show_feed) {
  auto* card = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
  gtk_widget_add_css_class(card, "pl-lane");
  gtk_box_append(GTK_BOX(card), make_card_artwork(item.artwork));

  auto* lines = gtk_box_new(GTK_ORIENTATION_VERTICAL, 3);
  gtk_widget_set_hexpand(lines, TRUE);
  auto* title = gtk_label_new(item.episode.title.c_str());
  gtk_label_set_wrap(GTK_LABEL(title), TRUE);
  gtk_label_set_xalign(GTK_LABEL(title), 0.0f);
  gtk_widget_add_css_class(title, "pl-title");
  gtk_box_append(GTK_BOX(lines), title);

  std::string meta;
  if (show_feed) meta += item.feed_title + " · ";
  meta += format_pub_date(item.episode.pub);
  if (item.episode.duration_sec > 0) {
    meta += " · " + format_seconds(item.episode.duration_sec);
  }
  auto* meta_label = gtk_label_new(meta.c_str());
  gtk_label_set_xalign(GTK_LABEL(meta_label), 0.0f);
  gtk_widget_add_css_class(meta_label, "pl-meta");
  gtk_box_append(GTK_BOX(lines), meta_label);

  auto* chips = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
  if (item.episode.done) gtk_box_append(GTK_BOX(chips), make_chip_label("played"));
  if (item.episode.downloaded) gtk_box_append(GTK_BOX(chips), make_chip_label("downloaded"));
  if (item.episode.transcript_status == "done") gtk_box_append(GTK_BOX(chips), make_chip_label("transcript"));
  if (item.episode.transcript_status == "running") gtk_box_append(GTK_BOX(chips), make_chip_label("transcribing"));
  if (item.episode.has_translation) gtk_box_append(GTK_BOX(chips), make_chip_label("translated"));
  if (item.episode.summary_status == "done") gtk_box_append(GTK_BOX(chips), make_chip_label("summary"));
  if (!item.episode.description.empty()) {
    auto* notes = gtk_label_new(item.episode.description.c_str());
    gtk_label_set_ellipsize(GTK_LABEL(notes), PANGO_ELLIPSIZE_END);
    gtk_label_set_xalign(GTK_LABEL(notes), 0.0f);
    gtk_widget_add_css_class(notes, "pl-meta");
    gtk_box_append(GTK_BOX(chips), notes);
  }
  if (gtk_widget_get_first_child(chips) != nullptr) {
    gtk_box_append(GTK_BOX(lines), chips);
  }
  if (item.episode.position_sec > 5 && !item.episode.done) {
    gtk_box_append(GTK_BOX(lines), make_progress_line(item));
  }
  gtk_box_append(GTK_BOX(card), lines);

  auto* play = gtk_button_new_from_icon_name("media-playback-start-symbolic");
  gtk_widget_set_valign(play, GTK_ALIGN_CENTER);
  gtk_widget_add_css_class(play, "flat");
  carry_data<RecentRow>(G_OBJECT(play), "recent-row", item);
  g_signal_connect(play, "clicked",
                   G_CALLBACK(+[](GtkButton* button, gpointer) {
                     auto const* row =
                         carried_data<RecentRow>(G_OBJECT(button), "recent-row");
                     if (row != nullptr) select_recent(*row, true);
                   }),
                   nullptr);
  gtk_box_append(GTK_BOX(card), play);

  RecentRow const row_copy = item;
  carry_data<RecentRow>(G_OBJECT(card), "recent-row", row_copy);
  auto* gesture = gtk_gesture_click_new();
  gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(gesture), GDK_BUTTON_PRIMARY);
  g_signal_connect(
      gesture, "released",
      G_CALLBACK(+[](GtkGestureClick*, int, double, double, gpointer data) {
        auto* card_widget = GTK_WIDGET(data);
        auto const* row =
            carried_data<RecentRow>(G_OBJECT(card_widget), "recent-row");
        if (row != nullptr) select_recent(*row, false);
      }),
      card);
  gtk_widget_add_controller(card, GTK_EVENT_CONTROLLER(gesture));
  return card;
}

GtkWidget* make_up_next_card(RecentRow const& item) {
  auto* overlay = gtk_overlay_new();
  auto* artwork = gtk_image_new();
  gtk_widget_add_css_class(artwork, "pl-card-art");
  gtk_widget_set_size_request(artwork, 200, 200);
  gtk_widget_set_halign(artwork, GTK_ALIGN_FILL);
  gtk_widget_set_valign(artwork, GTK_ALIGN_FILL);
  load_artwork(GTK_IMAGE(artwork), item.artwork);
  gtk_overlay_set_child(GTK_OVERLAY(overlay), artwork);

  auto* lines = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
  gtk_widget_set_margin_top(lines, 100);
  gtk_widget_set_margin_start(lines, 14);
  gtk_widget_set_margin_end(lines, 14);
  gtk_widget_set_margin_bottom(lines, 12);
  gtk_widget_set_valign(lines, GTK_ALIGN_END);
  gtk_widget_set_halign(lines, GTK_ALIGN_FILL);

  std::string const lane = item.episode.position_sec > 5
                               ? std::string("Continue listening")
                               : std::string("New Episode");
  std::string const meta_text =
      item.episode.pub.empty()
          ? lane
          : lane + " · " + format_pub_date(item.episode.pub);
  auto* meta = gtk_label_new(nullptr);
  gtk_label_set_markup(GTK_LABEL(meta), meta_text.c_str());
  gtk_label_set_xalign(GTK_LABEL(meta), 0.0f);
  gtk_widget_add_css_class(meta, "pl-meta");
  gtk_widget_add_css_class(meta, "pl-scrim");
  gtk_box_append(GTK_BOX(lines), meta);

  auto* title = gtk_label_new(item.episode.title.c_str());
  gtk_label_set_wrap(GTK_LABEL(title), TRUE);
  gtk_label_set_xalign(GTK_LABEL(title), 0.0f);
  gtk_label_set_lines(GTK_LABEL(title), 2);
  gtk_label_set_ellipsize(GTK_LABEL(title), PANGO_ELLIPSIZE_END);
  gtk_widget_add_css_class(title, "pl-title");
  gtk_widget_add_css_class(title, "pl-scrim");
  gtk_box_append(GTK_BOX(lines), title);
  gtk_overlay_add_overlay(GTK_OVERLAY(overlay), lines);
  gtk_overlay_set_measure_overlay(GTK_OVERLAY(overlay), lines, TRUE);

  auto* frame = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_add_css_class(frame, "pl-upnext");
  gtk_box_append(GTK_BOX(frame), overlay);
  RecentRow const row_copy = item;
  carry_data<RecentRow>(G_OBJECT(frame), "recent-row", row_copy);
  auto* gesture = gtk_gesture_click_new();
  gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(gesture), GDK_BUTTON_PRIMARY);
  g_signal_connect(
      gesture, "released",
      G_CALLBACK(+[](GtkGestureClick*, int, double, double, gpointer data) {
        auto* card_widget = GTK_WIDGET(data);
        auto const* row =
            carried_data<RecentRow>(G_OBJECT(card_widget), "recent-row");
        if (row != nullptr) select_recent(*row, true);
      }),
      frame);
  gtk_widget_add_controller(frame, GTK_EVENT_CONTROLLER(gesture));
  return frame;
}

GtkWidget* make_section_caption(char const* text) {
  auto* caption = gtk_label_new(text);
  gtk_label_set_xalign(GTK_LABEL(caption), 0.0f);
  gtk_widget_add_css_class(caption, "pl-section");
  return caption;
}

void render_home() {
  while (GtkWidget* child =
             gtk_widget_get_first_child(GTK_WIDGET(g_state.home_list))) {
    gtk_box_remove(g_state.home_list, child);
  }

  auto const lane = up_next();
  if (!lane.empty()) {
    gtk_box_append(g_state.home_list,
                   make_section_caption(tr("page.upnext")));
    auto* lane_scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(lane_scroll),
                                   GTK_POLICY_AUTOMATIC, GTK_POLICY_NEVER);
    auto* lane_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    for (auto const& item : lane) {
      gtk_box_append(GTK_BOX(lane_row), make_up_next_card(item));
    }
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(lane_scroll), lane_row);
    gtk_box_append(g_state.home_list, lane_scroll);
  }

  if (!g_state.feeds.empty()) {
    gtk_box_append(g_state.home_list,
                   make_section_caption(tr("page.myshows")));
    auto* shows_flow = gtk_flow_box_new();
    gtk_flow_box_set_selection_mode(GTK_FLOW_BOX(shows_flow),
                                    GTK_SELECTION_NONE);
    gtk_flow_box_set_max_children_per_line(GTK_FLOW_BOX(shows_flow), 8);
    gtk_flow_box_set_min_children_per_line(GTK_FLOW_BOX(shows_flow), 4);
    gtk_widget_add_css_class(shows_flow, "pl-flow");
    gtk_widget_set_halign(GTK_WIDGET(shows_flow), GTK_ALIGN_START);
    for (auto const& feed : g_state.feeds) {
      auto* tile = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
      auto* artwork = gtk_image_new();
      gtk_image_set_pixel_size(GTK_IMAGE(artwork), 96);
      gtk_widget_add_css_class(artwork, "pl-card-art");
      load_artwork(GTK_IMAGE(artwork), feed.artwork);
      gtk_box_append(GTK_BOX(tile), artwork);
      auto* title = gtk_label_new(feed.title.c_str());
      gtk_label_set_ellipsize(GTK_LABEL(title), PANGO_ELLIPSIZE_END);
      gtk_label_set_xalign(GTK_LABEL(title), 0.0f);
      gtk_widget_add_css_class(title, "pl-tile-label");
      gtk_widget_set_size_request(title, 96, -1);
      gtk_box_append(GTK_BOX(tile), title);
      carry_data<std::string>(G_OBJECT(tile), "feed-id", feed.id);
      auto* gesture = gtk_gesture_click_new();
      gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(gesture),
                                    GDK_BUTTON_PRIMARY);
      g_signal_connect(
          gesture, "released",
          G_CALLBACK(+[](GtkGestureClick*, int, double, double, gpointer data) {
            auto* tile_widget = GTK_WIDGET(data);
            auto const* feed_id = carried_data<std::string>(
                G_OBJECT(tile_widget), "feed-id");
            if (feed_id == nullptr) return;
            g_state.selected_feed_id = *feed_id;
            reload_episodes();
            show_page("episodes");
            // sync the sidebar selection to this feed's row
            g_state.suppress_feed_selection = true;
            int index = 0;
            while (auto* child = gtk_list_box_get_row_at_index(
                       g_state.feed_list, index)) {
              if (auto const* carried = carried_data<std::string>(
                      G_OBJECT(child), "feed-id")) {
                if (*carried == *feed_id) {
                  gtk_list_box_select_row(g_state.feed_list, child);
                  break;
                }
              }
              ++index;
            }
            g_state.suppress_feed_selection = false;
          }),
          tile);
      gtk_widget_add_controller(tile, GTK_EVENT_CONTROLLER(gesture));
      gtk_flow_box_append(GTK_FLOW_BOX(shows_flow), tile);
    }
    gtk_box_append(g_state.home_list, shows_flow);
  }

  if (!g_state.recents.empty()) {
    gtk_box_append(g_state.home_list,
                   make_section_caption(tr("page.latestepisodes")));
    int shown = 0;
    for (auto const& item : g_state.recents) {
      gtk_box_append(g_state.home_list, make_episode_card(item, true));
      if (++shown >= 6) break;
    }
  }
}

void render_recent_page() {
  while (GtkWidget* child =
             gtk_widget_get_first_child(GTK_WIDGET(g_state.recent_list))) {
    gtk_box_remove(g_state.recent_list, child);
  }
  if (g_state.recents.empty()) {
    auto* empty = gtk_label_new(tr("empty.recent"));
    gtk_widget_add_css_class(empty, "pl-meta");
    gtk_widget_set_margin_top(empty, 24);
    gtk_box_append(g_state.recent_list, empty);
    return;
  }
  for (auto const& item : g_state.recents) {
    gtk_box_append(g_state.recent_list, make_episode_card(item, true));
  }
}

void render_downloaded_page() {
  while (GtkWidget* child =
             gtk_widget_get_first_child(GTK_WIDGET(g_state.downloaded_list))) {
    gtk_box_remove(g_state.downloaded_list, child);
  }
  bool any = false;
  for (auto const& item : g_state.recents) {
    if (!item.episode.downloaded) continue;
    gtk_box_append(g_state.downloaded_list, make_episode_card(item, true));
    any = true;
  }
  if (!any) {
    auto* empty = gtk_label_new("No downloaded episodes.");
    gtk_widget_add_css_class(empty, "pl-meta");
    gtk_widget_set_margin_top(empty, 24);
    gtk_box_append(g_state.downloaded_list, empty);
  }
}

void render_shows() {
  while (GtkWidget* child =
             gtk_widget_get_first_child(GTK_WIDGET(g_state.shows_flow))) {
    gtk_flow_box_remove(g_state.shows_flow, child);
  }
  for (auto const& feed : g_state.feeds) {
    auto* tile = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    auto* artwork = gtk_image_new();
    gtk_image_set_pixel_size(GTK_IMAGE(artwork), 120);
    gtk_widget_add_css_class(artwork, "pl-card-art");
    load_artwork(GTK_IMAGE(artwork), feed.artwork);
    gtk_box_append(GTK_BOX(tile), artwork);
    auto* title = gtk_label_new(feed.title.c_str());
    gtk_label_set_ellipsize(GTK_LABEL(title), PANGO_ELLIPSIZE_END);
    gtk_label_set_xalign(GTK_LABEL(title), 0.0f);
    gtk_widget_add_css_class(title, "pl-tile-label");
    gtk_widget_set_size_request(title, 120, -1);
    gtk_box_append(GTK_BOX(tile), title);
    carry_data<std::string>(G_OBJECT(tile), "feed-id", feed.id);
    gtk_flow_box_append(g_state.shows_flow, tile);
  }
}

void show_page(char const* name) {
  g_state.current_page = name;
  gtk_stack_set_visible_child_name(g_state.page_stack, name);
  if (std::string(name) == "home") render_home();
  if (std::string(name) == "recent") render_recent_page();
  if (std::string(name) == "downloaded") render_downloaded_page();
  if (std::string(name) == "shows") render_shows();
  if (std::string(name) == "discover" && !g_state.discover_loaded) {
    load_discover();
  }
}

void reload_recent() {
  if (g_state.api == nullptr) return;
  (void)g_state.api->episode_recent_async(
      [](rivet_app::Result<std::vector<std::vector<std::string>>> result) {
        on_result(std::move(result),
                  [](std::vector<std::vector<std::string>> const& rows) {
                    g_state.recents.clear();
                    for (auto const& row : rows) {
                      if (row.size() < 14) continue;
                      RecentRow item;
                      item.feed_id = row[0];
                      item.feed_title = row[1];
                      item.artwork = row[2];
                      item.episode.id = row[3];
                      item.episode.title = row[4];
                      item.episode.pub = row[5];
                      item.episode.duration_sec = std::atof(row[6].c_str());
                      item.episode.downloaded = row[7] == "1";
                      item.episode.transcript_status = row[8];
                      item.episode.summary_status = row[9];
                      item.episode.position_sec = std::atof(row[10].c_str());
                      item.episode.done = row[11] == "1";
                      item.episode.has_translation = row[12] == "1";
                      item.episode.description = row[13];
                      g_state.recents.push_back(item);
                    }
                    if (g_state.current_page == "home") render_home();
                    if (g_state.current_page == "recent") render_recent_page();
                    if (g_state.current_page == "downloaded") {
                      render_downloaded_page();
                    }
                  });
      });
}

void select_recent(RecentRow const& row, bool autoplay) {
  if (!g_state.selected_episode_id.empty() &&
      g_state.selected_episode_id != row.episode.id) {
    stop_playback(true);
  }
  g_state.selected_feed_id = row.feed_id;
  g_state.selected_episode_id = row.episode.id;
  for (auto const& feed : g_state.feeds) {
    if (feed.id == row.feed_id) {
      gtk_label_set_text(g_state.episodes_caption, feed.title.c_str());
      break;
    }
  }
  reload_episodes();
  show_episode_details(&row.episode);
  if (autoplay) start_playback();
}

// ---------------------------------------------------------------------------
// Discover — full-directory search + the curated catalog as artwork cards
// ---------------------------------------------------------------------------

GtkWidget* make_discover_card(std::vector<std::string> const& row) {
  if (row.size() < 7) return nullptr;
  auto* card = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
  gtk_widget_add_css_class(card, "pl-lane");
  auto* artwork = gtk_image_new();
  gtk_image_set_pixel_size(GTK_IMAGE(artwork), 56);
  gtk_widget_add_css_class(artwork, "pl-card-art");
  load_artwork(GTK_IMAGE(artwork), row.size() > 7 ? row[7] : "");
  gtk_box_append(GTK_BOX(card), artwork);

  auto* lines = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
  gtk_widget_set_hexpand(lines, TRUE);
  auto* title = gtk_label_new(row[2].c_str());
  gtk_label_set_xalign(GTK_LABEL(title), 0.0f);
  gtk_widget_add_css_class(title, "pl-title");
  gtk_box_append(GTK_BOX(lines), title);
  auto* description = gtk_label_new(row[3].c_str());
  gtk_label_set_wrap(GTK_LABEL(description), TRUE);
  gtk_label_set_xalign(GTK_LABEL(description), 0.0f);
  gtk_widget_add_css_class(description, "pl-meta");
  gtk_box_append(GTK_BOX(lines), description);
  gtk_box_append(GTK_BOX(card), lines);

  bool const added = row[6] == "1";
  auto* actions = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
  if (!added) {
    // try before you subscribe: peek the feed without subscribing, cache
    // the newest episode's audio, and play it outside the library
    auto* preview = gtk_button_new_with_label(tr("action.preview"));
    carry_data<std::string>(G_OBJECT(preview), "feed-url", row[4]);
    g_signal_connect(
        preview, "clicked",
        G_CALLBACK(+[](GtkButton* button, gpointer) {
          auto const* url =
              carried_data<std::string>(G_OBJECT(button), "feed-url");
          if (url == nullptr || g_state.api == nullptr) return;
          std::string const feed_url = *url;
          set_status(tr("status.preview_fetch"));
          (void)g_state.api->feed_peek_async(
              feed_url,
              [](rivet_app::Result<std::vector<std::vector<std::string>>>
                     result) {
                on_result(
                    std::move(result),
                    [](std::vector<std::vector<std::string>> const& rows) {
                      if (rows.empty() || rows[0].size() < 7 ||
                          rows[0][3].find_first_not_of(" \t\r\n") ==
                              std::string::npos) {
                        set_status_error(
                            std::string(tr("status.preview_no_enclosure")));
                        return;
                      }
                      std::string const enclosure = rows[0][3];
                      std::string const title = rows[0][4];
                      std::string const show = rows[0][0];
                      set_status(tr("status.preview_download"));
                      (void)g_state.api->preview_audio_async(
                          enclosure,
                          [title, show](rivet_app::Result<std::string> result) {
                            on_result(
                                std::move(result),
                                [title, show](std::string const& path) {
                                  start_preview_playback(path, title, show);
                                });
                          });
                    });
              });
        }),
        nullptr);
    gtk_box_append(GTK_BOX(actions), preview);
  }
  auto* action = gtk_button_new_with_label(added ? tr("action.added")
                                                 : tr("action.add"));
  gtk_widget_set_sensitive(action, !added);
  if (!added) {
    gtk_widget_add_css_class(action, "suggested-action");
    carry_data<std::string>(G_OBJECT(action), "feed-url", row[4]);
    g_signal_connect(
        action, "clicked",
        G_CALLBACK(+[](GtkButton* button, gpointer) {
          auto const* url = carried_data<std::string>(G_OBJECT(button), "feed-url");
          if (url == nullptr || g_state.api == nullptr) return;
          std::string const feed_url = *url;
          (void)g_state.api->feed_add_async(
              feed_url,
              [](rivet_app::Result<std::string> result) {
                on_result(std::move(result),
                          [](std::string const&) {
                            reload_feeds();
                            reload_recent();
                            if (g_state.discover_loaded) load_discover();
                          });
              });
        }),
        nullptr);
  }
  gtk_box_append(GTK_BOX(actions), action);
  gtk_widget_set_valign(actions, GTK_ALIGN_CENTER);
  gtk_box_append(GTK_BOX(card), actions);
  return card;
}

void render_discover_results(std::vector<std::vector<std::string>> const& rows,
                             std::string const& error) {
  while (GtkWidget* child =
             gtk_widget_get_first_child(GTK_WIDGET(g_state.discover_results))) {
    gtk_box_remove(g_state.discover_results, child);
  }
  if (!error.empty()) {
    auto* note = gtk_label_new(
        (std::string(tr("discover.search_failed")) + error).c_str());
    gtk_widget_add_css_class(note, "pl-meta");
    gtk_box_append(g_state.discover_results, note);
    return;
  }
  for (auto const& row : rows) {
    if (auto* card = make_discover_card(row)) {
      gtk_box_append(g_state.discover_results, card);
    }
  }
}

void on_discover_search_clicked(GtkButton*, gpointer) {
  if (g_state.api == nullptr) return;
  auto const query = std::string(gtk_editable_get_text(GTK_EDITABLE(g_state.discover_entry)));
  if (query.empty()) return;
  (void)g_state.api->catalog_search_async(
      query,
      [](rivet_app::Result<std::vector<std::vector<std::string>>> result) {
        on_result(
            std::move(result),
            [](std::vector<std::vector<std::string>> const& rows) {
              render_discover_results(rows, std::string());
            });
      });
}

void load_discover() {
  g_state.discover_loaded = true;
  if (g_state.api == nullptr) return;
  (void)g_state.api->catalog_list_async(
      [](rivet_app::Result<std::vector<std::vector<std::string>>> result) {
        on_result(std::move(result),
                  [](std::vector<std::vector<std::string>> const& rows) {
                    while (GtkWidget* child = gtk_widget_get_first_child(
                               GTK_WIDGET(g_state.discover_box))) {
                      gtk_box_remove(g_state.discover_box, child);
                    }
                    std::string current_category;
                    for (auto const& row : rows) {
                      if (row.size() < 7) continue;
                      if (row[1] != current_category) {
                        current_category = row[1];
                        gtk_box_append(g_state.discover_box,
                                       make_section_caption(row[1].c_str()));
                      }
                      if (auto* card = make_discover_card(row)) {
                        gtk_box_append(g_state.discover_box, card);
                      }
                    }
                  });
      });
}

void show_episode_details(EpisodeRow const* episode) {
  g_state.transcript.clear();
  g_state.summary_json.clear();
  if (episode != nullptr) {
    gtk_label_set_text(g_state.detail_title, episode->title.c_str());
    std::string meta = format_pub_date(episode->pub);
    if (episode->duration_sec > 0) {
      meta += " · " + format_seconds(episode->duration_sec);
    }
    if (episode->position_sec > 5 && !episode->done) {
      meta += " · at " + format_seconds(episode->position_sec);
    }
    gtk_label_set_text(g_state.detail_meta, meta.c_str());
    gtk_label_set_text(g_state.detail_notes, episode->description.c_str());
    gtk_widget_set_visible(GTK_WIDGET(g_state.detail_box), TRUE);
    // the window-level player bar follows the selection
    gtk_widget_set_visible(GTK_WIDGET(g_state.player_bar), TRUE);
    gtk_label_set_text(GTK_LABEL(g_state.player_title), episode->title.c_str());
    for (auto const& feed : g_state.feeds) {
      if (feed.id == g_state.selected_feed_id) {
        gtk_label_set_text(GTK_LABEL(g_state.player_show), feed.title.c_str());
        load_artwork(g_state.player_artwork, feed.artwork);
        break;
      }
    }
    for (auto const& item : g_state.recents) {
      if (item.episode.id == g_state.selected_episode_id) {
        gtk_label_set_text(GTK_LABEL(g_state.player_title),
                           item.episode.title.c_str());
        gtk_label_set_text(GTK_LABEL(g_state.player_show),
                           item.feed_title.c_str());
        load_artwork(g_state.player_artwork, item.artwork);
        break;
      }
    }
  }
  load_transcript();
  load_summary();
  load_chapters();
  update_action_buttons();
  mpris_update();
}

void on_episode_row_selected(GtkListBox*, GtkListBoxRow* row, gpointer) {
  if (g_state.suppress_episode_selection) return;
  if (row == nullptr) return;
  auto const* id = carried_data<std::string>(G_OBJECT(row), "episode-id");
  if (id == nullptr) return;
  if (g_state.selected_episode_id != *id) {
    // Switching episodes stops the current playback and banks its position.
    stop_playback(true);
  }
  g_state.selected_episode_id = *id;
  show_episode_details(selected_episode());
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

  auto* played = g_menu_item_new(tr("action.mark_played"), nullptr);
  g_menu_item_set_action_and_target_value(
      played, "win.episode-done", g_variant_new("(ss)", id->c_str(), "1"));
  auto* unplayed = g_menu_item_new(tr("action.mark_unplayed"), nullptr);
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
  // episodes loaded from a lane card, before the feed page finished loading
  for (auto const& item : g_state.recents) {
    if (item.episode.id == g_state.selected_episode_id) return &item.episode;
  }
  return nullptr;
}

void update_action_buttons() {
  auto const* episode = selected_episode();
  gtk_widget_set_sensitive(GTK_WIDGET(g_state.pipeline_button),
                           episode != nullptr);
  if (episode == nullptr) {
    for (GtkButton* button :
         {g_state.download_button, g_state.transcribe_button,
          g_state.translate_button, g_state.summarize_button,
          g_state.mark_button, g_state.remove_audio_button}) {
      gtk_widget_set_sensitive(GTK_WIDGET(button), FALSE);
    }
    return;
  }
  bool const transcript_running = episode->transcript_status == "running";
  bool const summary_running = episode->summary_status == "running";
  gtk_widget_set_sensitive(GTK_WIDGET(g_state.download_button),
                           !episode->downloaded);
  gtk_widget_set_sensitive(
      GTK_WIDGET(g_state.transcribe_button),
      !transcript_running && episode->transcript_status != "done");
  gtk_widget_set_sensitive(GTK_WIDGET(g_state.translate_button),
                           episode->transcript_status == "done" &&
                               !episode->has_translation);
  gtk_widget_set_sensitive(
      GTK_WIDGET(g_state.summarize_button),
      episode->transcript_status == "done" && !summary_running &&
          episode->summary_status != "done");
  gtk_widget_set_sensitive(GTK_WIDGET(g_state.mark_button), TRUE);
  gtk_button_set_label(g_state.mark_button,
                       episode->done ? tr("action.mark_unplayed")
                                     : tr("action.mark_played"));
  gtk_widget_set_visible(GTK_WIDGET(g_state.remove_audio_button),
                         episode->downloaded);
  gtk_widget_set_sensitive(GTK_WIDGET(g_state.remove_audio_button), TRUE);
}

// ---------------------------------------------------------------------------
// Transcript + summary (read-only panels)
// ---------------------------------------------------------------------------

GtkWidget* make_transcript_segment(std::vector<std::string> const& seg) {
  auto* grid = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
  auto const start = std::atof(seg[0].c_str());
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
  if (!seg[3].empty() && g_state.transcript_bilingual) {
    auto* translation = GTK_LABEL(gtk_label_new(seg[3].c_str()));
    gtk_label_set_wrap(translation, TRUE);
    gtk_label_set_xalign(translation, 0.0f);
    gtk_label_set_selectable(translation, TRUE);
    gtk_widget_add_css_class(GTK_WIDGET(translation), "pl-meta");
    gtk_box_append(GTK_BOX(lines), GTK_WIDGET(translation));
  }
  gtk_widget_set_hexpand(lines, TRUE);
  gtk_box_append(GTK_BOX(grid), lines);
  auto* button = gtk_button_new();
  gtk_button_set_child(GTK_BUTTON(button), grid);
  // Keep keyboard focus explicit: the transcript journey depends on native
  // Enter/Space activation, including under headless Wayland accessibility
  // sessions where implicit focusability can differ between GTK releases.
  gtk_widget_set_focusable(button, TRUE);
  gtk_widget_set_can_target(button, TRUE);
  gtk_widget_add_css_class(button, "flat");
  gtk_widget_add_css_class(button, "pl-seg");
  gtk_widget_set_hexpand(button, TRUE);
  gtk_widget_set_tooltip_text(button, tr("transcript.seek_hint"));
  std::string const accessible_label =
      std::string(tr("transcript.sentence")) + ": " + seg[2];
  gtk_accessible_update_property(
      GTK_ACCESSIBLE(button), GTK_ACCESSIBLE_PROPERTY_LABEL,
      accessible_label.c_str(), -1);
  carry_data<double>(G_OBJECT(button), "seg-start", start);
  carry_data<double>(G_OBJECT(button), "seg-end", std::atof(seg[1].c_str()));

  // A real button gives every sentence native focus, Enter/Space activation,
  // an accessibility action and the same click-to-seek behavior.
  g_signal_connect(
      button, "clicked",
      G_CALLBACK(+[](GtkButton* row, gpointer) {
        if (g_state.selected_episode_id.empty()) return;
        auto const* start =
            carried_data<double>(G_OBJECT(row), "seg-start");
        if (start != nullptr) {
          g_state.transcript_follow = true;
          if (g_state.follow_button != nullptr) {
            gtk_toggle_button_set_active(g_state.follow_button, TRUE);
          }
          apply_seek(*start);
        }
      }),
      nullptr);
  return button;
}

void render_transcript_public() {
  while (GtkWidget* child =
             gtk_widget_get_first_child(GTK_WIDGET(g_state.transcript_box))) {
    gtk_box_remove(g_state.transcript_box, child);
  }
  if (g_state.transcript.empty()) {
    auto* hint = GTK_LABEL(gtk_label_new(
        g_state.selected_episode_id.empty()
            ? tr("transcript.none_selected")
            : tr("transcript.none")));
    gtk_label_set_wrap(hint, TRUE);
    gtk_label_set_xalign(hint, 0.0f);
    gtk_widget_add_css_class(GTK_WIDGET(hint), "pl-meta");
    gtk_box_append(g_state.transcript_box, GTK_WIDGET(hint));
    return;
  }
  g_state.segment_widgets.clear();
  g_state.followed_segment = -1;
  for (auto const& seg : g_state.transcript) {
    if (seg.size() < 4) continue;
    if (seg[2].empty() && seg[3].empty()) continue;
    // mode filtering can empty a row (translation-only with no translation
    // yet); make_transcript_segment still renders the timestamp, so keep it
    auto* widget = make_transcript_segment(seg);
    g_state.segment_widgets.push_back(widget);
    gtk_box_append(g_state.transcript_box, widget);
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
        g_state.selected_episode_id.empty() ? tr("transcript.none_selected")
                                            : tr("transcript.none")));
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
// Playback — one GStreamer playbin over the local audio cache
// ---------------------------------------------------------------------------

// The host only tracks a binary playing/paused distinction; the playbin's
// own state machine details (PAUSED with primed buffers) are irrelevant here.
bool playbin_playing() {
  if (g_state.playbin == nullptr) return false;
  GstState state = GST_STATE_NULL;
  if (gst_element_get_state(g_state.playbin, &state, nullptr, 0) ==
      GST_STATE_CHANGE_FAILURE) {
    return false;
  }
  return state == GST_STATE_PLAYING;
}

void set_play_button_glyph(bool playing) {
  // media keys and the desktop player widget track the transport state
  static char const* const kStatus[] = {"PlaybackStatus", nullptr};
  mpris_update(kStatus);
  gtk_button_set_icon_name(
      g_state.play_button,
      playing ? "media-playback-pause-symbolic"
              : "media-playback-start-symbolic");
}

double playbin_position() {
  if (g_state.playbin == nullptr) return 0.0;
  gint64 position = 0;
  if (!gst_element_query_position(g_state.playbin, GST_FORMAT_TIME,
                                  &position)) {
    return 0.0;
  }
  return static_cast<double>(position) / GST_SECOND;
}

void save_position(bool done) {
  if (g_state.api == nullptr || g_state.selected_episode_id.empty() ||
      g_state.playbin == nullptr) {
    return;
  }
  auto const position = static_cast<long long>(playbin_position());
  (void)g_state.api->position_save_async(
      g_state.selected_episode_id, std::to_string(position),
      done ? "1" : "0", [](rivet_app::Result<bool>) {});
}

void apply_seek(double seconds) {
  if (g_state.playbin == nullptr) return;
  seconds = std::max(seconds, 0.0);
  if (g_state.media_duration > 0) {
    seconds = std::min(seconds, static_cast<double>(g_state.media_duration) /
                                    GST_SECOND);
  }
  auto const position = static_cast<gint64>(seconds * GST_SECOND);
  if (!gst_element_seek_simple(g_state.playbin, GST_FORMAT_TIME,
                               static_cast<GstSeekFlags>(GST_SEEK_FLAG_FLUSH |
                                                         GST_SEEK_FLAG_ACCURATE),
                               position)) {
    set_status_error("seek failed");
  }
  gtk_label_set_text(g_state.position_text, format_seconds(seconds).c_str());
  mpris_seeked(seconds);
}

// Rate changes on a playbin are a flushing seek carrying the new rate.
void apply_playback_rate(double rate) {
  if (g_state.playbin == nullptr) return;
  if (rate <= 0) rate = 1.0;
  g_state.playback_rate = rate;
  gint64 position = 0;
  gst_element_query_position(g_state.playbin, GST_FORMAT_TIME, &position);
  gst_element_seek(g_state.playbin, rate, GST_FORMAT_TIME,
                   static_cast<GstSeekFlags>(GST_SEEK_FLAG_FLUSH |
                                             GST_SEEK_FLAG_ACCURATE),
                   GST_SEEK_TYPE_SET, position, GST_SEEK_TYPE_NONE,
                   GST_CLOCK_TIME_NONE);
  static char const* const kRate[] = {"Rate", nullptr};
  mpris_update(kRate);
}

void start_playback() {
  auto const episode_id = g_state.selected_episode_id;
  if (episode_id.empty()) return;
  g_state.preview_active = false;  // any library playback ends an audition
  if (g_state.playbin == nullptr) {
    set_status_error("GStreamer playbin is unavailable on this system");
    return;
  }
  auto const audio = find_local_audio(episode_id);
  if (audio.empty()) {
    set_status(tr("status.need_download"));
    start_job("download", episode_id);
    return;
  }
  stop_playback(false);
  g_state.media_duration = 0;
  g_state.pending_seek = -1.0;
  g_state.ticks_since_save = 0;

  GFile* file = g_file_new_for_path(audio.c_str());
  gchar* uri = g_file_get_uri(file);
  g_object_set(g_state.playbin, "uri", uri, nullptr);
  g_free(uri);
  g_object_unref(file);

  // Resume from the recorded position past the first seconds.
  if (auto const* episode = selected_episode()) {
    if (episode->position_sec > 5 && !episode->done) {
      g_state.pending_seek = episode->position_sec;
    }
  }
  gst_element_set_state(g_state.playbin, GST_STATE_PLAYING);
  set_play_button_glyph(true);
  set_status(tr("status.playing"));
}

void toggle_play_pause() {
  // the episode-id check lives in start_playback: an audition owns the
  // player without one, and the uri check below covers both cases
  if (g_state.playbin == nullptr) {
    return;
  }
  gchar* uri = nullptr;
  g_object_get(g_state.playbin, "uri", &uri, nullptr);
  bool const has_source = uri != nullptr && uri[0] != '\0';
  g_free(uri);
  if (!has_source) {
    start_playback();
    return;
  }
  if (playbin_playing()) {
    gst_element_set_state(g_state.playbin, GST_STATE_PAUSED);
    set_play_button_glyph(false);
    save_position(false);
  } else {
    gst_element_set_state(g_state.playbin, GST_STATE_PLAYING);
    set_play_button_glyph(true);
  }
}

void stop_playback(bool save) {
  if (save) save_position(false);
  if (g_state.playbin != nullptr) {
    gst_element_set_state(g_state.playbin, GST_STATE_NULL);
    // Drop the media along with the pipeline: a stale uri would let
    // toggle_play_pause resurrect the previous episode after a switch.
    g_object_set(g_state.playbin, "uri", nullptr, nullptr);
  }
  set_play_button_glyph(false);
}

void skip_by(double delta) { apply_seek(playbin_position() + delta); }

void skip_chapter(int direction) {
  if (g_state.chapters.empty() || g_state.playbin == nullptr) return;
  double const position = playbin_position();
  if (direction > 0) {
    for (auto const& mark : g_state.chapters) {
      if (mark.start > position + 0.5) {
        apply_seek(mark.start);
        return;
      }
    }
    return;
  }
  // backward: to the current chapter's start, or the previous one when
  // already near the beginning of this chapter
  size_t idx = 0;
  double current_start = g_state.chapters.front().start;
  for (size_t i = 0; i < g_state.chapters.size(); ++i) {
    if (g_state.chapters[i].start <= position) {
      current_start = g_state.chapters[i].start;
      idx = i;
    } else {
      break;
    }
  }
  apply_seek(position - current_start < 5 && idx > 0
                 ? g_state.chapters[idx - 1].start
                 : current_start);
}

// Audition playback outside the library: the audio is already local
// (preview-audio), there is no episode id, so position saving and the
// autoplay chain all stand down until a library episode plays again.
void start_preview_playback(std::string const& path, std::string const& title,
                            std::string const& show) {
  if (g_state.playbin == nullptr) return;
  stop_playback(false);
  g_state.preview_active = true;
  g_state.selected_episode_id.clear();
  g_state.transcript.clear();
  render_transcript_public();
  g_state.media_duration = 0;
  g_state.pending_seek = -1.0;
  g_state.ticks_since_save = 0;

  GFile* file = g_file_new_for_path(path.c_str());
  gchar* uri = g_file_get_uri(file);
  g_object_set(g_state.playbin, "uri", uri, nullptr);
  g_free(uri);
  g_object_unref(file);

  gst_element_set_state(g_state.playbin, GST_STATE_PLAYING);
  set_play_button_glyph(true);
  gtk_label_set_text(GTK_LABEL(g_state.player_title), title.c_str());
  gtk_label_set_text(GTK_LABEL(g_state.player_show),
                     (show + " · " + tr("status.preview_marker")).c_str());
  gtk_widget_set_visible(GTK_WIDGET(g_state.player_bar), TRUE);
  set_status((std::string(tr("status.preview_playing")) + title).c_str());
  mpris_update();
}

void on_playback_finished() {
  if (g_state.preview_active) {
    // a preview has no library state to close out — just stop
    g_state.preview_active = false;
    gst_element_set_state(g_state.playbin, GST_STATE_NULL);
    set_play_button_glyph(false);
    set_status(tr("status.preview_ended"));
    mpris_update();
    return;
  }
  save_position(true);
  gst_element_set_state(g_state.playbin, GST_STATE_NULL);
  set_play_button_glyph(false);
  set_status(tr("status.finished"));
  reload_episodes();
  // continuous playback: roll into the next (older) episode of the same
  // feed, like Apple Podcasts without an Up Next queue
  for (size_t i = 0; i + 1 < g_state.episodes.size(); ++i) {
    if (g_state.episodes[i].id == g_state.selected_episode_id) {
      set_status(tr("status.autoplay_next"));
      g_state.selected_episode_id = g_state.episodes[i + 1].id;
      show_episode_details(&g_state.episodes[i + 1]);
      start_playback();
      break;
    }
  }
}

// Transcript follow-along: highlight the sentence the player is in and
// keep it comfortably in view. The scroll fires only when the active
// sentence changes, so 1 s ticks never yank the list mid-read.
void highlight_running_segment(double position) {
  int active = -1;
  for (size_t i = 0; i < g_state.segment_widgets.size(); ++i) {
    auto const* start =
        carried_data<double>(G_OBJECT(g_state.segment_widgets[i]), "seg-start");
    auto const* end =
        carried_data<double>(G_OBJECT(g_state.segment_widgets[i]), "seg-end");
    if (start != nullptr && end != nullptr && position >= *start &&
        position < *end) {
      active = static_cast<int>(i);
      break;
    }
  }
  if (active == g_state.followed_segment) return;
  if (g_state.followed_segment >= 0 && g_state.followed_segment <
          static_cast<int>(g_state.segment_widgets.size())) {
    gtk_widget_remove_css_class(
        g_state.segment_widgets[g_state.followed_segment], "pl-seg-active");
  }
  g_state.followed_segment = active;
  if (active < 0) return;
  GtkWidget* widget = g_state.segment_widgets[active];
  gtk_widget_add_css_class(widget, "pl-seg-active");
  if (g_state.transcript_follow && g_state.transcript_scroll != nullptr) {
    graphene_point_t point = GRAPHENE_POINT_INIT(0, 0);
    if (gtk_widget_compute_point(widget, GTK_WIDGET(g_state.transcript_box),
                                 &point, &point)) {
      double target = point.y - 80;
      gtk_adjustment_set_value(
          gtk_scrolled_window_get_vadjustment(g_state.transcript_scroll),
          target > 0 ? target : 0);
    }
  }
}

void scroll_to_running_segment() {
  if (g_state.followed_segment < 0 ||
      g_state.followed_segment >= static_cast<int>(g_state.segment_widgets.size()) ||
      g_state.transcript_scroll == nullptr) {
    return;
  }
  GtkWidget* widget = g_state.segment_widgets[g_state.followed_segment];
  graphene_point_t point = GRAPHENE_POINT_INIT(0, 0);
  if (gtk_widget_compute_point(widget, GTK_WIDGET(g_state.transcript_box),
                               &point, &point)) {
    gtk_adjustment_set_value(
        gtk_scrolled_window_get_vadjustment(g_state.transcript_scroll),
        point.y > 80 ? point.y - 80 : 0);
  }
  gtk_widget_grab_focus(widget);
}

gboolean on_bus_message(GstBus*, GstMessage* message, gpointer) {
  switch (GST_MESSAGE_TYPE(message)) {
    case GST_MESSAGE_EOS: {
      dispatch_ui([] { on_playback_finished(); });
      break;
    }
    case GST_MESSAGE_ERROR: {
      GError* error = nullptr;
      gchar* debug = nullptr;
      gst_message_parse_error(message, &error, &debug);
      std::string text = error != nullptr ? error->message : "playback error";
      g_error_free(error);
      g_free(debug);
      dispatch_ui([text] {
        gst_element_set_state(g_state.playbin, GST_STATE_NULL);
        set_play_button_glyph(false);
        set_status_error(text);
      });
      break;
    }
    default:
      break;
  }
  return G_SOURCE_CONTINUE;
}

int on_player_tick(gpointer) {
  if (g_state.playbin == nullptr) {
    return G_SOURCE_CONTINUE;
  }
  if (g_state.pending_seek >= 0) {
    // Apply the resume seek once the pipeline is clocked (position moves).
    if (playbin_position() > 0 ||
        (!playbin_playing() &&
         gst_element_get_state(g_state.playbin, nullptr, nullptr, 0) ==
             GST_STATE_CHANGE_SUCCESS)) {
      apply_seek(g_state.pending_seek);
      g_state.pending_seek = -1.0;
    }
    return G_SOURCE_CONTINUE;
  }
  if (g_state.playbin == nullptr || !playbin_playing()) {
    return G_SOURCE_CONTINUE;
  }

  gint64 duration = 0;
  if (gst_element_query_duration(g_state.playbin, GST_FORMAT_TIME,
                                 &duration) &&
      duration > 0) {
    g_state.media_duration = duration;
  }
  double const position = playbin_position();
  double const duration_sec =
      static_cast<double>(g_state.media_duration) / GST_SECOND;

  // Programmatic slider sync only while the user's hand is off the handle;
  // stomping user_seeking here would break a drag in progress.
  if (!g_state.user_seeking) {
    gtk_range_set_range(GTK_RANGE(g_state.seek_scale), 0,
                        duration_sec > 0 ? duration_sec : 100);
    gtk_range_set_value(GTK_RANGE(g_state.seek_scale), position);
  }
  gtk_label_set_text(g_state.position_text, format_seconds(position).c_str());
  gtk_label_set_text(g_state.duration_text,
                     format_seconds(duration_sec).c_str());
  highlight_running_segment(position);

  if (duration_sec > 0 && position >= duration_sec - 0.75) {
    // The EOS message owns the end-of-stream handoff; this guard keeps a
    // stalled pipeline from saving `done` early.
    return G_SOURCE_CONTINUE;
  }
  if (++g_state.ticks_since_save >= 5) {
    g_state.ticks_since_save = 0;
    save_position(false);
  }
  return G_SOURCE_CONTINUE;
}

void player_init() {
  gst_init(nullptr, nullptr);
  g_state.playbin = gst_element_factory_make("playbin", "playbin");
  if (g_state.playbin == nullptr) {
    set_status_error("GStreamer playbin is unavailable on this system");
    gtk_widget_set_sensitive(GTK_WIDGET(g_state.player_bar), FALSE);
    return;
  }
  GstBus* bus = gst_element_get_bus(g_state.playbin);
  g_state.bus_watch_id = gst_bus_add_watch(bus, on_bus_message, nullptr);
  gst_object_unref(bus);
  g_state.tick_source_id = g_timeout_add(1000, on_player_tick, nullptr);
}

void player_shutdown() {
  if (g_state.tick_source_id != 0) {
    g_source_remove(g_state.tick_source_id);
    g_state.tick_source_id = 0;
  }
  if (g_state.bus_watch_id != 0) {
    g_source_remove(g_state.bus_watch_id);
    g_state.bus_watch_id = 0;
  }
  if (g_state.playbin != nullptr) {
    gst_element_set_state(g_state.playbin, GST_STATE_NULL);
    gst_object_unref(g_state.playbin);
    g_state.playbin = nullptr;
  }
}

// ---------------------------------------------------------------------------
// MPRIS — media keys and the desktop player widget over D-Bus (the Linux
// counterpart of macOS MPRemoteCommandCenter and Windows SMTC). A minimal
// Root + Player pair: transport, position, rate and metadata. Pure
// host-side — nothing here touches the RVT1 contract.
// ---------------------------------------------------------------------------

guint mpris_owner_id{0};
GDBusConnection* mpris_connection{nullptr};
GDBusNodeInfo* mpris_introspection{nullptr};
std::string mpris_track_id = "org.mpris.MediaPlayer2.PodLens/track-0";

char const kMprisXml[] =
    "<node>\n"
    "  <interface name=\"org.mpris.MediaPlayer2\">\n"
    "    <method name=\"Raise\"/>\n"
    "    <method name=\"Quit\"/>\n"
    "    <property name=\"DesktopEntry\" type=\"s\" access=\"read\"/>\n"
    "    <property name=\"Identity\" type=\"s\" access=\"read\"/>\n"
    "    <property name=\"CanQuit\" type=\"b\" access=\"read\"/>\n"
    "    <property name=\"CanRaise\" type=\"b\" access=\"read\"/>\n"
    "    <property name=\"HasTrackList\" type=\"b\" access=\"read\"/>\n"
    "    <property name=\"SupportedUriSchemes\" type=\"as\" access=\"read\"/>\n"
    "  </interface>\n"
    "  <interface name=\"org.mpris.MediaPlayer2.Player\">\n"
    "    <method name=\"Play\"/>\n"
    "    <method name=\"Pause\"/>\n"
    "    <method name=\"PlayPause\"/>\n"
    "    <method name=\"Stop\"/>\n"
    "    <method name=\"Next\"/>\n"
    "    <method name=\"Previous\"/>\n"
    "    <method name=\"Seek\">\n"
    "      <arg name=\"Offset\" type=\"x\" direction=\"in\"/>\n"
    "    </method>\n"
    "    <method name=\"SetPosition\">\n"
    "      <arg name=\"TrackId\" type=\"o\" direction=\"in\"/>\n"
    "      <arg name=\"Position\" type=\"x\" direction=\"in\"/>\n"
    "    </method>\n"
    "    <property name=\"PlaybackStatus\" type=\"s\" access=\"read\"/>\n"
    "    <property name=\"LoopStatus\" type=\"s\" access=\"read\"/>\n"
    "    <property name=\"Rate\" type=\"d\" access=\"readwrite\"/>\n"
    "    <property name=\"Volume\" type=\"d\" access=\"read\"/>\n"
    "    <property name=\"Position\" type=\"x\" access=\"read\"/>\n"
    "    <property name=\"MinimumRate\" type=\"d\" access=\"read\"/>\n"
    "    <property name=\"MaximumRate\" type=\"d\" access=\"read\"/>\n"
    "    <property name=\"CanPlay\" type=\"b\" access=\"read\"/>\n"
    "    <property name=\"CanPause\" type=\"b\" access=\"read\"/>\n"
    "    <property name=\"CanSeek\" type=\"b\" access=\"read\"/>\n"
    "    <property name=\"CanControl\" type=\"b\" access=\"read\"/>\n"
    "    <property name=\"Metadata\" type=\"a{sv}\" access=\"read\"/>\n"
    "  </interface>\n"
    "</node>\n";

std::string mpris_title() {
  if (g_state.preview_active) return g_state.preview_title;
  if (auto const* episode = selected_episode()) return episode->title;
  return {};
}

std::string mpris_show() {
  if (g_state.preview_active) return g_state.preview_show;
  for (auto const& item : g_state.recents) {
    if (item.episode.id == g_state.selected_episode_id) return item.feed_title;
  }
  for (auto const& feed : g_state.feeds) {
    if (feed.id == g_state.selected_feed_id) return feed.title;
  }
  return {};
}

GVariant* mpris_metadata() {
  GVariantBuilder builder;
  g_variant_builder_init(&builder, G_VARIANT_TYPE("a{sv}"));
  g_variant_builder_add(&builder, "{sv}", "mpris:trackid",
                        g_variant_new_object_path(mpris_track_id.c_str()));
  if (g_state.media_duration > 0) {
    g_variant_builder_add(
        &builder, "{sv}", "mpris:length",
        g_variant_new_int64(static_cast<gint64>(
            static_cast<double>(g_state.media_duration) / GST_SECOND * 1e6)));
  }
  std::string const title = mpris_title();
  if (!title.empty()) {
    g_variant_builder_add(&builder, "{sv}", "xesam:title",
                          g_variant_new_string(title.c_str()));
  }
  std::string const show = mpris_show();
  if (!show.empty()) {
    GVariantBuilder artists;
    g_variant_builder_init(&artists, G_VARIANT_TYPE("as"));
    g_variant_builder_add(&artists, "s", show.c_str());
    g_variant_builder_add(&builder, "{sv}", "xesam:artist",
                          g_variant_builder_end(&artists));
  }
  return g_variant_builder_end(&builder);
}

GVariant* mpris_player_property(char const* property) {
  if (std::strcmp(property, "PlaybackStatus") == 0) {
    return g_variant_new_string(playbin_playing() ? "Playing" : "Paused");
  }
  if (std::strcmp(property, "LoopStatus") == 0) {
    return g_variant_new_string("None");
  }
  if (std::strcmp(property, "Rate") == 0) {
    return g_variant_new_double(g_state.playback_rate);
  }
  if (std::strcmp(property, "Volume") == 0) {
    return g_variant_new_double(1.0);
  }
  if (std::strcmp(property, "Position") == 0) {
    return g_variant_new_int64(
        static_cast<gint64>(playbin_position() * 1e6));
  }
  if (std::strcmp(property, "MinimumRate") == 0) {
    return g_variant_new_double(1.0);
  }
  if (std::strcmp(property, "MaximumRate") == 0) {
    return g_variant_new_double(3.0);
  }
  if (std::strcmp(property, "CanPlay") == 0 ||
      std::strcmp(property, "CanPause") == 0 ||
      std::strcmp(property, "CanSeek") == 0 ||
      std::strcmp(property, "CanControl") == 0) {
    return g_variant_new_boolean(TRUE);
  }
  if (std::strcmp(property, "Metadata") == 0) {
    return mpris_metadata();
  }
  return nullptr;
}

GVariant* mpris_root_property(char const* property) {
  if (std::strcmp(property, "DesktopEntry") == 0) {
    return g_variant_new_string("podlens");
  }
  if (std::strcmp(property, "Identity") == 0) {
    return g_variant_new_string("PodLens");
  }
  if (std::strcmp(property, "CanQuit") == 0 ||
      std::strcmp(property, "CanRaise") == 0 ||
      std::strcmp(property, "HasTrackList") == 0) {
    return g_variant_new_boolean(FALSE);
  }
  if (std::strcmp(property, "SupportedUriSchemes") == 0) {
    GVariantBuilder schemes;
    g_variant_builder_init(&schemes, G_VARIANT_TYPE("as"));
    return g_variant_builder_end(&schemes);
  }
  return nullptr;
}

// Tell watching players which Player properties changed.
void mpris_update(char const* const* properties) {
  if (mpris_connection == nullptr || properties == nullptr) return;
  GVariantBuilder changed;
  g_variant_builder_init(&changed, G_VARIANT_TYPE("a{sv}"));
  for (char const* const* prop = properties; *prop != nullptr; ++prop) {
    if (GVariant* value = mpris_player_property(*prop)) {
      g_variant_builder_add(&changed, "{sv}", *prop, value);
    }
  }
  GVariantBuilder invalidated;
  g_variant_builder_init(&invalidated, G_VARIANT_TYPE("as"));
  g_dbus_connection_emit_signal(
      mpris_connection, nullptr, "/org/mpris/MediaPlayer2",
      "org.freedesktop.DBus.Properties", "PropertiesChanged",
      g_variant_new("(sa{sv}as)", "org.mpris.MediaPlayer2.Player", &changed,
                    &invalidated),
      nullptr);
}

// Convenience wrappers for the common change sets.
void mpris_update() {
  static char const* const kStatusAndMetadata[] = {
      "PlaybackStatus", "Metadata", nullptr};
  mpris_update(kStatusAndMetadata);
}

void mpris_seeked(double seconds) {
  if (mpris_connection == nullptr) return;
  g_dbus_connection_emit_signal(
      mpris_connection, nullptr, "/org/mpris/MediaPlayer2",
      "org.mpris.MediaPlayer2.Player", "Seeked",
      g_variant_new("(x)", static_cast<gint64>(seconds * 1e6)), nullptr);
}

void mpris_method_call(GDBusConnection*, gchar const*, gchar const*,
                       gchar const*, gchar const* method_name,
                       GVariant* parameters, GDBusMethodInvocation* invocation,
                       gpointer) {
  if (std::strcmp(method_name, "Play") == 0 ||
      std::strcmp(method_name, "Pause") == 0 ||
      std::strcmp(method_name, "PlayPause") == 0) {
    dispatch_ui([] {
      // MPRIS has no notion of the host's arm-the-source flow: play with
      // nothing loaded starts the selected episode, like the play button.
      toggle_play_pause();
    });
    g_dbus_method_invocation_return_value(invocation, nullptr);
    return;
  }
  if (std::strcmp(method_name, "Stop") == 0) {
    dispatch_ui([] {
      stop_playback(true);
      mpris_update();
    });
    g_dbus_method_invocation_return_value(invocation, nullptr);
    return;
  }
  if (std::strcmp(method_name, "Seek") == 0) {
    gint64 offset = 0;
    g_variant_get(parameters, "(x)", &offset);
    dispatch_ui([offset] { skip_by(static_cast<double>(offset) / 1e6); });
    g_dbus_method_invocation_return_value(invocation, nullptr);
    return;
  }
  if (std::strcmp(method_name, "SetPosition") == 0) {
    gint64 position = 0;
    g_variant_get(parameters, "(ox)", nullptr, &position);
    dispatch_ui([position] {
      apply_seek(static_cast<double>(position) / 1e6);
      mpris_seeked(static_cast<double>(position) / 1e6);
    });
    g_dbus_method_invocation_return_value(invocation, nullptr);
    return;
  }
  // Next/Previous/Raise/Quit: unsupported on purpose — no queue, no
  // window-raising contract; reply with an error so callers move on.
  g_dbus_method_invocation_return_error(
      invocation, G_DBUS_ERROR, G_DBUS_ERROR_NOT_SUPPORTED,
      "PodLens does not support %s", method_name);
}

GVariant* mpris_get_property(GDBusConnection*, gchar const*, gchar const*,
                             gchar const* interface_name,
                             gchar const* property_name, GError** error,
                             gpointer) {
  GVariant* value = std::strcmp(interface_name, "org.mpris.MediaPlayer2.Player") == 0
                        ? mpris_player_property(property_name)
                        : mpris_root_property(property_name);
  if (value == nullptr) {
    g_set_error(error, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_PROPERTY,
                "no such property: %s", property_name);
  }
  return value;
}

gboolean mpris_set_property(GDBusConnection*, gchar const*, gchar const*,
                            gchar const*, gchar const*, GVariant* value,
                            GError** error, gpointer) {
  if (std::strcmp(g_variant_get_type_string(value), "d") == 0) {
    double rate = g_variant_get_double(value);
    dispatch_ui([rate] {
      if (rate > 0) apply_playback_rate(rate);
    });
    return TRUE;
  }
  g_set_error(error, G_DBUS_ERROR, G_DBUS_ERROR_NOT_SUPPORTED,
              "property is read-only");
  return FALSE;
}

void mpris_start() {
  mpris_introspection = g_dbus_node_info_new_for_xml(kMprisXml, nullptr);
  if (mpris_introspection == nullptr) return;
  static GDBusInterfaceVTable const root_vtable = {
      mpris_method_call, mpris_get_property, nullptr};
  static GDBusInterfaceVTable const player_vtable = {
      mpris_method_call, mpris_get_property, mpris_set_property};
  mpris_owner_id = g_bus_own_name(
      G_BUS_TYPE_SESSION, "org.mpris.MediaPlayer2.PodLens",
      G_BUS_NAME_OWNER_FLAGS_NONE,
      +[](GDBusConnection* connection, gchar const*, gpointer) {
        mpris_connection = connection;
        GError* error = nullptr;
        g_dbus_connection_register_object(
            connection, "/org/mpris/MediaPlayer2",
            mpris_introspection->interfaces[0], &root_vtable, nullptr, nullptr,
            &error);
        if (error != nullptr) g_error_free(error);
        error = nullptr;
        g_dbus_connection_register_object(
            connection, "/org/mpris/MediaPlayer2",
            mpris_introspection->interfaces[1], &player_vtable, nullptr,
            nullptr, &error);
        if (error != nullptr) g_error_free(error);
      },
      nullptr, nullptr, nullptr, nullptr);
}

void load_chapters() {
  g_state.chapters.clear();
  bool const has = false;
  gtk_widget_set_visible(GTK_WIDGET(g_state.chapter_back_button), has);
  gtk_widget_set_visible(GTK_WIDGET(g_state.chapter_forward_button), has);
  if (g_state.api == nullptr || g_state.selected_episode_id.empty()) return;
  auto const episode_id = g_state.selected_episode_id;
  (void)g_state.api->episode_chapters_async(
      episode_id,
      [episode_id](
          rivet_app::Result<std::vector<std::vector<std::string>>> result) {
        on_result(std::move(result),
                  [episode_id](std::vector<
                               std::vector<std::string>> const& rows) {
                    if (g_state.selected_episode_id != episode_id) return;
                    g_state.chapters.clear();
                    for (auto const& row : rows) {
                      if (row.size() < 2) continue;
                      g_state.chapters.push_back(
                          {std::atof(row[0].c_str()), row[1]});
                    }
                    bool const has = !g_state.chapters.empty();
                    gtk_widget_set_visible(
                        GTK_WIDGET(g_state.chapter_back_button), has);
                    gtk_widget_set_visible(
                        GTK_WIDGET(g_state.chapter_forward_button), has);
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
      tr("dialog.understand"), g_state.window,
      static_cast<GtkDialogFlags>(GTK_DIALOG_MODAL |
                                  GTK_DIALOG_DESTROY_WITH_PARENT),
      tr("dialog.cancel"), GTK_RESPONSE_CANCEL, tr("action.start"),
      GTK_RESPONSE_ACCEPT, nullptr);
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

// ---------------------------------------------------------------------------
// Episode jobs — download / transcribe / translate / summarize
// ---------------------------------------------------------------------------

void start_job(std::string const& kind, std::string const& episode_id) {
  if (g_state.api == nullptr || episode_id.empty()) return;
  set_status("Job started — the list refreshes when it finishes");
  auto const started = [episode_id](std::string const& job_id) {
    g_state.active_job_id = job_id;
    g_state.active_job_episode = episode_id;
    set_job_ui("job 0%", 0.0, true);
  };
  if (kind == "download") {
    (void)g_state.api->episode_download_async(
        episode_id,
        [started](rivet_app::Result<std::string> result) {
          on_result(std::move(result), started);
        });
  } else if (kind == "transcribe") {
    (void)g_state.api->episode_transcribe_async(
        episode_id,
        [started](rivet_app::Result<std::string> result) {
          on_result(std::move(result), started);
        });
  } else if (kind == "translate") {
    (void)g_state.api->episode_translate_async(
        episode_id,
        [started](rivet_app::Result<std::string> result) {
          on_result(std::move(result), started);
        });
  } else if (kind == "summarize") {
    (void)g_state.api->episode_summarize_async(
        episode_id,
        [started](rivet_app::Result<std::string> result) {
          on_result(std::move(result), started);
        });
  }
}

void on_mark_clicked(GtkButton*, gpointer) {
  auto const* episode = selected_episode();
  if (episode == nullptr || g_state.api == nullptr) return;
  auto const id = episode->id;
  auto const done = episode->done ? "0" : "1";
  (void)g_state.api->episode_set_done_async(
      id, done, [](rivet_app::Result<bool>) {
        dispatch_ui([] {
          reload_feeds();
          reload_recent();
          reload_episodes();
        });
      });
}

void on_remove_audio_clicked(GtkButton*, gpointer) {
  auto const episode_id = g_state.selected_episode_id;
  if (episode_id.empty() || g_state.api == nullptr) return;
  (void)g_state.api->episode_remove_audio_async(
      episode_id, [](rivet_app::Result<bool> result) {
        on_result(std::move(result), [](bool) {
          set_status("Local audio deleted");
          reload_episodes();
        });
      });
}

void on_play_clicked(GtkButton*, gpointer) { toggle_play_pause(); }

// Sleep timer: pause playback when the deadline lands. GTK timeouts run on
// the main loop, so the callback can touch widgets directly.
void on_sleep_activated(GSimpleAction*, GVariant* value, gpointer) {
  int const minutes = g_variant_get_int32(value);
  if (g_state.sleep_source_id != 0) {
    g_source_remove(g_state.sleep_source_id);
    g_state.sleep_source_id = 0;
  }
  g_state.sleep_minutes = minutes;
  if (minutes <= 0) {
    set_status(tr("sleep.off"));
    return;
  }
  g_state.sleep_source_id = g_timeout_add_seconds(
      minutes * 60,
      +[](gpointer) -> int {
        g_state.sleep_source_id = 0;
        g_state.sleep_minutes = 0;
        if (g_state.playbin != nullptr && playbin_playing()) {
          gst_element_set_state(g_state.playbin, GST_STATE_PAUSED);
          set_play_button_glyph(false);
        }
        set_status(tr("sleep.fired"));
        return G_SOURCE_REMOVE;
      },
      nullptr);
  char label[64];
  std::snprintf(label, sizeof(label), tr("sleep.minutes"), minutes);
  set_status(label);
}

void on_skip_back_clicked(GtkButton*, gpointer) { skip_by(-15.0); }

void on_skip_forward_clicked(GtkButton*, gpointer) { skip_by(30.0); }

void on_chapter_back_clicked(GtkButton*, gpointer) { skip_chapter(-1); }

void on_chapter_forward_clicked(GtkButton*, gpointer) { skip_chapter(1); }

void on_seek_value_changed(GtkRange*, gpointer) {
  if (g_state.user_seeking) return;
  apply_seek(gtk_range_get_value(GTK_RANGE(g_state.seek_scale)));
}

void on_seek_pressed(GtkGestureClick*, int, double, double, gpointer) {
  g_state.user_seeking = true;
}

void on_seek_released(GtkGestureClick*, int, double, double, gpointer) {
  if (!g_state.user_seeking) return;
  g_state.user_seeking = false;
  apply_seek(gtk_range_get_value(GTK_RANGE(g_state.seek_scale)));
}

void on_rate_changed(GtkComboBox* combo, gpointer) {
  auto* text = gtk_combo_box_text_get_active_text(
      GTK_COMBO_BOX_TEXT(combo));
  if (text == nullptr) return;
  double const rate = std::atof(text);
  g_free(text);
  if (rate > 0 && g_state.playbin != nullptr) {
    apply_playback_rate(rate);
  }
}

// ---------------------------------------------------------------------------
// Actions — add feed / refresh all / remove feed / mark done / about
// ---------------------------------------------------------------------------

void add_feed_with_url(std::string const& url) {
  if (g_state.api == nullptr || url.empty()) return;
  (void)g_state.api->feed_add_async(
      url, [](rivet_app::Result<std::string> result) {
        on_result(std::move(result), [](std::string const&) {
          set_status(tr("status.feed_added"));
          reload_feeds();
        });
      });
}

void on_add_feed_activated(GSimpleAction*, GVariant*, gpointer) {
  if (g_state.api == nullptr) return;
  auto* dialog = gtk_dialog_new_with_buttons(
      tr("menu.add_feed"), g_state.window,
      static_cast<GtkDialogFlags>(GTK_DIALOG_MODAL |
                                  GTK_DIALOG_DESTROY_WITH_PARENT),
      tr("dialog.cancel"), GTK_RESPONSE_CANCEL, tr("action.add"),
      GTK_RESPONSE_ACCEPT, nullptr);
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

void on_feed_refresh_activated(GSimpleAction*, GVariant* parameter,
                               gpointer) {
  if (g_state.api == nullptr || parameter == nullptr) return;
  std::string const feed_id(g_variant_get_string(parameter, nullptr));
  set_status("Refreshing…");
  (void)g_state.api->feed_refresh_async(
      feed_id, [](rivet_app::Result<std::int64_t> result) {
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
  gtk_about_dialog_set_comments(GTK_ABOUT_DIALOG(dialog),
                                tr("about.tagline"));
  gtk_about_dialog_set_license_type(GTK_ABOUT_DIALOG(dialog),
                                    GTK_LICENSE_AGPL_3_0);
  gtk_about_dialog_set_website(GTK_ABOUT_DIALOG(dialog),
                               "https://podlens.jrtx.site");
  gtk_about_dialog_set_website_label(GTK_ABOUT_DIALOG(dialog),
                                     tr("about.website"));
  static char const* kIssueLinks[] = {
      "github.com/turinglambdaai/podlens/issues", nullptr};
  gtk_about_dialog_add_credit_section(GTK_ABOUT_DIALOG(dialog),
                                      tr("about.issues"), kIssueLinks);
  gtk_window_set_transient_for(GTK_WINDOW(dialog), g_state.window);
  gtk_window_set_modal(GTK_WINDOW(dialog), TRUE);
  gtk_window_present(GTK_WINDOW(dialog));
}

// ---------------------------------------------------------------------------
// Settings — one labeled entry per backend setting, Save pushes them all
// ---------------------------------------------------------------------------

using SettingsFields = std::vector<std::pair<std::string, GtkEntry*>>;
using StringPairs = std::vector<std::pair<std::string, std::string>>;

// Persist the dialog's rows one by one through the async API. Every RPC is
// issued on the UI thread and each completion hops back through dispatch_ui,
// so nothing here can race the runtime teardown at shutdown the way a
// detached thread holding a synchronous settings_set call could.
void save_settings_pairs(std::shared_ptr<StringPairs> pairs, size_t index) {
  if (pairs == nullptr || g_state.api == nullptr) return;
  if (index >= pairs->size()) {
    set_status(tr("status.saved"));
    return;
  }
  auto const& [key, value] = (*pairs)[index];
  (void)g_state.api->settings_set_async(
      key, value,
      [pairs = std::move(pairs), index](rivet_app::Result<bool> result) {
        dispatch_ui([pairs = std::move(pairs), index,
                     result = std::move(result)]() mutable {
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
          save_settings_pairs(pairs, index + 1);
        });
      });
}

void open_url(char const* url) {
  gtk_show_uri(g_state.window, url, GDK_CURRENT_TIME);
}

void on_settings_activated(GSimpleAction*, GVariant*, gpointer) {
  if (g_state.api == nullptr) return;
  (void)g_state.api->settings_list_async(
      [](rivet_app::Result<std::vector<std::vector<std::string>>> result) {
        on_result(std::move(result),
                  [](std::vector<std::vector<std::string>> const& rows) {
                    auto* dialog = gtk_dialog_new_with_buttons(
                        tr("dialog.settings_title"), g_state.window,
                        static_cast<GtkDialogFlags>(
                            GTK_DIALOG_MODAL |
                            GTK_DIALOG_DESTROY_WITH_PARENT),
                        tr("dialog.cancel"), GTK_RESPONSE_CANCEL,
                        tr("action.save"), GTK_RESPONSE_ACCEPT, nullptr);
                    auto* area = gtk_dialog_get_content_area(
                        GTK_DIALOG(dialog));
                    auto* panel = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
                    gtk_widget_set_margin_top(panel, 12);
                    gtk_widget_set_margin_bottom(panel, 6);
                    gtk_widget_set_margin_start(panel, 14);
                    gtk_widget_set_margin_end(panel, 14);

                    // one labeled GtkEntry per setting; Save pushes every
                    // row through settings-set (Windows parity)
                    auto* fields = new SettingsFields();
                    for (auto const& row : rows) {
                      if (row.size() < 2) continue;
                      auto* key_label =
                          GTK_LABEL(gtk_label_new(row[0].c_str()));
                      gtk_label_set_xalign(key_label, 0.0f);
                      gtk_widget_add_css_class(GTK_WIDGET(key_label),
                                               "pl-title");
                      auto* entry =
                          GTK_ENTRY(gtk_entry_new());
                      gtk_accessible_update_property(
                          GTK_ACCESSIBLE(entry), GTK_ACCESSIBLE_PROPERTY_LABEL,
                          row[0].c_str(), -1);
                      gtk_editable_set_text(GTK_EDITABLE(entry),
                                            row[1].c_str());
                      gtk_box_append(GTK_BOX(panel),
                                     GTK_WIDGET(key_label));
                      gtk_box_append(GTK_BOX(panel),
                                     GTK_WIDGET(entry));
                      if (row.size() >= 3 && !row[2].empty()) {
                        auto* description = GTK_LABEL(
                            gtk_label_new(row[2].c_str()));
                        gtk_label_set_wrap(description, TRUE);
                        gtk_label_set_xalign(description, 0.0f);
                        gtk_widget_add_css_class(GTK_WIDGET(description),
                                                 "pl-meta");
                        gtk_box_append(GTK_BOX(panel),
                                       GTK_WIDGET(description));
                      }
                      fields->emplace_back(row[0], entry);
                    }

                    auto* hint = GTK_LABEL(
                        gtk_label_new(tr("status.settings_hint")));
                    gtk_label_set_wrap(hint, TRUE);
                    gtk_label_set_xalign(hint, 0.0f);
                    gtk_widget_add_css_class(GTK_WIDGET(hint), "pl-meta");
                    gtk_box_append(GTK_BOX(panel), GTK_WIDGET(hint));

                    // About block (mac/win settings parity): version,
                    // license and the support links, so a bug report starts
                    // one click away.
                    auto* about =
                        gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
                    auto* about_version = GTK_LABEL(gtk_label_new(
                        ("PodLens v" + std::string(rivet_app::kVersion))
                            .c_str()));
                    gtk_label_set_xalign(about_version, 0.0f);
                    gtk_widget_add_css_class(GTK_WIDGET(about_version),
                                             "pl-title");
                    gtk_box_append(GTK_BOX(about),
                                   GTK_WIDGET(about_version));
                    auto* license = GTK_LABEL(gtk_label_new(
                        "Released under the GNU AGPL-3.0"));
                    gtk_label_set_wrap(license, TRUE);
                    gtk_label_set_xalign(license, 0.0f);
                    gtk_widget_add_css_class(GTK_WIDGET(license),
                                             "pl-meta");
                    gtk_box_append(GTK_BOX(about), GTK_WIDGET(license));
                    auto* website = gtk_link_button_new_with_label(
                        "https://podlens.jrtx.site", "Website");
                    gtk_widget_set_has_tooltip(website, FALSE);
                    gtk_box_append(GTK_BOX(about), website);
                    auto* issue = gtk_link_button_new_with_label(
                        "https://github.com/turinglambdaai/podlens/issues",
                        "Report an Issue");
                    gtk_widget_set_has_tooltip(issue, FALSE);
                    gtk_box_append(GTK_BOX(about), issue);
                    gtk_box_append(GTK_BOX(panel), about);

                    auto* scroll = gtk_scrolled_window_new();
                    gtk_scrolled_window_set_policy(
                        GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER,
                        GTK_POLICY_AUTOMATIC);
                    gtk_scrolled_window_set_child(
                        GTK_SCROLLED_WINDOW(scroll), panel);
                    gtk_widget_set_size_request(scroll, -1, 420);
                    gtk_box_append(GTK_BOX(area), scroll);

                    carry_data<SettingsFields*>(G_OBJECT(dialog),
                                                "settings-fields", fields);
                    g_signal_connect(
                        dialog, "response",
                        G_CALLBACK(+[](GtkDialog* dialog, gint response,
                                       gpointer) {
                          auto* fields = *carried_data<SettingsFields*>(
                              G_OBJECT(dialog), "settings-fields");
                          gtk_window_destroy(GTK_WINDOW(dialog));
                          if (response != GTK_RESPONSE_ACCEPT) {
                            delete fields;
                            return;
                          }
                          // The entries must be read on the UI thread before
                          // the dialog dies; the writes ride the async API
                          // (save_settings_pairs) so no detached thread can
                          // outlive the backend at shutdown.
                          auto pairs = std::make_shared<StringPairs>();
                          for (auto const& [key, entry] : *fields) {
                            pairs->emplace_back(
                                key,
                                gtk_editable_get_text(GTK_EDITABLE(entry)));
                          }
                          delete fields;
                          save_settings_pairs(pairs, 0);
                        }),
                        nullptr);
                    gtk_window_present(GTK_WINDOW(dialog));
                  });
      });
}

// ---------------------------------------------------------------------------
// Updates — check → consent → download → install handoff (docs/UPDATE.md).
// The backend verifies and downloads the signed artifact; the host owns the
// dialogs, the silent 4-hour throttle and installation per install shape.
// ---------------------------------------------------------------------------

void record_update_check() {
  if (g_state.api == nullptr) return;
  (void)g_state.api->settings_set_async(
      "last-update-check", std::to_string(std::time(nullptr)),
      [](rivet_app::Result<bool>) {
        // Throttle stamping is best-effort.
      });
}

void show_update_dialog(std::string const& title, std::string const& body,
                        std::string confirm_label, std::string cancel_label,
                        std::function<void()> on_confirm) {
  auto* dialog = gtk_dialog_new_with_buttons(
      title.c_str(), g_state.window,
      static_cast<GtkDialogFlags>(GTK_DIALOG_MODAL |
                                  GTK_DIALOG_DESTROY_WITH_PARENT),
      cancel_label.c_str(), GTK_RESPONSE_CANCEL,
      confirm_label.empty() ? nullptr : confirm_label.c_str(),
      GTK_RESPONSE_ACCEPT, nullptr);
  auto* area = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
  auto* text = GTK_LABEL(gtk_label_new(body.c_str()));
  gtk_label_set_wrap(text, TRUE);
  // Error text must be copyable — a bug report starts from this string.
  gtk_label_set_selectable(text, TRUE);
  gtk_label_set_max_width_chars(text, 56);
  gtk_widget_set_margin_top(GTK_WIDGET(text), 12);
  gtk_widget_set_margin_bottom(GTK_WIDGET(text), 6);
  gtk_widget_set_margin_start(GTK_WIDGET(text), 14);
  gtk_widget_set_margin_end(GTK_WIDGET(text), 14);
  gtk_box_append(GTK_BOX(area), GTK_WIDGET(text));
  if (on_confirm) {
    carry_data<std::function<void()>>(G_OBJECT(dialog), "on-confirm",
                                      std::move(on_confirm));
  }
  g_signal_connect(
      dialog, "response",
      G_CALLBACK(+[](GtkDialog* dialog, gint response, gpointer) {
        std::function<void()> confirm;
        if (auto const* carried = carried_data<std::function<void()>>(
                G_OBJECT(dialog), "on-confirm")) {
          confirm = *carried;
        }
        gtk_window_destroy(GTK_WINDOW(dialog));
        if (response == GTK_RESPONSE_ACCEPT && confirm) {
          confirm();
        }
      }),
      nullptr);
  gtk_window_present(GTK_WINDOW(dialog));
}

void show_update_error(std::string const& message) {
  show_update_dialog("PodLens",
                     std::string(tr("menu.check_updates_failed")) + message,
                     "", "Close", nullptr);
}

void open_releases_page() {
  if (g_state.api == nullptr) return;
  (void)g_state.api->open_releases_async([](rivet_app::Result<void>) {});
}

void stop_update_poll() {
  if (g_state.update_poll_source != 0) {
    g_source_remove(g_state.update_poll_source);
    g_state.update_poll_source = 0;
  }
}

void close_update_progress() {
  if (g_state.update_progress_dialog != nullptr) {
    gtk_window_destroy(g_state.update_progress_dialog);
    g_state.update_progress_dialog = nullptr;
    g_state.update_progress_bar = nullptr;
    g_state.update_progress_label = nullptr;
  }
}

// AppImage: the backend already swapped the sibling .AppImage over the
// running image atomically, so the only step left is the user's consent to
// re-exec. Declining keeps the current process; the next launch is the new
// version regardless.
void show_appimage_restart_dialog() {
  show_update_dialog(
      "Update downloaded",
      "The update is in place — restart PodLens to switch to the new "
      "version.",
      "Restart and update", "Cancel", [] {
        char const* self = getenv("APPIMAGE");
        if (self == nullptr || self[0] == '\0') return;
        execl(self, self, static_cast<char*>(nullptr));
        // execl only returns on failure — surface it, the old image keeps
        // running either way.
        show_update_error(std::string("re-exec failed: ") +
                          std::strerror(errno));
      });
}

// deb/rpm installs: the download cannot self-swap a system-managed prefix,
// so the dialog explains the upgrade path and still opens the file's folder
// for whoever wants the archive itself.
void show_package_manager_dialog(std::string const& path) {
  std::string body =
      "This copy came from the system package manager — use it to upgrade "
      "PodLens.";
  if (!path.empty()) {
    body += "\n\n" + path;
  }
  show_update_dialog("Update downloaded", body, "", "Close", nullptr);
}

// Downloaded: hand off the verified tar.gz. Installation stays manual, so
// the dialog explains the step and points at the file's folder; nothing is
// ever executed or moved by the host.
void show_downloaded_dialog(std::string const& path) {
  std::string body =
      "To install, extract the downloaded archive over your current "
      "PodLens folder.";
  if (!path.empty()) {
    body += "\n\n" + path;
  }
  std::string folder;
  if (!path.empty()) {
    auto const slash = path.find_last_of('/');
    if (slash != std::string::npos) folder = path.substr(0, slash);
  }
  g_state.update_downloaded_path = path;
  show_update_dialog("Update downloaded", body,
                     folder.empty() ? "" : "Open Folder", "Close",
                     [folder] {
                       GFile* dir = g_file_new_for_path(folder.c_str());
                       gchar* uri = g_file_get_uri(dir);
                       open_url(uri);
                       g_free(uri);
                       g_object_unref(dir);
                     });
}

int on_update_poll_tick(gpointer) {
  if (g_state.api == nullptr || g_state.update_progress_dialog == nullptr) {
    g_state.update_poll_source = 0;
    return G_SOURCE_REMOVE;
  }
  (void)g_state.api->update_state_async(
      [](rivet_app::Result<rivet_app::UpdateState> result) {
        dispatch_ui([result = std::move(result)]() mutable {
          if (g_state.update_progress_dialog == nullptr) return;  // detached
          if (!result.succeeded()) return;  // keep polling through hiccups
          rivet_app::UpdateState const& state = result.get();
          auto const percent = std::clamp<std::int64_t>(state.percent, 0, 100);
          if (g_state.update_progress_bar != nullptr) {
            gtk_progress_bar_set_fraction(g_state.update_progress_bar,
                                          static_cast<double>(percent) /
                                              100.0);
          }
          if (g_state.update_progress_label != nullptr) {
            gtk_label_set_text(
                g_state.update_progress_label,
                ("Downloading the new version… " + std::to_string(percent) +
                 "%")
                    .c_str());
          }
          if (state.phase != "downloaded" && state.phase != "error") {
            return;
          }
          std::string const downloaded = state.downloaded_path.value_or("");
          std::string const failure = state.message.value_or("");
          stop_update_poll();
          close_update_progress();
          g_state.update_download_in_flight = false;
          if (state.phase == "error") {
            show_update_error(failure.empty() ? std::string("unknown failure")
                                              : failure);
            return;
          }
          auto const shape = install_shape();
          if (shape == "appimage") {
            show_appimage_restart_dialog();
            return;
          }
          if (shape == "package") {
            show_package_manager_dialog(downloaded);
            return;
          }
          show_downloaded_dialog(downloaded);
        });
      });
  return G_SOURCE_CONTINUE;
}

void start_update_download() {
  if (g_state.api == nullptr || g_state.update_download_in_flight) return;
  g_state.update_download_in_flight = true;

  auto* dialog = gtk_dialog_new_with_buttons(
      "Downloading the new version…", g_state.window,
      static_cast<GtkDialogFlags>(GTK_DIALOG_MODAL |
                                  GTK_DIALOG_DESTROY_WITH_PARENT),
      "Cancel", GTK_RESPONSE_CANCEL, nullptr);
  auto* area = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
  auto* content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
  gtk_widget_set_margin_top(content, 12);
  gtk_widget_set_margin_bottom(content, 8);
  gtk_widget_set_margin_start(content, 14);
  gtk_widget_set_margin_end(content, 14);
  gtk_box_append(GTK_BOX(area), content);
  auto* bar = gtk_progress_bar_new();
  gtk_widget_set_size_request(bar, 320, -1);
  gtk_box_append(GTK_BOX(content), bar);
  auto* label =
      GTK_LABEL(gtk_label_new("Downloading the new version… 0%"));
  gtk_box_append(GTK_BOX(content), GTK_WIDGET(label));

  // Canceling only detaches the progress UI — the backend keeps downloading
  // (family parity: there is no cancel RPC).
  g_signal_connect(
      dialog, "response",
      G_CALLBACK(+[](GtkDialog* dialog, gint, gpointer) {
        stop_update_poll();
        g_state.update_progress_dialog = nullptr;
        g_state.update_progress_bar = nullptr;
        g_state.update_progress_label = nullptr;
        g_state.update_download_in_flight = false;
        gtk_window_destroy(GTK_WINDOW(dialog));
      }),
      nullptr);
  g_signal_connect(
      dialog, "destroy",
      G_CALLBACK(+[](GtkWidget*, gpointer) {
        g_state.update_progress_dialog = nullptr;
        g_state.update_progress_bar = nullptr;
        g_state.update_progress_label = nullptr;
        stop_update_poll();
      }),
      nullptr);
  gtk_window_present(GTK_WINDOW(dialog));
  g_state.update_progress_dialog = GTK_WINDOW(dialog);
  g_state.update_progress_bar = GTK_PROGRESS_BAR(bar);
  g_state.update_progress_label = label;
  g_state.update_poll_source =
      g_timeout_add(400, on_update_poll_tick, nullptr);

  (void)g_state.api->start_download_async(
      [](rivet_app::Result<void> result) {
        dispatch_ui([result = std::move(result)]() mutable {
          if (g_state.update_progress_dialog == nullptr) return;
          if (result.succeeded()) return;  // the poll owns the rest
          // The download never started; stop staring at a 0% bar.
          stop_update_poll();
          close_update_progress();
          g_state.update_download_in_flight = false;
          std::string message = "unknown failure";
          try {
            std::rethrow_exception(result.error);
          } catch (std::exception const& e) {
            message = e.what();
          } catch (...) {
          }
          show_update_error(message);
        });
      });
}

// Check for updates. `silent` keeps every outcome except an available
// update invisible (startup auto-check — never nag); a manual check reports
// everything. Any settled check records `last-update-check`, so the next
// launch's silent attempt stays inside the 4-hour throttle.
void check_for_updates(bool silent) {
  if (g_state.api == nullptr || g_state.update_check_in_flight ||
      g_state.update_download_in_flight) {
    return;
  }
  if (is_dev_copy()) {
    if (!silent) {
      show_update_dialog("PodLens", tr("update.dev"), tr("update.open_releases"),
                         tr("dialog.cancel"), open_releases_page);
    }
    return;
  }
  if (install_shape() == "package") {
    if (!silent) {
      // deb/rpm upgrade through the package manager; never download or exec.
      show_update_dialog("PodLens", tr("update.package"),
                         tr("update.open_releases"), tr("dialog.cancel"),
                         open_releases_page);
    }
    return;
  }
  g_state.update_check_in_flight = true;
  if (!silent) {
    set_status(tr("update.checking"));
  }
  (void)g_state.api->update_check_async(
      [silent](rivet_app::Result<rivet_app::UpdateCheck> result) {
        dispatch_ui([result = std::move(result), silent]() mutable {
          g_state.update_check_in_flight = false;
          // Stamp the throttle on every terminal path (docs/UPDATE.md).
          record_update_check();
          if (!result.succeeded()) {
            // Transport-level failure: the backend is going away, so do not
            // pretend a check happened.
            if (silent) return;
            std::string message = "unknown failure";
            try {
              std::rethrow_exception(result.error);
            } catch (std::exception const& e) {
              message = e.what();
            } catch (...) {
            }
            show_update_error(message);
            return;
          }
          rivet_app::UpdateCheck const& check = result.get();
          if (check.status == "available") {
            std::string const version =
                check.available_version.value_or(check.current_version);
            char body[160];
            std::snprintf(body, sizeof(body), tr("update.available_body"),
                          version.c_str());
            show_update_dialog(tr("update.available_title"), body,
                               tr("update.download"), tr("dialog.cancel"),
                               start_update_download);
            return;
          }
          if (silent) return;  // auto-check only speaks when there is news
          if (check.status == "error") {
            show_update_error(check.error.value_or("unknown failure"));
            return;
          }
          show_update_dialog("PodLens", tr("menu.check_updates_done"), "",
                             tr("action.close"),
                             nullptr);
        });
      });
}

void on_check_updates_activated(GSimpleAction*, GVariant*, gpointer) {
  check_for_updates(/*silent=*/false);
}

// Silent launch check: once per process, 3 s after the backend is up, gated
// by the settings (check-updates-enabled) and the 4-hour throttle. A failed
// throttle read counts as "never checked" — silent mode ignores errors.
gboolean on_update_auto_check(gpointer) {
  if (g_state.api == nullptr ||
      g_state.shutting_down.load(std::memory_order_acquire)) {
    return G_SOURCE_REMOVE;
  }
  if (is_dev_copy()) return G_SOURCE_REMOVE;
  if (install_shape() == "package") return G_SOURCE_REMOVE;
  if (!g_state.check_updates_enabled) return G_SOURCE_REMOVE;
  auto const now = static_cast<long long>(std::time(nullptr));
  if (now - g_state.last_update_check < 4 * 60 * 60) return G_SOURCE_REMOVE;
  check_for_updates(/*silent=*/true);
  return G_SOURCE_REMOVE;
}

// Reads the two update-related settings, then schedules the silent check.
void start_auto_update_check() {
  if (g_state.api == nullptr) return;
  (void)g_state.api->settings_list_async(
      [](rivet_app::Result<std::vector<std::vector<std::string>>> result) {
        on_result(std::move(result),
                  [](std::vector<std::vector<std::string>> const& rows) {
                    for (auto const& row : rows) {
                      if (row.size() < 2) continue;
                      if (row[0] == "check-updates-enabled") {
                        g_state.check_updates_enabled = row[1] != "false";
                      } else if (row[0] == "last-update-check") {
                        try {
                          g_state.last_update_check = std::stoll(row[1]);
                        } catch (...) {
                          g_state.last_update_check = 0;  // unset/malformed
                        }
                      }
                    }
                    g_timeout_add_seconds(3, on_update_auto_check, nullptr);
                  });
      });
}

// ---------------------------------------------------------------------------
// Notifications — backend `notify` events become freedesktop notifications
// ---------------------------------------------------------------------------

void post_notification(std::string const& body) {
  if (g_state.notifications_disabled) return;
  try {
    if (!rivet::system::Notifications::available()) {
      g_state.notifications_disabled = true;
      return;
    }
    rivet::system::Notifications::Notify("PodLens", "podlens-notify",
                                         "PodLens", body);
  } catch (...) {
    // A missing notification service must never take the host down.
    g_state.notifications_disabled = true;
  }
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
  g_menu_append(menu, tr("menu.add_feed"), "app.add-feed");
  g_menu_append(menu, tr("menu.refresh_all"), "app.refresh-all");
  auto* settings_menu = g_menu_new();
  g_menu_append(settings_menu, tr("menu.settings"), "app.settings");
  g_menu_append_submenu(menu, tr("menu.options"), G_MENU_MODEL(settings_menu));
  g_object_unref(settings_menu);
  // Family menu convention: Check for Updates… and About are a pair in the
  // Help menu.
  auto* help = g_menu_new();
  g_menu_append(help, tr("menu.check_updates"), "app.check-updates");
  g_menu_append(help, tr("menu.about"), "app.about");
  g_menu_append_submenu(menu, tr("menu.help"), G_MENU_MODEL(help));
  g_object_unref(help);

  auto* menubutton = gtk_menu_button_new();
  gtk_accessible_update_property(
      GTK_ACCESSIBLE(menubutton), GTK_ACCESSIBLE_PROPERTY_LABEL,
      tr("menu.main"), -1);
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
              reload_recent();
              if (g_state.current_page == "episodes") reload_episodes();
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
            dispatch_ui([text] {
              set_status(text.c_str());
              post_notification(text);
            });
          } else if (auto const* open =
                         std::get_if<rivet_app::Open_urlEvent>(&event)) {
            std::string const url = open->value;
            dispatch_ui([url] {
              gtk_show_uri(g_state.window, url.c_str(), GDK_CURRENT_TIME);
            });
          } else if (auto const* update =
                         std::get_if<rivet_app::Update_availableEvent>(
                             &event)) {
            std::string const version = update->value;
            dispatch_ui([version] {
              set_status(("Update available: " + version).c_str());
            });
          }
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

  set_status(tr("status.ready"));
  reload_feeds();
  start_auto_update_check();
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

void on_shutdown(GApplication*, gpointer) {
  // Exactly-once teardown: the GTK shutdown signal and the signal hook can
  // both reach this; whichever lands first does the work.
  if (g_state.teardown_started.exchange(true, std::memory_order_acq_rel)) {
    return;
  }
  g_state.shutting_down.store(true, std::memory_order_release);
  // Update flow first: its poll must not fire an RPC against a stopped
  // backend, and the progress dialog is modal to the window going away.
  stop_update_poll();
  close_update_progress();
  player_shutdown();
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
  auto* add_feed =
      make_button_with_icon("list-add-symbolic", tr("action.add_feed"));
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

  // The episodes page wraps the caption + the list stack (existing shell).
  auto* episodes_page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_box_append(GTK_BOX(episodes_page), episode_caption);
  gtk_box_append(GTK_BOX(episodes_page), list_stack);

  // Library pages: home / discover / recent / shows / downloaded, each a
  // scrolled column with its own caption — the Podcasts page model.
  auto* page_stack = gtk_stack_new();
  gtk_stack_add_named(GTK_STACK(page_stack), episodes_page, "episodes");

  {
    auto* home_page = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(home_page),
                                   GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    auto* home_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_margin_top(home_box, 6);
    gtk_widget_set_margin_start(home_box, 14);
    gtk_widget_set_margin_end(home_box, 14);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(home_page), home_box);
    gtk_stack_add_named(GTK_STACK(page_stack), home_page, "home");
    g_state.home_list = GTK_BOX(home_box);
  }
  {
    auto* recent_page = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(recent_page),
                                   GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    auto* recent_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_margin_top(recent_box, 6);
    gtk_widget_set_margin_start(recent_box, 14);
    gtk_widget_set_margin_end(recent_box, 14);
    auto* recent_caption = gtk_label_new(tr("nav.recent"));
    gtk_label_set_xalign(GTK_LABEL(recent_caption), 0.0f);
    gtk_widget_add_css_class(recent_caption, "pl-page-title");
    gtk_box_append(GTK_BOX(recent_box), recent_caption);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(recent_page), recent_box);
    gtk_stack_add_named(GTK_STACK(page_stack), recent_page, "recent");
    g_state.recent_list = GTK_BOX(recent_box);
  }
  {
    auto* shows_page = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(shows_page),
                                   GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    auto* shows_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_margin_top(shows_box, 6);
    gtk_widget_set_margin_start(shows_box, 14);
    gtk_widget_set_margin_end(shows_box, 14);
    auto* shows_caption = gtk_label_new(tr("nav.shows"));
    gtk_label_set_xalign(GTK_LABEL(shows_caption), 0.0f);
    gtk_widget_add_css_class(shows_caption, "pl-page-title");
    gtk_box_append(GTK_BOX(shows_box), shows_caption);
    auto* shows_flow = gtk_flow_box_new();
    gtk_flow_box_set_selection_mode(GTK_FLOW_BOX(shows_flow),
                                    GTK_SELECTION_NONE);
    gtk_flow_box_set_max_children_per_line(GTK_FLOW_BOX(shows_flow), 6);
    gtk_flow_box_set_min_children_per_line(GTK_FLOW_BOX(shows_flow), 3);
    gtk_widget_add_css_class(shows_flow, "pl-flow");
    gtk_box_append(GTK_BOX(shows_box), shows_flow);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(shows_page), shows_box);
    gtk_stack_add_named(GTK_STACK(page_stack), shows_page, "shows");
    g_state.shows_flow = GTK_FLOW_BOX(shows_flow);
    // clicking a tile opens that feed's episode page
    g_signal_connect(
        shows_flow, "child-activated",
        G_CALLBACK(+[](GtkFlowBox*, GtkFlowBoxChild* child, gpointer) {
          auto* tile = gtk_flow_box_child_get_child(child);
          if (tile == nullptr) return;
          auto const* feed_id =
              carried_data<std::string>(G_OBJECT(tile), "feed-id");
          if (feed_id == nullptr) return;
          g_state.selected_feed_id = *feed_id;
          for (auto const& feed : g_state.feeds) {
            if (feed.id == *feed_id) {
              gtk_label_set_text(g_state.episodes_caption, feed.title.c_str());
              break;
            }
          }
          reload_episodes();
          show_page("episodes");
        }),
        nullptr);
  }
  {
    auto* downloaded_page = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(downloaded_page),
                                   GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    auto* downloaded_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_margin_top(downloaded_box, 6);
    gtk_widget_set_margin_start(downloaded_box, 14);
    gtk_widget_set_margin_end(downloaded_box, 14);
    auto* downloaded_caption = gtk_label_new(tr("nav.downloaded"));
    gtk_label_set_xalign(GTK_LABEL(downloaded_caption), 0.0f);
    gtk_widget_add_css_class(downloaded_caption, "pl-page-title");
    gtk_box_append(GTK_BOX(downloaded_box), downloaded_caption);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(downloaded_page),
                                  downloaded_box);
    gtk_stack_add_named(GTK_STACK(page_stack), downloaded_page, "downloaded");
    g_state.downloaded_list = GTK_BOX(downloaded_box);
  }
  {
    // Discover: search row over the curated catalog as artwork cards.
    auto* discover_page = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(discover_page),
                                   GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    auto* discover_page_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_margin_top(discover_page_box, 6);
    gtk_widget_set_margin_start(discover_page_box, 14);
    gtk_widget_set_margin_end(discover_page_box, 14);
    auto* discover_caption = gtk_label_new(tr("nav.discover"));
    gtk_label_set_xalign(GTK_LABEL(discover_caption), 0.0f);
    gtk_widget_add_css_class(discover_caption, "pl-page-title");
    gtk_box_append(GTK_BOX(discover_page_box), discover_caption);

    auto* search_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    auto* entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(entry),
                                   tr("discover.search_placeholder"));
    gtk_widget_set_hexpand(entry, TRUE);
    gtk_box_append(GTK_BOX(search_row), entry);
    auto* search_button =
        gtk_button_new_from_icon_name("system-search-symbolic");
    gtk_box_append(GTK_BOX(search_row), search_button);
    gtk_box_append(GTK_BOX(discover_page_box), search_row);
    auto* results = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_box_append(GTK_BOX(discover_page_box), results);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(discover_page),
                                  discover_page_box);
    gtk_stack_add_named(GTK_STACK(page_stack), discover_page, "discover");
    g_state.discover_box = GTK_BOX(results);
    g_state.discover_entry = GTK_ENTRY(entry);
    g_signal_connect(search_button, "clicked",
                     G_CALLBACK(on_discover_search_clicked), nullptr);
    g_signal_connect(
        entry, "activate", G_CALLBACK(+[](GtkEntry*, gpointer) {
          on_discover_search_clicked(nullptr, nullptr);
        }),
        nullptr);
  }

  gtk_stack_set_visible_child_name(GTK_STACK(page_stack), "home");

  // Page column | detail column; the divider starts generous to the pages.
  auto* vertical = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
  gtk_paned_set_start_child(GTK_PANED(vertical), page_stack);
  gtk_paned_set_position(GTK_PANED(vertical), 420);

  // Detail pane: header, action row (Understand + progress), reading stack.
  auto* detail = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
  gtk_widget_set_margin_top(detail, 6);
  gtk_widget_set_margin_start(detail, 14);
  gtk_widget_set_margin_end(detail, 14);
  gtk_widget_set_margin_bottom(detail, 6);
  gtk_widget_set_visible(detail, FALSE);
  gtk_widget_set_size_request(detail, 360, -1);

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
  auto* pipeline =
      make_button_with_icon("starred-symbolic", tr("action.understand"));
  gtk_widget_add_css_class(pipeline, "suggested-action");
  gtk_widget_set_sensitive(pipeline, FALSE);
  gtk_box_append(GTK_BOX(actions), pipeline);
  auto* mark = gtk_button_new_with_label(tr("action.mark_played"));
  gtk_widget_set_sensitive(mark, FALSE);
  gtk_box_append(GTK_BOX(actions), mark);
  auto* remove_audio = gtk_button_new_with_label(tr("action.delete_audio"));
  gtk_widget_set_sensitive(remove_audio, FALSE);
  gtk_widget_set_visible(remove_audio, FALSE);
  gtk_box_append(GTK_BOX(actions), remove_audio);
  auto* focus_button = GTK_TOGGLE_BUTTON(
      gtk_toggle_button_new_with_label(tr("focus.enter")));
  gtk_widget_set_tooltip_text(GTK_WIDGET(focus_button), tr("focus.enter_hint"));
  gtk_box_append(GTK_BOX(actions), GTK_WIDGET(focus_button));
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

  // Reading row: translation on/off, follow, return to current.
  auto* reading = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  {
    auto* switcher_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    // reading mode: line-by-line translation on/off
    auto* bilingual_button = GTK_TOGGLE_BUTTON(
        gtk_toggle_button_new_with_label(tr("mode.bilingual")));
    gtk_toggle_button_set_active(bilingual_button, TRUE);
    gtk_box_append(GTK_BOX(switcher_row), GTK_WIDGET(bilingual_button));
    auto* follow_button = GTK_TOGGLE_BUTTON(
        gtk_toggle_button_new_with_label(tr("transcript.follow")));
    gtk_toggle_button_set_active(follow_button, TRUE);
    gtk_widget_set_tooltip_text(GTK_WIDGET(follow_button),
                                tr("transcript.follow_hint"));
    gtk_box_append(GTK_BOX(switcher_row), GTK_WIDGET(follow_button));
    auto* current_button =
        gtk_button_new_with_label(tr("transcript.current"));
    gtk_widget_set_tooltip_text(current_button,
                                tr("transcript.current_hint"));
    gtk_box_append(GTK_BOX(switcher_row), current_button);
    g_state.follow_button = follow_button;
    g_signal_connect(
        follow_button, "toggled",
        G_CALLBACK(+[](GtkToggleButton* toggle, gpointer) {
          g_state.transcript_follow = gtk_toggle_button_get_active(toggle);
          if (g_state.transcript_follow) scroll_to_running_segment();
        }),
        nullptr);
    g_signal_connect(
        current_button, "clicked",
        G_CALLBACK(+[](GtkButton*, gpointer) {
          g_state.transcript_follow = true;
          if (g_state.follow_button != nullptr) {
            gtk_toggle_button_set_active(g_state.follow_button, TRUE);
          }
          scroll_to_running_segment();
        }),
        nullptr);
    gtk_box_append(GTK_BOX(reading), switcher_row);
  }

  auto* transcript_scroll = gtk_scrolled_window_new();
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(transcript_scroll),
                                 GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  auto* transcript_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
  gtk_widget_set_margin_top(transcript_box, 6);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(transcript_scroll),
                                transcript_box);
  auto* summary_scroll = gtk_scrolled_window_new();
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(summary_scroll),
                                 GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  auto* summary_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
  gtk_widget_set_margin_top(summary_box, 6);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(summary_scroll),
                                summary_box);

  // one reading surface: summary on top, transcript below
  gtk_box_append(GTK_BOX(reading), GTK_WIDGET(summary_scroll));
  gtk_box_append(GTK_BOX(reading), GTK_WIDGET(transcript_scroll));
  gtk_widget_set_vexpand(GTK_WIDGET(transcript_scroll), TRUE);
  gtk_widget_set_vexpand(reading, TRUE);
  gtk_box_append(GTK_BOX(detail), reading);
  gtk_paned_set_end_child(GTK_PANED(vertical), detail);
  gtk_widget_set_vexpand(GTK_WIDGET(vertical), TRUE);

  gtk_box_append(GTK_BOX(pane), vertical);
  gtk_paned_set_end_child(GTK_PANED(body), pane);

  // Player bar: cover, now playing, transcript shortcut, play/pause,
  // ±15/30 s, chapter jumps, seek scale, clock and rate. Visible whenever
  // an episode is selected.
  auto* player_bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
  gtk_widget_add_css_class(player_bar, "pl-statusbar");
  gtk_widget_set_visible(player_bar, FALSE);
  auto* player_artwork = gtk_image_new();
  gtk_image_set_pixel_size(GTK_IMAGE(player_artwork), 44);
  gtk_widget_add_css_class(player_artwork, "pl-card-art");
  gtk_box_append(GTK_BOX(player_bar), GTK_WIDGET(player_artwork));
  auto* now_playing = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  auto* player_title = gtk_label_new("");
  gtk_label_set_ellipsize(GTK_LABEL(player_title), PANGO_ELLIPSIZE_END);
  gtk_widget_add_css_class(player_title, "pl-title");
  gtk_box_append(GTK_BOX(now_playing), player_title);
  auto* player_show = gtk_label_new("");
  gtk_label_set_ellipsize(GTK_LABEL(player_show), PANGO_ELLIPSIZE_END);
  gtk_widget_add_css_class(player_show, "pl-meta");
  gtk_box_append(GTK_BOX(now_playing), player_show);
  gtk_widget_set_size_request(now_playing, 200, -1);
  gtk_box_append(GTK_BOX(player_bar), now_playing);
  auto* play_button = gtk_button_new_from_icon_name(
      "media-playback-start-symbolic");
  gtk_widget_set_tooltip_text(play_button, tr("player.play_pause"));
  gtk_accessible_update_property(
      GTK_ACCESSIBLE(play_button), GTK_ACCESSIBLE_PROPERTY_LABEL,
      tr("player.play_pause"), -1);
  gtk_box_append(GTK_BOX(player_bar), play_button);
  auto* skip_back =
      gtk_button_new_from_icon_name("media-skip-backward-symbolic");
  gtk_widget_set_tooltip_text(skip_back, tr("player.back15"));
  gtk_box_append(GTK_BOX(player_bar), skip_back);
  auto* skip_forward =
      gtk_button_new_from_icon_name("media-skip-forward-symbolic");
  gtk_widget_set_tooltip_text(skip_forward, tr("player.forward30"));
  gtk_box_append(GTK_BOX(player_bar), skip_forward);
  auto* chapter_back =
      gtk_button_new_from_icon_name("go-previous-symbolic");
  gtk_widget_set_tooltip_text(chapter_back, tr("player.chapter_prev"));
  gtk_widget_set_visible(chapter_back, FALSE);
  gtk_box_append(GTK_BOX(player_bar), chapter_back);
  auto* chapter_forward =
      gtk_button_new_from_icon_name("go-next-symbolic");
  gtk_widget_set_tooltip_text(chapter_forward, tr("player.chapter_next"));
  gtk_widget_set_visible(chapter_forward, FALSE);
  gtk_box_append(GTK_BOX(player_bar), chapter_forward);
  auto* sleep_button = gtk_menu_button_new();
  gtk_menu_button_set_icon_name(GTK_MENU_BUTTON(sleep_button),
                                "weather-clear-night-symbolic");
  gtk_widget_set_tooltip_text(sleep_button, tr("sleep.title"));
  {
    auto* sleep_menu = g_menu_new();
    g_menu_append(sleep_menu, tr("sleep.off"), "win.sleep::0");
    for (int minutes : {5, 10, 15, 30, 45, 60, 90}) {
      char label[64];
      std::snprintf(label, sizeof(label), tr("sleep.minutes"), minutes);
      auto* item = g_menu_item_new(label, nullptr);
      g_menu_item_set_action_and_target_value(item, "win.sleep",
                                              g_variant_new_int32(minutes));
      g_menu_append_item(sleep_menu, item);
      g_object_unref(item);
    }
    gtk_menu_button_set_menu_model(GTK_MENU_BUTTON(sleep_button),
                                   G_MENU_MODEL(sleep_menu));
    g_object_unref(sleep_menu);
  }
  gtk_box_append(GTK_BOX(player_bar), sleep_button);
  auto* position_text = GTK_LABEL(gtk_label_new("00:00"));
  gtk_box_append(GTK_BOX(player_bar), GTK_WIDGET(position_text));
  auto* seek_scale = GTK_SCALE(
      gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 0, 100, 1));
  gtk_range_set_value(GTK_RANGE(seek_scale), 0);
  gtk_scale_set_draw_value(seek_scale, FALSE);
  gtk_widget_set_hexpand(GTK_WIDGET(seek_scale), TRUE);
  gtk_box_append(GTK_BOX(player_bar), GTK_WIDGET(seek_scale));
  auto* duration_text = GTK_LABEL(gtk_label_new("00:00"));
  gtk_box_append(GTK_BOX(player_bar), GTK_WIDGET(duration_text));
  auto* rate_selector = GTK_COMBO_BOX_TEXT(gtk_combo_box_text_new());
  for (char const* rate : {"1.0×", "1.25×", "1.5×", "1.75×", "2.0×",
                           "2.5×", "3.0×"}) {
    gtk_combo_box_text_append_text(rate_selector, rate);
  }
  gtk_combo_box_set_active(GTK_COMBO_BOX(rate_selector), 0);
  gtk_box_append(GTK_BOX(player_bar), GTK_WIDGET(rate_selector));

  // Status bar at the bottom carries progress chatter quietly.
  auto* statusbar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_add_css_class(statusbar, "pl-statusbar");
  auto* status_text = gtk_label_new(tr("status.starting"));
  gtk_label_set_ellipsize(GTK_LABEL(status_text), PANGO_ELLIPSIZE_END);
  gtk_label_set_xalign(GTK_LABEL(status_text), 0.0f);
  gtk_widget_add_css_class(status_text, "pl-meta");
  gtk_box_append(GTK_BOX(statusbar), status_text);

  auto* root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_box_append(GTK_BOX(root), body);
  gtk_box_append(GTK_BOX(root), player_bar);
  gtk_box_append(GTK_BOX(root), statusbar);
  gtk_window_set_child(GTK_WINDOW(window), root);

  g_state.window = GTK_WINDOW(window);
  g_state.pipeline_button = GTK_BUTTON(pipeline);
  g_state.download_button = GTK_BUTTON(download);
  g_state.transcribe_button = GTK_BUTTON(transcribe);
  g_state.translate_button = GTK_BUTTON(translate);
  g_state.summarize_button = GTK_BUTTON(summarize);
  g_state.mark_button = GTK_BUTTON(mark);
  g_state.remove_audio_button = GTK_BUTTON(remove_audio);
  g_state.player_bar = GTK_BOX(player_bar);
  g_state.play_button = GTK_BUTTON(play_button);
  g_state.skip_back_button = GTK_BUTTON(skip_back);
  g_state.skip_forward_button = GTK_BUTTON(skip_forward);
  g_state.chapter_back_button = GTK_BUTTON(chapter_back);
  g_state.chapter_forward_button = GTK_BUTTON(chapter_forward);
  g_state.seek_scale = seek_scale;
  g_state.position_text = position_text;
  g_state.duration_text = duration_text;
  g_state.feed_list = GTK_LIST_BOX(feed_list);
  g_state.sidebar = sidebar;
  g_state.page_stack = GTK_STACK(page_stack);
  g_state.player_artwork = GTK_IMAGE(player_artwork);
  g_state.player_title = GTK_LABEL(player_title);
  g_state.player_show = GTK_LABEL(player_show);
  g_state.episodes_caption = GTK_LABEL(episode_caption);
  g_state.episode_list = GTK_LIST_BOX(episode_list);
  g_state.list_stack = GTK_STACK(list_stack);
  g_state.empty_icon = GTK_LABEL(empty_icon);
  g_state.empty_text = GTK_LABEL(empty_text);
  g_state.detail_box = GTK_BOX(detail);
  g_state.focus_button = focus_button;
  g_state.detail_title = GTK_LABEL(detail_title);
  g_state.detail_meta = GTK_LABEL(detail_meta);
  g_state.detail_notes = GTK_LABEL(detail_notes);
  g_state.job_bar = GTK_PROGRESS_BAR(job_bar);
  g_state.job_label = GTK_LABEL(job_label);
  g_state.transcript_scroll = GTK_SCROLLED_WINDOW(transcript_scroll);
  g_state.transcript_box = GTK_BOX(transcript_box);
  g_state.summary_box = GTK_BOX(summary_box);
  g_state.status_text = GTK_LABEL(status_text);

  add_app_action(app, "add-feed", on_add_feed_activated);
  add_app_action(app, "refresh-all", on_refresh_all_activated);
  add_app_action(app, "about", on_about_activated);
  add_app_action(app, "settings", on_settings_activated);
  add_app_action(app, "check-updates", on_check_updates_activated);
  add_window_action(GTK_APPLICATION_WINDOW(window), "feed-remove",
                    G_VARIANT_TYPE_STRING, on_feed_remove_activated);
  add_window_action(GTK_APPLICATION_WINDOW(window), "feed-refresh",
                    G_VARIANT_TYPE_STRING, on_feed_refresh_activated);
  add_window_action(GTK_APPLICATION_WINDOW(window), "episode-done",
                    G_VARIANT_TYPE("(ss)"), on_episode_done_activated);
  add_window_action(GTK_APPLICATION_WINDOW(window), "sleep",
                    G_VARIANT_TYPE_INT32, on_sleep_activated);

  g_signal_connect(feed_list, "row-selected",
                   G_CALLBACK(on_nav_row_selected), nullptr);
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
  g_signal_connect(mark, "clicked", G_CALLBACK(on_mark_clicked), nullptr);
  g_signal_connect(remove_audio, "clicked",
                   G_CALLBACK(on_remove_audio_clicked), nullptr);
  g_signal_connect(
      focus_button, "toggled",
      G_CALLBACK(+[](GtkToggleButton* toggle, gpointer) {
        bool const focused = gtk_toggle_button_get_active(toggle);
        if (g_state.sidebar != nullptr) {
          gtk_widget_set_visible(g_state.sidebar, !focused);
        }
        if (g_state.page_stack != nullptr) {
          gtk_widget_set_visible(GTK_WIDGET(g_state.page_stack), !focused);
        }
        gtk_button_set_label(GTK_BUTTON(toggle),
                             tr(focused ? "focus.exit" : "focus.enter"));
      }),
      nullptr);
  g_signal_connect(play_button, "clicked", G_CALLBACK(on_play_clicked),
                   nullptr);
  g_signal_connect(skip_back, "clicked", G_CALLBACK(on_skip_back_clicked),
                   nullptr);
  g_signal_connect(skip_forward, "clicked",
                   G_CALLBACK(on_skip_forward_clicked), nullptr);
  g_signal_connect(chapter_back, "clicked",
                   G_CALLBACK(on_chapter_back_clicked), nullptr);
  g_signal_connect(chapter_forward, "clicked",
                   G_CALLBACK(on_chapter_forward_clicked), nullptr);
  g_signal_connect(seek_scale, "value-changed",
                   G_CALLBACK(on_seek_value_changed), nullptr);
  {
    auto* seek_gesture = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(seek_gesture),
                                  GDK_BUTTON_PRIMARY);
    g_signal_connect(seek_gesture, "pressed",
                     G_CALLBACK(on_seek_pressed), nullptr);
    g_signal_connect(seek_gesture, "released",
                     G_CALLBACK(on_seek_released), nullptr);
    gtk_widget_add_controller(GTK_WIDGET(seek_scale),
                              GTK_EVENT_CONTROLLER(seek_gesture));
  }
  g_signal_connect(rate_selector, "changed", G_CALLBACK(on_rate_changed),
                   nullptr);
  g_signal_connect(
      bilingual_button, "toggled",
      G_CALLBACK(+[](GtkToggleButton* toggle, gpointer) {
        g_state.transcript_bilingual = gtk_toggle_button_get_active(toggle);
        render_transcript_public();
      }),
      nullptr);

  // Show the player bar as soon as an episode exists to play.
  g_signal_connect(episode_list, "row-selected",
                   G_CALLBACK(+[](GtkListBox*, GtkListBoxRow* row,
                                  gpointer) {
                     gtk_widget_set_visible(
                         GTK_WIDGET(g_state.player_bar), row != nullptr);
                   }),
                   nullptr);

  player_init();
  mpris_start();
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
