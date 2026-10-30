#include "pch.h"
#include "MainWindow.xaml.h"
#if __has_include("MainWindow.g.cpp")
#include "MainWindow.g.cpp"
#endif
#include "GeneratedBackend.hpp"
#include "I18n.h"

#include <shellapi.h>
#include <shlobj.h>

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Media.Core.h>
#include <winrt/Windows.Media.Playback.h>
#include <winrt/Windows.Media.h>
#include <winrt/Windows.System.h>
#include <winrt/Microsoft.UI.Text.h>
#include <winrt/Microsoft.UI.Xaml.Input.h>
#include <winrt/Microsoft.UI.Xaml.Media.h>
#include <winrt/Windows.Storage.h>
#include <winrt/Windows.Storage.Streams.h>
#include <winrt/Microsoft.UI.Windowing.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cwchar>
#include <fstream>
#include <future>
#include <regex>
#include <set>
#include <sstream>
#include <iomanip>
#include <locale>
#include <thread>
#include <type_traits>

namespace winrt::RivetHost::implementation {

using Microsoft::UI::Xaml::Visibility;
namespace mux = winrt::Microsoft::UI::Xaml;
namespace muxc = winrt::Microsoft::UI::Xaml::Controls;

// Host helpers shared with MainWindow.Update.cpp (declared in
// MainWindow.xaml.h, taskly's HostHelpers pattern); everything else
// file-local stays in the anonymous namespaces.

std::filesystem::path executable_path() {
  std::wstring buffer(32768, L'\0');
  auto const length = ::GetModuleFileNameW(nullptr, buffer.data(),
                                          static_cast<DWORD>(buffer.size()));
  if (length == 0 || length == buffer.size()) {
    throw std::runtime_error("GetModuleFileNameW failed");
  }
  buffer.resize(length);
  return std::filesystem::path(buffer);
}

std::string utf8(std::filesystem::path const& path) {
  auto const wide = path.wstring();
  if (wide.empty()) {
    return {};
  }
  auto const size = ::WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
                                          wide.data(),
                                          static_cast<int>(wide.size()),
                                          nullptr, 0, nullptr, nullptr);
  if (size <= 0) {
    throw std::runtime_error("WideCharToMultiByte failed");
  }
  std::string result(static_cast<std::size_t>(size), '\0');
  if (::WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
                            wide.data(), static_cast<int>(wide.size()),
                            result.data(), size, nullptr, nullptr) != size) {
    throw std::runtime_error("WideCharToMultiByte failed");
  }
  return result;
}

std::string to_utf8(std::wstring const& wide) {
  if (wide.empty()) return {};
  auto const size = ::WideCharToMultiByte(CP_UTF8, 0, wide.data(),
                                          static_cast<int>(wide.size()),
                                          nullptr, 0, nullptr, nullptr);
  std::string result(static_cast<std::size_t>(size > 0 ? size : 0), '\0');
  if (size > 0) {
    ::WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()),
                          result.data(), size, nullptr, nullptr);
  }
  return result;
}

std::wstring to_wide(std::string const& utf8_text) {
  if (utf8_text.empty()) return {};
  auto const size = ::MultiByteToWideChar(CP_UTF8, 0, utf8_text.data(),
                                          static_cast<int>(utf8_text.size()),
                                          nullptr, 0);
  std::wstring result(static_cast<std::size_t>(size > 0 ? size : 0), L'\0');
  if (size > 0) {
    ::MultiByteToWideChar(CP_UTF8, 0, utf8_text.data(),
                          static_cast<int>(utf8_text.size()),
                          result.data(), size);
  }
  return result;
}

namespace {

rivet::windows::RacketRuntimeConfig runtime_config() {
  auto const exe = executable_path();
  auto const root = exe.parent_path();
  auto const runtime = root / L"runtime";

  rivet::windows::RacketRuntimeConfig config;
  config.executable_path = utf8(exe);
  config.petite_boot = utf8(runtime / L"petite.boot");
  config.scheme_boot = utf8(runtime / L"scheme.boot");
  config.racket_boot = utf8(runtime / L"racket.boot");
  config.backend_bundle = utf8(root / L"res" / L"core.zo");
  config.module_name = rivet_app::kModuleName;
  config.entry_symbol = rivet_app::kEntryName;
  config.dll_dir = runtime.wstring();
  return config;
}

// ---------------------------------------------------------------------------
// Tiny JSON readers for the summary payload. The backend emits flat, regular
// JSON ({"tldr": "...", "key-points": [...], "quotes": [{...}], ...}) so a
// scanner beats pulling in a JSON library.

std::wstring json_string_value(std::wstring const& json, std::wstring const& key) {
  auto const key_pos = json.find(L"\"" + key + L"\"");
  if (key_pos == std::wstring::npos) return {};
  auto const colon = json.find(L':', key_pos + key.size() + 2);
  if (colon == std::wstring::npos) return {};
  auto const open = json.find(L'"', colon + 1);
  if (open == std::wstring::npos) return {};
  std::wstring out;
  for (auto i = open + 1; i < json.size(); ++i) {
    if (json[i] == L'\\' && i + 1 < json.size()) {
      wchar_t const next = json[i + 1];
      if (next == L'n') out += L'\n';
      else if (next == L't') out += L'\t';
      else out += next;
      ++i;
      continue;
    }
    if (json[i] == L'"') break;
    out += json[i];
  }
  return out;
}

// [start, end) span of the value for `key`, when it is an array.
bool json_array_span(std::wstring const& json, std::wstring const& key,
                     size_t& begin, size_t& end) {
  auto const key_pos = json.find(L"\"" + key + L"\"");
  if (key_pos == std::wstring::npos) return false;
  auto const colon = json.find(L':', key_pos + key.size() + 2);
  if (colon == std::wstring::npos) return false;
  auto const open = json.find(L'[', colon + 1);
  if (open == std::wstring::npos) return false;
  int depth = 0;
  bool in_string = false;
  for (auto i = open; i < json.size(); ++i) {
    wchar_t const c = json[i];
    if (in_string) {
      if (c == L'\\') { ++i; continue; }
      if (c == L'"') in_string = false;
      continue;
    }
    if (c == L'"') in_string = true;
    else if (c == L'[') ++depth;
    else if (c == L']') {
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

std::vector<std::wstring> json_array_strings(std::wstring const& json,
                                             std::wstring const& key) {
  std::vector<std::wstring> out;
  size_t begin = 0, end = 0;
  if (!json_array_span(json, key, begin, end)) return out;
  bool in_string = false;
  size_t string_start = 0;
  for (auto i = begin; i < end; ++i) {
    wchar_t const c = json[i];
    if (in_string) {
      if (c == L'\\') { ++i; continue; }
      if (c == L'"') {
        in_string = false;
        out.push_back(json.substr(string_start, i - string_start));
      }
      continue;
    }
    if (c == L'"') {
      in_string = true;
      string_start = i + 1;
    }
  }
  return out;
}

// Each element of `key` is an object carrying `text` and `translation`.
std::vector<std::pair<std::wstring, std::wstring>> json_array_objects(
    std::wstring const& json, std::wstring const& key) {
  std::vector<std::pair<std::wstring, std::wstring>> out;
  size_t begin = 0, end = 0;
  if (!json_array_span(json, key, begin, end)) return out;
  int depth = 0;
  bool in_string = false;
  size_t object_start = 0;
  for (auto i = begin; i < end; ++i) {
    wchar_t const c = json[i];
    if (in_string) {
      if (c == L'\\') { ++i; continue; }
      if (c == L'"') in_string = false;
      continue;
    }
    if (c == L'"') in_string = true;
    else if (c == L'{') {
      if (depth == 0) object_start = i;
      ++depth;
    } else if (c == L'}') {
      --depth;
      if (depth == 0) {
        std::wstring const object = json.substr(object_start, i - object_start + 1);
        out.emplace_back(json_string_value(object, L"text"),
                         json_string_value(object, L"translation"));
      }
    }
  }
  return out;
}

std::wstring FormatSeconds(double seconds) {
  if (seconds < 0 || !std::isfinite(seconds)) seconds = 0;
  wchar_t buffer[16];
  swprintf_s(buffer, L"%02d:%02d", static_cast<int>(seconds) / 60,
             static_cast<int>(seconds) % 60);
  return buffer;
}

muxc::Border MakeChip(std::wstring const& text);

// WinRT TimeSpans tick in 100 ns units; playback math speaks in seconds.
double TimeSpanSeconds(winrt::Windows::Foundation::TimeSpan const& span) {
  return static_cast<double>(span.count()) / 1e7;
}

}  // namespace

MainWindow::MainWindow() {
  InitializeComponent();

  Title(winrt::hstring(std::wstring(podlens::Tr("app.title"))));

  // The title bar shows WinUI's generic placeholder unless the window is
  // handed an icon explicitly; rivet embeds the app icon in the exe at
  // resource ID 1 for exactly this use (rivet-cli/build.rkt).
  if (HICON icon =
          ::LoadIconW(::GetModuleHandleW(nullptr), MAKEINTRESOURCEW(1))) {
    AppWindow().SetIcon(winrt::Microsoft::UI::IconId{
        static_cast<std::uint64_t>(reinterpret_cast<UINT_PTR>(icon))});
  }

  SubtitleText().Text(std::wstring(podlens::Tr("app.subtitle")));
  StatusLine().Text(std::wstring(podlens::Tr("status.starting")));
  AddFeedButton().Content(box_value(winrt::hstring(
      L"＋ " + std::wstring(podlens::Tr("nav.add_feed")))));
  SettingsNavLabel().Text(std::wstring(podlens::Tr("menu.settings")));
  HelpButton().Content(box_value(
      winrt::hstring(std::wstring(podlens::Tr("menu.help")))));
  MenuCheckUpdates().Text(
      winrt::hstring(std::wstring(podlens::Tr("menu.check_updates"))));
  MenuRefreshAll().Text(
      winrt::hstring(std::wstring(podlens::Tr("menu.refresh_all"))));
  MenuAbout().Text(winrt::hstring(std::wstring(podlens::Tr("menu.about"))));
  EpisodesHeader().Text(std::wstring(podlens::Tr("episodes.header")));
  DetailEmptyTitle().Text(std::wstring(podlens::Tr("detail.none")));
  DetailEmptyHint().Text(std::wstring(podlens::Tr("detail.hint")));
  DetailEmptyCta().Content(box_value(winrt::hstring(
      std::wstring(podlens::Tr("empty.discover_cta")))));
  DetailEmptyCta().Click([weak = get_weak()](auto&&, auto&&) {
    if (auto window = weak.get()) window->ShowPage(Page::Discover);
  });
  EpisodesEmptyTitle().Text(std::wstring(podlens::Tr("empty.episodes_title")));
  EpisodesEmptyHint().Text(std::wstring(podlens::Tr("empty.episodes_hint")));
  MenuRemoveAudio().Text(std::wstring(podlens::Tr("action.remove_audio")));
  BilingualToggle().Content(box_value(winrt::hstring(
      std::wstring(podlens::Tr("mode.bilingual")))));
  FocusModeButton().Content(box_value(winrt::hstring(
      std::wstring(podlens::Tr("focus.enter")))));
  FollowTranscriptButton().Content(box_value(winrt::hstring(
      std::wstring(podlens::Tr("transcript.follow")))));
  ReturnToCurrentButton().Content(box_value(winrt::hstring(
      std::wstring(podlens::Tr("transcript.current")))));
  DiscoverSearchBox().PlaceholderText(
      winrt::hstring(std::wstring(podlens::Tr("discover.search_placeholder"))));
  // Filling the selectors fires SelectionChanged while the window is still
  // being constructed; initialized_ keeps those callbacks harmless.
  for (wchar_t const* label :
       {L"1.0×", L"1.25×", L"1.5×", L"1.75×", L"2.0×", L"2.5×", L"3.0×"}) {
    RateSelector().Items().Append(box_value(winrt::hstring(label)));
  }
  RateSelector().SelectedIndex(0);

  position_timer_ = DispatcherQueue().CreateTimer();
  position_timer_.Interval(std::chrono::seconds{1});
  position_timer_.Tick([weak = get_weak()](auto&&, auto&&) {
    if (auto window = weak.get()) window->TickPlayer();
  });

  sleep_timer_ = DispatcherQueue().CreateTimer();
  sleep_timer_.IsRepeating(false);
  sleep_timer_.Tick([weak = get_weak()](auto&&, auto&&) {
    if (auto window = weak.get()) {
      window->sleep_minutes_ = 0;
      window->RemotePause();
      window->SetStatus(true, std::wstring(podlens::Tr("player.sleep_fired")));
    }
  });

  // background subscription refresh; new episodes surface in the status line
  refresh_timer_ = DispatcherQueue().CreateTimer();
  refresh_timer_.Interval(std::chrono::minutes{30});
  refresh_timer_.Tick([weak = get_weak()](auto&&, auto&&) {
    if (auto window = weak.get()) window->AutoRefreshTick();
  });
  refresh_timer_.Start();

  PipelineButton().Content(box_value(
      winrt::hstring(std::wstring(podlens::Tr("action.pipeline")))));
  MenuMark().Text(std::wstring(podlens::Tr("action.mark_played")));
  muxc::ToolTipService::SetToolTip(
      FocusModeButton(),
      box_value(winrt::hstring(std::wstring(podlens::Tr("focus.enter_hint")))));
  muxc::ToolTipService::SetToolTip(
      FollowTranscriptButton(),
      box_value(winrt::hstring(std::wstring(podlens::Tr("transcript.follow_hint")))));
  muxc::ToolTipService::SetToolTip(
      ReturnToCurrentButton(),
      box_value(winrt::hstring(std::wstring(podlens::Tr("transcript.current_hint")))));

  // The player is constructed up front: every call on a null projected
  // MediaPlayer throws, so deferring construction made playback itself
  // fail. SMTC is wired manually — the CommandManager auto-handles
  // play/pause without refreshing our glyph and timers, and the extra
  // buttons (next/previous) would promise a queue that does not exist.
  try {
    player_ = winrt::Windows::Media::Playback::MediaPlayer();
    player_.CommandManager().IsEnabled(false);
    auto smtc = player_.SystemMediaTransportControls();
    smtc.IsEnabled(true);
    smtc.IsPlayEnabled(true);
    smtc.IsPauseEnabled(true);
    smtc.IsNextEnabled(false);
    smtc.IsPreviousEnabled(false);
    auto const dispatcher = DispatcherQueue();
    auto const weak = get_weak();
    smtc.ButtonPressed([dispatcher, weak](auto&&, auto&& args) {
      auto const button = args.Button();
      dispatcher.TryEnqueue([weak, button]() {
        if (auto window = weak.get()) {
          try {
            if (button ==
                winrt::Windows::Media::SystemMediaTransportControlsButton::Play) {
              window->RemotePlay();
            } else if (button ==
                       winrt::Windows::Media::SystemMediaTransportControlsButton::Pause) {
              window->RemotePause();
            }
          } catch (...) {
          }
        }
      });
    });
  } catch (winrt::hresult_error const& e) {
    SetStatus(false, e.message().c_str());
  }

  // Player bar flyout/tooltips carry the build-time language.
  try {
    if (auto flyout = SleepButton().Flyout().try_as<muxc::MenuFlyout>()) {
      for (uint32_t i = 0; i < flyout.Items().Size(); ++i) {
        auto item = flyout.Items().GetAt(i).try_as<muxc::MenuFlyoutItem>();
        if (!item) continue;
        int const minutes = static_cast<int>(std::wcstol(
            winrt::unbox_value_or<winrt::hstring>(item.Tag(), winrt::hstring(L"0"))
                .c_str(),
            nullptr, 10));
        if (minutes <= 0) {
          item.Text(std::wstring(podlens::Tr("player.sleep_off")));
        } else {
          wchar_t text[48];
          swprintf(text, 48, podlens::Tr("player.sleep_minutes").data(), minutes);
          item.Text(std::wstring(text));
        }
      }
    }
    muxc::ToolTipService::SetToolTip(
        SleepButton(), box_value(winrt::hstring(std::wstring(podlens::Tr("player.sleep")))));
    muxc::ToolTipService::SetToolTip(
        SkipBackButton(),
        box_value(winrt::hstring(std::wstring(podlens::Tr("player.skip_back")))));
    muxc::ToolTipService::SetToolTip(
        SkipForwardButton(),
        box_value(winrt::hstring(std::wstring(podlens::Tr("player.skip_forward")))));
    muxc::ToolTipService::SetToolTip(
        ChapterBackButton(),
        box_value(winrt::hstring(std::wstring(podlens::Tr("chapter.prev")))));
    muxc::ToolTipService::SetToolTip(
        ChapterForwardButton(),
        box_value(winrt::hstring(std::wstring(podlens::Tr("chapter.next")))));
  } catch (...) {
  }

  error_bar_timer_ = DispatcherQueue().CreateTimer();
  error_bar_timer_.Interval(std::chrono::seconds{6});
  error_bar_timer_.IsRepeating(false);
  error_bar_timer_.Tick([weak = get_weak()](auto&&, auto&&) {
    if (auto window = weak.get()) window->ErrorBar().IsOpen(false);
  });

  initialized_ = true;
  InitializeBackendAsync();
}

winrt::fire_and_forget MainWindow::InitializeBackendAsync() {
  auto const dispatcher = DispatcherQueue();
  auto const weak = get_weak();
  auto backend = std::make_shared<rivet::windows::Backend>(runtime_config());

  try {
    // Booting the embedded runtime can block on file I/O, so only startup
    // moves off the UI thread. RPC traffic is completion-driven below.
    co_await winrt::resume_background();
    backend->start();

    // Backend events arrive on the reader thread; dispatch before touching
    // any UI object.
    backend->set_event_handler([dispatcher, weak](std::string const& name,
                                                  rivet::Value const& value) {
      dispatcher.TryEnqueue([weak, name, value]() mutable {
        if (auto window = weak.get()) {
          try {
            auto const event = rivet_app::decode_event(name, value);
            if (auto const notify = std::get_if<rivet_app::NotifyEvent>(&event)) {
              window->SetStatus(true, to_wide(notify->value));
            } else if (auto const open = std::get_if<rivet_app::Open_urlEvent>(&event)) {
              std::wstring const url = to_wide(open->value);
              ::ShellExecuteW(nullptr, L"open", url.c_str(), nullptr, nullptr,
                              SW_SHOWNORMAL);
            } else if (auto const update =
                           std::get_if<rivet_app::Update_availableEvent>(&event)) {
              // the version travels on the wire for compat; the Windows flow
              // surfaces through the update-check dialog instead
              (void)update;
            } else if (auto const changed =
                           std::get_if<rivet_app::Episodes_changedEvent>(&event)) {
              window->ReloadFeeds();
              window->ReloadRecent();
              if (window->page_ == Page::Episodes) window->ReloadEpisodes();
            }
          } catch (std::exception const&) {
            // unknown event names are ignored — forward compatibility
          }
        }
      });
    });

    dispatcher.TryEnqueue([weak, backend = std::move(backend)]() mutable {
      if (auto window = weak.get()) {
        window->backend_ = std::move(backend);
        window->api_ = std::make_unique<rivet_app::API>(*window->backend_);
        window->SetStatus(true, std::wstring(podlens::Tr("status.ready")));
        window->ReloadFeeds();
        window->ReloadRecent();
        window->ResumeLast();
        // A previous update run may have left a failure report
        // (docs/UPDATE.md); surface it once, then start the throttled
        // silent check.
        window->StartAutoUpdateCheck();
      } else {
        // Never destroy the last Backend reference on its own reader thread.
        std::thread([backend = std::move(backend)]() mutable {
          backend->stop();
        }).detach();
      }
    });
  } catch (std::exception const& e) {
    auto message = std::string(e.what());
    dispatcher.TryEnqueue([weak, message = std::move(message)] {
      if (auto window = weak.get()) {
        window->SetStatus(false, to_wide(message));
      }
    });
  }
}

// ---- page switching ---------------------------------------------------------

void MainWindow::ShowPage(Page page) {
  page_ = page;
  HomePage().Visibility(page == Page::Home ? Visibility::Visible
                                           : Visibility::Collapsed);
  DiscoverView().Visibility(page == Page::Discover ? Visibility::Visible
                                                   : Visibility::Collapsed);
  RecentPage().Visibility(page == Page::Recent ? Visibility::Visible
                                               : Visibility::Collapsed);
  ShowsPage().Visibility(page == Page::Shows ? Visibility::Visible
                                             : Visibility::Collapsed);
  DownloadedPage().Visibility(page == Page::Downloaded ? Visibility::Visible
                                                       : Visibility::Collapsed);
  EpisodesPage().Visibility(page == Page::Episodes ? Visibility::Visible
                                                   : Visibility::Collapsed);
  if (page == Page::Discover && DiscoverList().Children().Size() == 0) {
    LoadDiscover();
  }
  if (page == Page::Home) RenderHomePage();
  if (page == Page::Shows) RenderShowsPage();
  if (page == Page::Downloaded) RenderDownloadedPage();
  UpdateDetailVisibility();
}

void MainWindow::UpdateDetailVisibility() {
  bool const has_episode = !selected_episode_id_.empty();

  bool const empty_library = feeds_.empty();
  DetailEmpty().Visibility(has_episode ? Visibility::Collapsed
                                       : Visibility::Visible);
  if (empty_library) {
    DetailEmptyTitle().Text(std::wstring(podlens::Tr("empty.feeds_title")));
    DetailEmptyHint().Text(std::wstring(podlens::Tr("empty.feeds_hint")));
  } else {
    DetailEmptyTitle().Text(std::wstring(podlens::Tr("detail.none")));
    DetailEmptyHint().Text(std::wstring(podlens::Tr("detail.hint")));
  }
  DetailViewHost().Visibility(has_episode ? Visibility::Visible
                                          : Visibility::Collapsed);
  PlayerBar().Visibility(has_episode ? Visibility::Visible
                                     : Visibility::Collapsed);
}

// ---- feeds ------------------------------------------------------------------

void MainWindow::ReloadFeeds() {
  if (!api_) return;
  auto const dispatcher = DispatcherQueue();
  auto const weak = get_weak();
  (void)api_->feed_list_async(
      [dispatcher, weak](rivet_app::Result<std::vector<std::vector<std::string>>> result) {
        dispatcher.TryEnqueue([weak, result]() mutable {
          if (auto window = weak.get()) {
            try {
              window->RenderFeeds(result.get());
            } catch (std::exception const& e) {
              window->SetStatus(false, to_wide(e.what()));
            }
          }
        });
      });
}

muxc::NavigationViewItem MakeNavItem(char const* tag, wchar_t const* glyph,
                                     wchar_t const* label) {
  muxc::NavigationViewItem item;
  item.Tag(box_value(winrt::hstring(to_wide(tag))));
  item.Content(box_value(winrt::hstring(label)));
  muxc::FontIcon icon;
  icon.Glyph(winrt::hstring(glyph));
  item.Icon(icon);
  return item;
}

// Nav layout: home, discover, a Library section (recent / shows /
// downloaded), a subscriptions header, then one cover-art row per feed.
// Feed rows start at fixed index 7 — ResumeLast and SelectFeedById rely on
// that offset.
void MainWindow::RenderFeeds(std::vector<std::vector<std::string>> rows) {
  feeds_.clear();
  Nav().MenuItems().Clear();

  Nav().MenuItems().Append(
      MakeNavItem("home", L"\xE80F", podlens::Tr("nav.home").data()));
  Nav().MenuItems().Append(
      MakeNavItem("discover", L"\uE8A9", podlens::Tr("nav.discover").data()));

  muxc::NavigationViewItemHeader library_header;
  library_header.Content(box_value(
      winrt::hstring(std::wstring(podlens::Tr("library.title")))));
  Nav().MenuItems().Append(library_header);
  Nav().MenuItems().Append(MakeNavItem(
      "library-recent", L"\xE823", podlens::Tr("library.recent").data()));
  Nav().MenuItems().Append(MakeNavItem(
      "library-shows", L"\uE8A9", podlens::Tr("library.shows").data()));
  Nav().MenuItems().Append(MakeNavItem(
      "library-downloaded", L"\xE896", podlens::Tr("library.downloaded").data()));

  muxc::NavigationViewItemHeader subs_header;
  subs_header.Content(box_value(
      winrt::hstring(std::wstring(podlens::Tr("menu.subs")))));
  Nav().MenuItems().Append(subs_header);

  for (auto const& row : rows) {
    if (row.size() < 5) continue;
    FeedRow feed;
    feed.id = row[0];
    feed.title = row[1];
    feed.artwork = row.size() > 3 ? row[3] : "";
    feed.count = row[4];
    feed.unplayed = row.size() > 7 ? std::atoi(row[7].c_str()) : 0;
    feeds_.push_back(feed);

    muxc::NavigationViewItem item;
    item.Tag(box_value(winrt::hstring(to_wide(feed.id))));
    muxc::StackPanel content;
    content.Orientation(Microsoft::UI::Xaml::Controls::Orientation::Horizontal);
    content.Spacing(6);
    muxc::TextBlock label;
    label.Text(winrt::hstring(to_wide(feed.title)));
    label.MaxLines(1);
    label.TextTrimming(Microsoft::UI::Xaml::TextTrimming::CharacterEllipsis);
    content.Children().Append(label);
    muxc::TextBlock count_text;
    count_text.Text(winrt::hstring(to_wide(feed.count)));
    count_text.FontSize(12);
    count_text.Opacity(0.65);
    content.Children().Append(count_text);
    if (feed.unplayed > 0) {
      content.Children().Append(
          MakeChip(std::to_wstring(feed.unplayed) +
                   std::wstring(podlens::Tr("badge.unplayed"))));
    }
    item.Content(box_value(content));
    if (!feed.artwork.empty()) {
      muxc::ImageIcon icon;
      icon.Width(24);
      icon.Height(24);
      try {
        winrt::Microsoft::UI::Xaml::Media::Imaging::BitmapImage bitmap(
            winrt::Windows::Foundation::Uri(to_wide(feed.artwork)));
        icon.Source(bitmap);
      } catch (...) {
      }
      item.Icon(icon);
    } else {
      muxc::FontIcon icon;
      icon.Glyph(winrt::hstring(L"\xE8F1"));
      item.Icon(icon);
    }
    Nav().MenuItems().Append(item);
  }

  // Resume-on-launch opens its feed's page; otherwise the startup page
  // stays Home (SelectionChanged drives the actual switch).
  if (!pending_resume_feed_id_.empty()) {
    for (uint32_t i = 0; i < feeds_.size(); ++i) {
      if (feeds_[i].id == pending_resume_feed_id_) {
        pending_resume_feed_id_.clear();
        selected_feed_id_ = feeds_[i].id;
        ShowPage(Page::Episodes);
        // Programmatic navigation suppresses Nav_SelectionChanged, which is
        // normally responsible for loading the feed's episodes. Resume must
        // therefore start that load explicitly before it can select the saved
        // episode and reveal the detail/player controls.
        ReloadEpisodes();
        uint32_t const nav_index = 7 + i;
        if (nav_index < Nav().MenuItems().Size()) {
          suppress_nav_ = true;
          Nav().SelectedItem(Nav().MenuItems().GetAt(nav_index));
          suppress_nav_ = false;
        }
        break;
      }
    }
    pending_resume_feed_id_.clear();
  }
  ReloadRecent();
  UpdateDetailVisibility();
}

// ---- episodes ----------------------------------------------------------------

void MainWindow::ReloadEpisodes() {
  auto const feed_id = SelectedFeedId();
  if (feed_id.empty() || !api_) return;
  auto const dispatcher = DispatcherQueue();
  auto const weak = get_weak();
  (void)api_->episode_list_async(
      feed_id,
      [dispatcher, weak](rivet_app::Result<std::vector<std::vector<std::string>>> result) {
        dispatcher.TryEnqueue([weak, result]() mutable {
          if (auto window = weak.get()) {
            try {
              window->RenderEpisodes(result.get());
            } catch (std::exception const& e) {
              window->SetStatus(false, to_wide(e.what()));
            }
          }
        });
      });
}

namespace {

muxc::Border MakeChip(std::wstring const& text) {
  muxc::TextBlock label;
  label.Text(winrt::hstring(text));
  label.FontSize(11);
  label.Foreground(Microsoft::UI::Xaml::Application::Current()
                       .Resources()
                       .Lookup(box_value(winrt::hstring(L"AppAccentBrush")))
                       .as<Microsoft::UI::Xaml::Media::Brush>());
  muxc::Border chip;
  chip.Child(label);
  chip.CornerRadius(winrt::Microsoft::UI::Xaml::CornerRadius{4});
  chip.Padding(winrt::Microsoft::UI::Xaml::Thickness{6, 2, 6, 2});
  chip.Background(Microsoft::UI::Xaml::Application::Current()
                      .Resources()
                      .Lookup(box_value(winrt::hstring(L"AppAccentSoftBrush")))
                      .as<Microsoft::UI::Xaml::Media::Brush>());
  chip.VerticalAlignment(Microsoft::UI::Xaml::VerticalAlignment::Center);
  return chip;
}

}  // namespace

void MainWindow::RenderEpisodes(std::vector<std::vector<std::string>> rows) {
  episodes_.clear();
  segment_borders_.clear();
  translation_blocks_.clear();
  EpisodeList().Items().Clear();

  for (auto const& row : rows) {
    if (row.size() < 11) continue;
    EpisodeRow episode;
    episode.id = row[0];
    episode.title = row[1];
    episode.pub = row[2];
    episode.duration_sec = std::wcstod(to_wide(row[3]).c_str(), nullptr);
    episode.downloaded = row[4] == "1";
    episode.transcript_status = row[5];
    episode.summary_status = row[6];
    episode.position_sec = std::wcstod(to_wide(row[7]).c_str(), nullptr);
    episode.done = row[8] == "1";
    episode.has_translation = row[9] == "1";
    episode.description = row[10];
    episodes_.push_back(episode);

    muxc::TextBlock title;
    title.Text(winrt::hstring(to_wide(episode.title)));
    title.FontSize(15);
    title.FontWeight(Microsoft::UI::Text::FontWeights::SemiBold());
    title.TextWrapping(Microsoft::UI::Xaml::TextWrapping::Wrap);
    title.MaxLines(2);
    title.TextTrimming(Microsoft::UI::Xaml::TextTrimming::CharacterEllipsis);

    std::wstring meta = to_wide(episode.pub);
    if (episode.duration_sec > 0) {
      meta += L" · " + FormatSeconds(episode.duration_sec);
    }
    if (episode.position_sec > 5 && !episode.done) {
      meta += L" · " + std::wstring(podlens::Tr("detail.position_prefix")) +
              FormatSeconds(episode.position_sec);
    }
    muxc::TextBlock meta_text;
    meta_text.Text(winrt::hstring(meta));
    meta_text.FontSize(12);
    meta_text.Foreground(Microsoft::UI::Xaml::Application::Current()
                             .Resources()
                             .Lookup(box_value(winrt::hstring(L"AppInkSoftBrush")))
                             .as<Microsoft::UI::Xaml::Media::Brush>());

    muxc::StackPanel chips;
    chips.Orientation(Microsoft::UI::Xaml::Controls::Orientation::Horizontal);
    chips.Spacing(4);
    if (episode.downloaded) chips.Children().Append(MakeChip(std::wstring(podlens::Tr("badge.downloaded"))));
    if (episode.transcript_status == "done") chips.Children().Append(MakeChip(std::wstring(podlens::Tr("badge.transcript"))));
    if (episode.transcript_status == "running") chips.Children().Append(MakeChip(std::wstring(podlens::Tr("badge.transcript_running"))));
    if (episode.transcript_status == "error") chips.Children().Append(MakeChip(std::wstring(podlens::Tr("badge.transcript_error"))));
    if (episode.summary_status == "done") chips.Children().Append(MakeChip(std::wstring(podlens::Tr("badge.summary"))));
    if (episode.has_translation) chips.Children().Append(MakeChip(std::wstring(podlens::Tr("badge.translated"))));
    if (episode.done) chips.Children().Append(MakeChip(std::wstring(podlens::Tr("badge.done"))));

    muxc::StackPanel lines;
    lines.Spacing(4);
    lines.Children().Append(title);
    lines.Children().Append(meta_text);
    if (chips.Children().Size() > 0) lines.Children().Append(chips);

    muxc::Border card;
    card.Child(lines);
    card.CornerRadius(winrt::Microsoft::UI::Xaml::CornerRadius{8});
    card.Padding(winrt::Microsoft::UI::Xaml::Thickness{14, 10, 14, 10});
    card.Margin(winrt::Microsoft::UI::Xaml::Thickness{0, 0, 0, 8});
    card.Background(Microsoft::UI::Xaml::Application::Current()
                        .Resources()
                        .Lookup(box_value(winrt::hstring(L"AppCardBrush")))
                        .as<Microsoft::UI::Xaml::Media::Brush>());

    // right-click / long-press: mark played or unplayed
    muxc::MenuFlyout flyout;
    muxc::MenuFlyoutItem played_item;
    played_item.Text(std::wstring(podlens::Tr("action.mark_played")));
    played_item.Click([weak = get_weak()](auto&& sender, auto&&) {
      if (auto window = weak.get()) window->MarkFromMenu(sender, L"1");
    });
    muxc::MenuFlyoutItem unplayed_item;
    unplayed_item.Text(std::wstring(podlens::Tr("action.mark_unplayed")));
    unplayed_item.Click([weak = get_weak()](auto&& sender, auto&&) {
      if (auto window = weak.get()) window->MarkFromMenu(sender, L"0");
    });
    flyout.Items().Append(played_item);
    flyout.Items().Append(unplayed_item);
    card.ContextFlyout(flyout);

    EpisodeList().Items().Append(card);
  }

  bool const empty = episodes_.empty();
  EpisodesEmpty().Visibility(empty ? Visibility::Visible : Visibility::Collapsed);
  EpisodeList().Visibility(empty ? Visibility::Collapsed : Visibility::Visible);

  // resume-on-launch: jump to the recorded episode once it exists
  if (!pending_resume_episode_id_.empty()) {
    for (uint32_t i = 0; i < episodes_.size(); ++i) {
      if (episodes_[i].id == pending_resume_episode_id_) {
        EpisodeList().SelectedIndex(i);
        SetStatus(true, std::wstring(podlens::Tr("status.resumed")) + L" " +
                            to_wide(episodes_[i].title));
        break;
      }
    }
    pending_resume_episode_id_.clear();
  }

  UpdateDetailVisibility();
  UpdateActionButtons();
}

void MainWindow::RenderDetailHeader() {
  EpisodeRow const* episode = SelectedEpisode();
  if (!episode) return;
  DetailTitle().Text(winrt::hstring(to_wide(episode->title)));
  std::wstring meta = FormatPubDate(to_wide(episode->pub));
  if (episode->duration_sec > 0) {
    meta += L" · " + FormatSeconds(episode->duration_sec);
  }
  if (episode->position_sec > 5 && !episode->done) {
    meta += L" · " + std::wstring(podlens::Tr("detail.position_prefix")) +
            FormatSeconds(episode->position_sec);
  }
  if (FeedRow const* feed = FeedById(selected_feed_id_)) {
    meta = to_wide(feed->title) + L" · " + meta;
  }
  DetailMeta().Text(winrt::hstring(meta));
  DetailNotes().Text(episode->description.empty()
                         ? L""
                         : winrt::hstring(to_wide(episode->description)));
  MenuMark().Text(std::wstring(podlens::Tr(
      episode->done ? "action.mark_unplayed" : "action.mark_played")));
  DurationText().Text(winrt::hstring(FormatSeconds(episode->duration_sec)));
}

void MainWindow::UpdateActionButtons() {
  EpisodeRow const* episode = SelectedEpisode();
  if (!episode) {
    PipelineButton().IsEnabled(false);
    MenuMark().IsEnabled(false);
    MenuRemoveAudio().Visibility(Visibility::Collapsed);
    return;
  }
  bool const transcript_running = episode->transcript_status == "running";
  PipelineButton().IsEnabled(true);
  MenuMark().IsEnabled(true);
  // Delete Audio only makes sense while local audio exists (Linux parity)
  MenuRemoveAudio().Visibility(episode->downloaded ? Visibility::Visible
                                                   : Visibility::Collapsed);
}

// ---- transcript ----------------------------------------------------------------

void MainWindow::LoadTranscript() {
  if (!api_ || selected_episode_id_.empty()) return;
  auto const dispatcher = DispatcherQueue();
  auto const weak = get_weak();
  auto const episode_id = selected_episode_id_;
  (void)api_->episode_transcript_async(
      episode_id,
      [dispatcher, weak](rivet_app::Result<std::vector<std::vector<std::string>>> result) {
        dispatcher.TryEnqueue([weak, result]() mutable {
          if (auto window = weak.get()) {
            try {
              window->transcript_ = result.get();
            } catch (std::exception const&) {
              window->transcript_.clear();
            }
            window->RenderTranscript();
            window->UpdateDetailVisibility();
          }
        });
      });
}

void MainWindow::RenderTranscript() {
  TranscriptList().Items().Clear();
  segment_borders_.clear();
  segment_buttons_.clear();
  translation_blocks_.clear();
  highlighted_segment_ = -1;

  if (transcript_.empty() && !selected_episode_id_.empty()) {
    muxc::TextBlock hint;
    hint.Text(winrt::hstring(std::wstring(podlens::Tr("detail.empty"))));
    hint.FontSize(14);
    hint.Opacity(0.7);
    hint.TextWrapping(Microsoft::UI::Xaml::TextWrapping::Wrap);
    hint.Margin(winrt::Microsoft::UI::Xaml::Thickness{0, 8, 0, 0});
    TranscriptList().Items().Append(hint);
    return;
  }

  for (size_t i = 0; i < transcript_.size(); ++i) {
    auto const& seg = transcript_[i];
    if (seg.size() < 4) continue;

    muxc::TextBlock time_text;
    time_text.Text(winrt::hstring(FormatSeconds(std::wcstod(to_wide(seg[0]).c_str(), nullptr))));
    time_text.FontSize(12);
    time_text.FontFamily(Microsoft::UI::Xaml::Media::FontFamily(winrt::hstring(L"Consolas")));
    time_text.VerticalAlignment(Microsoft::UI::Xaml::VerticalAlignment::Top);
    time_text.Foreground(Microsoft::UI::Xaml::Application::Current()
                             .Resources()
                             .Lookup(box_value(winrt::hstring(L"AppInkSoftBrush")))
                             .as<Microsoft::UI::Xaml::Media::Brush>());

    muxc::TextBlock original;
    original.Text(winrt::hstring(to_wide(seg[2])));
    original.FontSize(15);
    original.TextWrapping(Microsoft::UI::Xaml::TextWrapping::Wrap);
    original.IsTextSelectionEnabled(true);

    muxc::TextBlock translation;
    translation.Text(winrt::hstring(to_wide(seg[3])));
    translation.FontSize(13);
    translation.TextWrapping(Microsoft::UI::Xaml::TextWrapping::Wrap);
    translation.Foreground(Microsoft::UI::Xaml::Application::Current()
                               .Resources()
                               .Lookup(box_value(winrt::hstring(L"AppInkSoftBrush")))
                               .as<Microsoft::UI::Xaml::Media::Brush>());
    translation.Visibility(bilingual_ ? Visibility::Visible : Visibility::Collapsed);

    muxc::StackPanel lines;
    lines.Spacing(3);
    if (!seg[2].empty()) lines.Children().Append(original);
    if (!seg[3].empty()) lines.Children().Append(translation);
    if (lines.Children().Size() == 0) continue;

    if (!seg[3].empty()) translation_blocks_.push_back(translation);

    muxc::Grid grid;
    grid.ColumnSpacing(10);
    grid.ColumnDefinitions().Append(muxc::ColumnDefinition());
    grid.ColumnDefinitions().Append(muxc::ColumnDefinition());
    grid.ColumnDefinitions().GetAt(0).Width({64, Microsoft::UI::Xaml::GridUnitType::Pixel});
    grid.ColumnDefinitions().GetAt(1).Width({1, Microsoft::UI::Xaml::GridUnitType::Star});
    muxc::Grid::SetColumn(time_text, 0);
    muxc::Grid::SetColumn(lines, 1);
    grid.Children().Append(time_text);
    grid.Children().Append(lines);

    muxc::Border row;
    row.Child(grid);
    row.CornerRadius(winrt::Microsoft::UI::Xaml::CornerRadius{6});
    row.Padding(winrt::Microsoft::UI::Xaml::Thickness{10, 6, 10, 6});

    muxc::Button seek_button;
    seek_button.Content(row);
    seek_button.HorizontalContentAlignment(Microsoft::UI::Xaml::HorizontalAlignment::Stretch);
    seek_button.HorizontalAlignment(Microsoft::UI::Xaml::HorizontalAlignment::Stretch);
    seek_button.Background(nullptr);
    seek_button.BorderThickness(winrt::Microsoft::UI::Xaml::Thickness{0});
    seek_button.Padding(winrt::Microsoft::UI::Xaml::Thickness{0});
    seek_button.Margin(winrt::Microsoft::UI::Xaml::Thickness{0, 0, 0, 2});
    seek_button.Tag(box_value(winrt::hstring(std::to_wstring(i))));
    winrt::Microsoft::UI::Xaml::Automation::AutomationProperties::SetAutomationId(
        seek_button,
        winrt::hstring(L"TranscriptSegment-" + std::to_wstring(i)));
    winrt::Microsoft::UI::Xaml::Automation::AutomationProperties::SetHelpText(
        seek_button,
        winrt::hstring(std::wstring(podlens::Tr("transcript.seek_hint"))));
    muxc::ToolTipService::SetToolTip(
        seek_button,
        box_value(winrt::hstring(std::wstring(podlens::Tr("transcript.seek_hint")))));
    seek_button.Click([weak = get_weak()](winrt::Windows::Foundation::IInspectable const& sender,
                                         Microsoft::UI::Xaml::RoutedEventArgs const&) {
      auto const button = sender.as<muxc::Button>();
      auto const index = std::stoi(std::wstring(
          winrt::unbox_value<winrt::hstring>(button.Tag())));
      if (auto window = weak.get()) {
        if (window->player_ && index < static_cast<int>(window->transcript_.size())) {
          auto const& seg = window->transcript_[static_cast<size_t>(index)];
          double const start = std::wcstod(to_wide(seg[0]).c_str(), nullptr);
          window->follow_transcript_ = true;
          window->FollowTranscriptButton().IsChecked(true);
          window->SeekTo(start);
        }
      }
    });
    segment_borders_.push_back(row);
    segment_buttons_.push_back(seek_button);
    TranscriptList().Items().Append(seek_button);
  }
}

void MainWindow::ApplyMode() {
  for (auto const& block : translation_blocks_) {
    block.Visibility(bilingual_ ? Visibility::Visible : Visibility::Collapsed);
  }
}

void MainWindow::HighlightRunningSegment(double position_seconds) {
  int running = -1;
  for (size_t i = 0; i < transcript_.size(); ++i) {
    auto const& seg = transcript_[i];
    if (seg.size() < 4) continue;
    double const start = std::wcstod(to_wide(seg[0]).c_str(), nullptr);
    double const end = std::wcstod(to_wide(seg[1]).c_str(), nullptr);
    if (position_seconds >= start && position_seconds < end) {
      running = static_cast<int>(i);
      break;
    }
  }
  if (running == highlighted_segment_) return;

  if (highlighted_segment_ >= 0 &&
      highlighted_segment_ < static_cast<int>(segment_borders_.size())) {
    segment_borders_[static_cast<size_t>(highlighted_segment_)].Background(nullptr);
  }
  highlighted_segment_ = running;
  if (running >= 0) {
    auto& border = segment_borders_[static_cast<size_t>(running)];
    border.Background(Microsoft::UI::Xaml::Application::Current()
                          .Resources()
                          .Lookup(box_value(winrt::hstring(L"AppAccentSoftBrush")))
                          .as<Microsoft::UI::Xaml::Media::Brush>());
    if (follow_transcript_ && running < static_cast<int>(segment_buttons_.size())) {
      TranscriptList().ScrollIntoView(segment_buttons_[static_cast<size_t>(running)]);
    }
  }
}

void MainWindow::ScrollToCurrentTranscript() {
  if (highlighted_segment_ < 0 ||
      highlighted_segment_ >= static_cast<int>(segment_buttons_.size())) {
    return;
  }
  TranscriptList().ScrollIntoView(
      segment_buttons_[static_cast<size_t>(highlighted_segment_)]);
  segment_buttons_[static_cast<size_t>(highlighted_segment_)].Focus(
      Microsoft::UI::Xaml::FocusState::Keyboard);
}

// ---- summary -------------------------------------------------------------------

void MainWindow::LoadSummary(std::string const& episode_id) {
  if (!api_ || episode_id.empty()) return;
  auto const dispatcher = DispatcherQueue();
  auto const weak = get_weak();
  (void)api_->episode_summary_async(
      episode_id,
      [dispatcher, weak, episode_id](rivet_app::Result<std::string> result) {
        dispatcher.TryEnqueue([weak, result, episode_id]() mutable {
          if (auto window = weak.get()) {
            try {
              window->summary_by_episode_[episode_id] = result.get();
            } catch (std::exception const&) {
              // empty summary stays absent
            }
            window->RenderSummary();
          }
        });
      });
}

namespace {

muxc::TextBlock SectionLabel(std::wstring const& text) {
  muxc::TextBlock label;
  label.Text(winrt::hstring(text));
  label.FontSize(13);
  label.FontWeight(Microsoft::UI::Text::FontWeights::SemiBold());
  label.Foreground(Microsoft::UI::Xaml::Application::Current()
                       .Resources()
                       .Lookup(box_value(winrt::hstring(L"AppAccentBrush")))
                       .as<Microsoft::UI::Xaml::Media::Brush>());
  return label;
}

}  // namespace

void MainWindow::RenderSummary() {
  SummaryList().Children().Clear();
  auto const found = summary_by_episode_.find(selected_episode_id_);
  if (found == summary_by_episode_.end() || found->second.empty()) {
    muxc::TextBlock hint;
    hint.Text(winrt::hstring(std::wstring(podlens::Tr("summary.empty"))));
    hint.FontSize(14);
    hint.Opacity(0.7);
    SummaryList().Children().Append(hint);
    return;
  }

  auto const json = to_wide(found->second);

  auto const tldr = json_string_value(json, L"tldr");
  if (!tldr.empty()) {
    SummaryList().Children().Append(SectionLabel(L"TL;DR"));
    muxc::TextBlock body;
    body.Text(winrt::hstring(tldr));
    body.FontSize(16);
    body.TextWrapping(Microsoft::UI::Xaml::TextWrapping::Wrap);
    SummaryList().Children().Append(body);
  }

  auto const points = json_array_strings(json, L"key-points");
  if (!points.empty()) {
    SummaryList().Children().Append(SectionLabel(std::wstring(podlens::Tr("summary.points"))));
    for (auto const& point : points) {
      muxc::TextBlock line;
      line.Text(winrt::hstring(L"· " + point));
      line.FontSize(14);
      line.TextWrapping(Microsoft::UI::Xaml::TextWrapping::Wrap);
      SummaryList().Children().Append(line);
    }
  }

  auto const quotes = json_array_objects(json, L"quotes");
  if (!quotes.empty()) {
    SummaryList().Children().Append(SectionLabel(std::wstring(podlens::Tr("summary.quotes"))));
    for (auto const& [text, translation] : quotes) {
      muxc::StackPanel quote;
      quote.Spacing(2);
      quote.Margin(winrt::Microsoft::UI::Xaml::Thickness{12, 0, 0, 6});
      muxc::TextBlock original;
      original.Text(winrt::hstring(L"“" + text + L"”"));
      original.FontSize(15);
      original.TextWrapping(Microsoft::UI::Xaml::TextWrapping::Wrap);
      quote.Children().Append(original);
      if (!translation.empty()) {
        muxc::TextBlock translated;
        translated.Text(winrt::hstring(translation));
        translated.FontSize(13);
        translated.TextWrapping(Microsoft::UI::Xaml::TextWrapping::Wrap);
        translated.Opacity(0.75);
        quote.Children().Append(translated);
      }
      SummaryList().Children().Append(quote);
    }
  }

  auto const topics = json_array_strings(json, L"topics");
  if (!topics.empty()) {
    SummaryList().Children().Append(SectionLabel(std::wstring(podlens::Tr("summary.topics"))));
    muxc::StackPanel chips;
    chips.Orientation(Microsoft::UI::Xaml::Controls::Orientation::Horizontal);
    chips.Spacing(6);
    for (auto const& topic : topics) {
      chips.Children().Append(MakeChip(topic));
    }
    SummaryList().Children().Append(chips);
  }
}

// ---- cross-feed lanes (home / library pages) ---------------------------------

void MainWindow::ReloadRecent() {
  if (!api_) return;
  auto const dispatcher = DispatcherQueue();
  auto const weak = get_weak();
  (void)api_->episode_recent_async(
      [dispatcher, weak](rivet_app::Result<std::vector<std::vector<std::string>>> result) {
        dispatcher.TryEnqueue([weak, result]() mutable {
          if (auto window = weak.get()) {
            try {
              window->RenderRecent(result.get());
            } catch (std::exception const& e) {
              window->SetStatus(false, to_wide(e.what()));
            }
          }
        });
      });
}

void MainWindow::RenderRecent(std::vector<std::vector<std::string>> rows) {
  recent_.clear();
  for (auto const& row : rows) {
    if (row.size() < 14) continue;
    RecentRow item;
    item.feed_id = row[0];
    item.feed_title = row[1];
    item.artwork = row[2];
    item.episode.id = row[3];
    item.episode.title = row[4];
    item.episode.pub = row[5];
    item.episode.duration_sec = std::wcstod(to_wide(row[6]).c_str(), nullptr);
    item.episode.downloaded = row[7] == "1";
    item.episode.transcript_status = row[8];
    item.episode.summary_status = row[9];
    item.episode.position_sec = std::wcstod(to_wide(row[10]).c_str(), nullptr);
    item.episode.done = row[11] == "1";
    item.episode.has_translation = row[12] == "1";
    item.episode.description = row[13];
    recent_.push_back(item);
  }
  if (page_ == Page::Home) RenderHomePage();
  if (page_ == Page::Recent) RenderRecentPage();
  if (page_ == Page::Downloaded) RenderDownloadedPage();
}

// The Up Next lane: in-progress episodes first, then each show's newest
// unplayed episode — Apple Podcasts' queue semantics for a library with
// no explicit queue.
std::vector<MainWindow::RecentRow> MainWindow::UpNext() const {
  std::vector<RecentRow> lane;
  std::set<std::string> seen_feeds;
  for (auto const& item : recent_) {
    if (item.episode.done) continue;
    if (item.episode.position_sec > 5) {
      lane.push_back(item);
      seen_feeds.insert(item.feed_id);
    }
  }
  for (auto const& item : recent_) {
    if (item.episode.done) continue;
    if (seen_feeds.count(item.feed_id) == 0) {
      lane.push_back(item);
      seen_feeds.insert(item.feed_id);
    }
    if (lane.size() >= 12) break;
  }
  return lane;
}

MainWindow::FeedRow const* MainWindow::FeedById(std::string const& id) const {
  for (auto const& feed : feeds_) {
    if (feed.id == id) return &feed;
  }
  return nullptr;
}

// The selected episode may come from any lane; the feed page's episodes_
// vector only carries it once the owning feed's list has loaded.
MainWindow::EpisodeRow const* MainWindow::SelectedEpisode() const {
  for (auto const& candidate : episodes_) {
    if (candidate.id == selected_episode_id_) return &candidate;
  }
  for (auto const& item : recent_) {
    if (item.episode.id == selected_episode_id_) return &item.episode;
  }
  return nullptr;
}

// "Fri, 16 Oct 2026 21:30:00 +0000" -> "10月17日" / "Oct 17"; anything the
// scanner cannot read passes through untouched.
std::wstring MainWindow::FormatPubDate(std::wstring const& raw) const {
  std::string const utf8 = to_utf8(raw);
  std::tm tm{};
  char const* const formats[] = {"%a, %d %b %Y %H:%M:%S %Z",
                                 "%a, %d %b %Y %H:%M:%S %z",
                                 "%Y-%m-%d %H:%M:%S %z",
                                 "%a, %d %b %Y"};
  bool parsed = false;
  for (char const* format : formats) {
    std::istringstream stream(utf8);
    stream.imbue(std::locale::classic());
    if (stream >> std::get_time(&tm, format)) {
      parsed = true;
      break;
    }
  }
  if (!parsed) return raw;
  wchar_t buffer[32];
  std::wstring const month_names[] = {
      L"", L"Jan", L"Feb", L"Mar", L"Apr", L"May", L"Jun",
      L"Jul", L"Aug", L"Sep", L"Oct", L"Nov", L"Dec"};
  int const month = std::max(0, std::min(11, tm.tm_mon));
  if (podlens::kUiLang == podlens::Lang::Zh) {
    swprintf(buffer, 32, L"%d月%d日", tm.tm_mon + 1, tm.tm_mday);
  } else {
    swprintf(buffer, 32, L"%s %d", month_names[month].c_str(), tm.tm_mday);
  }
  return buffer;
}

// Cover art with a quiet accent fallback; BitmapImage streams the URL on
// its own so a slow CDN never blocks the list.
muxc::Border MainWindow::MakeArtwork(std::wstring const& url, double size) {
  muxc::Border host;
  host.Width(size);
  host.Height(size);
  host.CornerRadius(winrt::Microsoft::UI::Xaml::CornerRadius{6});
  host.Background(Microsoft::UI::Xaml::Application::Current()
                      .Resources()
                      .Lookup(box_value(winrt::hstring(L"AppAccentSoftBrush")))
                      .as<Microsoft::UI::Xaml::Media::Brush>());
  if (!url.empty()) {
    muxc::Image image;
    image.Stretch(Microsoft::UI::Xaml::Media::Stretch::UniformToFill);
    try {
      winrt::Microsoft::UI::Xaml::Media::Imaging::BitmapImage bitmap{
          winrt::Windows::Foundation::Uri(url)};
      image.Source(bitmap);
    } catch (...) {
    }
    host.Child(image);
  }
  return host;
}

// The shared cross-feed episode card: artwork, title, show/date/duration
// meta, status chips, notes, progress and a play button.
muxc::Border MainWindow::MakeEpisodeCard(RecentRow const& item, bool show_feed) {
  muxc::Image artwork;
  artwork.Width(56);
  artwork.Height(56);
  artwork.Stretch(Microsoft::UI::Xaml::Media::Stretch::UniformToFill);
  if (!item.artwork.empty()) {
    try {
      artwork.Source(winrt::Microsoft::UI::Xaml::Media::Imaging::BitmapImage(
          winrt::Windows::Foundation::Uri(to_wide(item.artwork))));
    } catch (...) {
    }
  }
  muxc::Border artwork_host;
  artwork_host.Child(artwork);
  artwork_host.CornerRadius(winrt::Microsoft::UI::Xaml::CornerRadius{6});
  artwork_host.Background(Microsoft::UI::Xaml::Application::Current()
                              .Resources()
                              .Lookup(box_value(winrt::hstring(L"AppAccentSoftBrush")))
                              .as<Microsoft::UI::Xaml::Media::Brush>());
  artwork_host.VerticalAlignment(Microsoft::UI::Xaml::VerticalAlignment::Top);

  muxc::TextBlock title;
  title.Text(winrt::hstring(to_wide(item.episode.title)));
  title.FontSize(13);
  title.FontWeight(Microsoft::UI::Text::FontWeights::SemiBold());
  title.TextWrapping(Microsoft::UI::Xaml::TextWrapping::Wrap);
  title.MaxLines(2);

  std::wstring meta;
  if (show_feed) meta += to_wide(item.feed_title) + L" · ";
  meta += FormatPubDate(to_wide(item.episode.pub));
  if (item.episode.duration_sec > 0) {
    meta += L" · " + FormatSeconds(item.episode.duration_sec);
  }
  muxc::TextBlock meta_text;
  meta_text.Text(winrt::hstring(meta));
  meta_text.FontSize(12);
  meta_text.Foreground(Microsoft::UI::Xaml::Application::Current()
                           .Resources()
                           .Lookup(box_value(winrt::hstring(L"AppInkSoftBrush")))
                           .as<Microsoft::UI::Xaml::Media::Brush>());

  muxc::StackPanel chips;
  chips.Orientation(Microsoft::UI::Xaml::Controls::Orientation::Horizontal);
  chips.Spacing(4);
  if (item.episode.done) chips.Children().Append(MakeChip(std::wstring(podlens::Tr("badge.done"))));
  if (item.episode.downloaded) chips.Children().Append(MakeChip(std::wstring(podlens::Tr("badge.downloaded"))));
  if (item.episode.transcript_status == "done") chips.Children().Append(MakeChip(std::wstring(podlens::Tr("badge.transcript"))));
  if (item.episode.transcript_status == "running") chips.Children().Append(MakeChip(std::wstring(podlens::Tr("badge.transcript_running"))));
  if (item.episode.has_translation) chips.Children().Append(MakeChip(std::wstring(podlens::Tr("badge.translated"))));
  if (item.episode.summary_status == "done") chips.Children().Append(MakeChip(std::wstring(podlens::Tr("badge.summary"))));

  muxc::TextBlock notes;
  notes.Text(winrt::hstring(to_wide(item.episode.description)));
  notes.FontSize(12);
  notes.Opacity(0.55);
  notes.MaxLines(1);
  notes.TextTrimming(Microsoft::UI::Xaml::TextTrimming::CharacterEllipsis);

  muxc::StackPanel lines;
  lines.Spacing(4);
  lines.Children().Append(title);
  lines.Children().Append(meta_text);
  muxc::StackPanel chips_row;
  chips_row.Orientation(Microsoft::UI::Xaml::Controls::Orientation::Horizontal);
  chips_row.Spacing(10);
  if (chips.Children().Size() > 0) chips_row.Children().Append(chips);
  if (!item.episode.description.empty()) chips_row.Children().Append(notes);
  if (chips_row.Children().Size() > 0) lines.Children().Append(chips_row);
  if (item.episode.position_sec > 5 && !item.episode.done) {
    muxc::ProgressBar progress;
    progress.Maximum(item.episode.duration_sec > 0 ? item.episode.duration_sec : 1);
    progress.Value(item.episode.position_sec);
    progress.Margin(winrt::Microsoft::UI::Xaml::Thickness{0, 2, 0, 0});
    lines.Children().Append(progress);
  }

  muxc::Button play;
  play.Width(36);
  play.Height(36);
  play.CornerRadius(winrt::Microsoft::UI::Xaml::CornerRadius{18});
  play.Background(Microsoft::UI::Xaml::Application::Current()
                      .Resources()
                      .Lookup(box_value(winrt::hstring(L"AppAccentSoftBrush")))
                      .as<Microsoft::UI::Xaml::Media::Brush>());
  play.BorderThickness(winrt::Microsoft::UI::Xaml::Thickness{0});
  muxc::FontIcon play_glyph;
  play_glyph.Glyph(winrt::hstring(L"\xE768"));
  play_glyph.FontSize(14);
  play.Content(box_value(play_glyph));
  play.VerticalAlignment(Microsoft::UI::Xaml::VerticalAlignment::Center);
  play.Click([weak = get_weak(), item](auto&&, auto&&) {
    if (auto window = weak.get()) window->SelectRecent(item, true);
  });

  muxc::Grid layout;
  layout.ColumnSpacing(12);
  layout.ColumnDefinitions().Append(muxc::ColumnDefinition());
  layout.ColumnDefinitions().Append(muxc::ColumnDefinition());
  layout.ColumnDefinitions().GetAt(0).Width({1, Microsoft::UI::Xaml::GridUnitType::Star});
  layout.ColumnDefinitions().GetAt(1).Width({0, Microsoft::UI::Xaml::GridUnitType::Auto});
  muxc::StackPanel left;
  left.Orientation(Microsoft::UI::Xaml::Controls::Orientation::Horizontal);
  left.Spacing(12);
  left.Children().Append(artwork_host);
  left.Children().Append(lines);
  muxc::Grid::SetColumn(left, 0);
  muxc::Grid::SetColumn(play, 1);
  layout.Children().Append(left);
  layout.Children().Append(play);

  muxc::Border card;
  card.Child(layout);
  card.CornerRadius(winrt::Microsoft::UI::Xaml::CornerRadius{8});
  card.Padding(winrt::Microsoft::UI::Xaml::Thickness{12, 10, 12, 10});
  card.Margin(winrt::Microsoft::UI::Xaml::Thickness{0, 0, 0, 8});
  card.Background(Microsoft::UI::Xaml::Application::Current()
                      .Resources()
                      .Lookup(box_value(winrt::hstring(L"AppCardBrush")))
                      .as<Microsoft::UI::Xaml::Media::Brush>());
  RecentRow const row_copy = item;
  card.Tapped([weak = get_weak(), row_copy](auto&&, auto&&) {
    if (auto window = weak.get()) window->SelectRecent(row_copy, false);
  });
  return card;
}

void MainWindow::SelectRecent(RecentRow const& row, bool autoplay) {
  SelectEpisodeContext(row.feed_id, row.episode.id, autoplay);
}

void MainWindow::SelectEpisodeContext(std::string const& feed_id,
                                      std::string const& episode_id,
                                      bool autoplay) {
  if (!selected_episode_id_.empty() && selected_episode_id_ != episode_id) {
    StopPlayback(true);
  }
  selected_feed_id_ = feed_id;
  selected_episode_id_ = episode_id;
  ReloadEpisodes();
  transcript_.clear();
  RenderTranscript();
  RenderSummary();
  RenderDetailHeader();
  UpdateDetailVisibility();
  UpdateActionButtons();
  UpdatePlayerInfo();
  if (!episode_id.empty()) {
    LoadTranscript();
    LoadSummary(episode_id);
    LoadChapters();
  }
  if (autoplay) StartPlayback();
}

// Up Next card: full-bleed cover, gradient scrim, lane meta + title; the
// whole card plays.
muxc::Border MainWindow::MakeUpNextCard(RecentRow const& item) {
  muxc::Image artwork;
  artwork.Stretch(Microsoft::UI::Xaml::Media::Stretch::UniformToFill);
  if (!item.artwork.empty()) {
    try {
      artwork.Source(winrt::Microsoft::UI::Xaml::Media::Imaging::BitmapImage(
          winrt::Windows::Foundation::Uri(to_wide(item.artwork))));
    } catch (...) {
    }
  }

  std::wstring const lane = item.episode.position_sec > 5
                                ? std::wstring(podlens::Tr("meta.continue"))
                                : std::wstring(podlens::Tr("meta.new_episode"));
  std::wstring const meta_text =
      item.episode.pub.empty()
          ? lane
          : lane + L" · " + FormatPubDate(to_wide(item.episode.pub));

  muxc::StackPanel overlay;
  overlay.Padding(winrt::Microsoft::UI::Xaml::Thickness{12});
  overlay.VerticalAlignment(Microsoft::UI::Xaml::VerticalAlignment::Bottom);
  muxc::TextBlock meta;
  meta.Text(winrt::hstring(meta_text));
  meta.FontSize(12);
  meta.Foreground(Microsoft::UI::Xaml::Media::SolidColorBrush(
      winrt::Windows::UI::Color{0xFF, 0xFF, 0xFF, 0xFF}));
  muxc::TextBlock title;
  title.Text(winrt::hstring(to_wide(item.episode.title)));
  title.FontSize(14);
  title.FontWeight(Microsoft::UI::Text::FontWeights::SemiBold());
  title.Foreground(Microsoft::UI::Xaml::Media::SolidColorBrush(
      winrt::Windows::UI::Color{0xFF, 0xFF, 0xFF, 0xFF}));
  title.TextWrapping(Microsoft::UI::Xaml::TextWrapping::Wrap);
  title.MaxLines(2);
  overlay.Children().Append(meta);
  overlay.Children().Append(title);

  Microsoft::UI::Xaml::Media::LinearGradientBrush scrim;
  Microsoft::UI::Xaml::Media::GradientStop top;
  top.Color(winrt::Windows::UI::Color{0x00, 0x00, 0x00, 0x00});
  top.Offset(0.0);
  Microsoft::UI::Xaml::Media::GradientStop bottom;
  bottom.Color(winrt::Windows::UI::Color{0xC8, 0x00, 0x00, 0x00});
  bottom.Offset(1.0);
  scrim.GradientStops().Append(top);
  scrim.GradientStops().Append(bottom);
  overlay.Background(scrim);

  muxc::Grid card_grid;
  card_grid.Width(230);
  card_grid.Height(230);
  card_grid.Children().Append(artwork);
  card_grid.Children().Append(overlay);
  muxc::Border card;
  card.Child(card_grid);
  card.CornerRadius(winrt::Microsoft::UI::Xaml::CornerRadius{12});
  card.Margin(winrt::Microsoft::UI::Xaml::Thickness{0, 4, 8, 4});
  RecentRow const row_copy = item;
  card.Tapped([weak = get_weak(), row_copy](auto&&, auto&&) {
    if (auto window = weak.get()) window->SelectRecent(row_copy, true);
  });
  return card;
}

muxc::TextBlock MainWindow::MakeSectionHeader(std::wstring const& title) {
  auto label = SectionLabel(title);
  label.Margin(winrt::Microsoft::UI::Xaml::Thickness{0, 14, 0, 4});
  return label;
}

muxc::Button MainWindow::MakeSeeAllButton(wchar_t const* label) {
  muxc::Button button;
  button.Background(Microsoft::UI::Xaml::Application::Current()
                        .Resources()
                        .Lookup(box_value(winrt::hstring(L"AppAccentSoftBrush")))
                        .as<Microsoft::UI::Xaml::Media::Brush>());
  button.BorderThickness(winrt::Microsoft::UI::Xaml::Thickness{0});
  button.Padding(winrt::Microsoft::UI::Xaml::Thickness{10, 2, 10, 2});
  button.Content(box_value(winrt::hstring(std::wstring(label) + L" →")));
  return button;
}

muxc::StackPanel MainWindow::MakeShowTile(FeedRow const& feed) {
  muxc::StackPanel tile;
  tile.Width(130);
  tile.Margin(winrt::Microsoft::UI::Xaml::Thickness{0, 0, 12, 12});
  auto art = MakeArtwork(to_wide(feed.artwork), 130);
  art.CornerRadius(winrt::Microsoft::UI::Xaml::CornerRadius{10});
  tile.Children().Append(art);
  muxc::TextBlock title;
  title.Text(winrt::hstring(to_wide(feed.title)));
  title.Margin(winrt::Microsoft::UI::Xaml::Thickness{0, 6, 0, 0});
  title.MaxLines(1);
  title.TextTrimming(Microsoft::UI::Xaml::TextTrimming::CharacterEllipsis);
  tile.Children().Append(title);
  wchar_t count_text[32];
  swprintf(count_text, 32, podlens::Tr("shows.count").data(),
           std::atoi(feed.count.c_str()));
  muxc::TextBlock count_label;
  count_label.Text(winrt::hstring(count_text));
  count_label.FontSize(12);
  count_label.Opacity(0.65);
  tile.Children().Append(count_label);
  return tile;
}

void MainWindow::RenderHomePage() {
  HomeList().Children().Clear();
  if (recent_.empty() && feeds_.empty()) return;

  auto const lane = UpNext();
  if (!lane.empty()) {
    HomeList().Children().Append(
        MakeSectionHeader(std::wstring(podlens::Tr("home.up_next"))));
    muxc::ScrollViewer lane_scroller;
    lane_scroller.HorizontalScrollBarVisibility(
        Microsoft::UI::Xaml::Controls::ScrollBarVisibility::Auto);
    lane_scroller.VerticalScrollBarVisibility(
        Microsoft::UI::Xaml::Controls::ScrollBarVisibility::Disabled);
    muxc::StackPanel lane_row;
    lane_row.Orientation(Microsoft::UI::Xaml::Controls::Orientation::Horizontal);
    lane_scroller.Content(lane_row);
    lane_scroller.Margin(winrt::Microsoft::UI::Xaml::Thickness{0, 4, 0, 6});
    for (auto const& item : lane) {
      lane_row.Children().Append(MakeUpNextCard(item));
    }
    HomeList().Children().Append(lane_scroller);
  }

  muxc::StackPanel shows_header;
  shows_header.Orientation(Microsoft::UI::Xaml::Controls::Orientation::Horizontal);
  shows_header.Spacing(8);
  shows_header.Margin(winrt::Microsoft::UI::Xaml::Thickness{0, 14, 0, 0});
  shows_header.Children().Append(
      MakeSectionHeader(std::wstring(podlens::Tr("home.my_shows"))));
  auto see_shows = MakeSeeAllButton(podlens::Tr("library.shows").data());
  see_shows.Click([weak = get_weak()](auto&&, auto&&) {
    if (auto window = weak.get()) window->ShowPage(Page::Shows);
  });
  shows_header.Children().Append(see_shows);
  HomeList().Children().Append(shows_header);

  muxc::StackPanel shows_row;
  shows_row.Orientation(Microsoft::UI::Xaml::Controls::Orientation::Horizontal);
  muxc::ScrollViewer shows_scroller;
  shows_scroller.HorizontalScrollBarVisibility(
      Microsoft::UI::Xaml::Controls::ScrollBarVisibility::Auto);
  shows_scroller.VerticalScrollBarVisibility(
      Microsoft::UI::Xaml::Controls::ScrollBarVisibility::Disabled);
  shows_scroller.Content(shows_row);
  shows_scroller.Margin(winrt::Microsoft::UI::Xaml::Thickness{0, 4, 0, 0});
  for (auto const& feed : feeds_) {
    auto tile = MakeShowTile(feed);
    std::string const feed_id = feed.id;
    tile.Tapped([weak = get_weak(), feed_id](auto&&, auto&&) {
      if (auto window = weak.get()) window->SelectFeedById(feed_id);
    });
    shows_row.Children().Append(tile);
  }
  HomeList().Children().Append(shows_scroller);

  if (!recent_.empty()) {
    HomeList().Children().Append(
        MakeSectionHeader(std::wstring(podlens::Tr("home.latest"))));
    int shown = 0;
    for (auto const& item : recent_) {
      HomeList().Children().Append(MakeEpisodeCard(item, true));
      if (++shown >= 6) break;
    }
  }
}

void MainWindow::RenderRecentPage() {
  RecentList().Children().Clear();
  if (recent_.empty()) {
    muxc::TextBlock empty;
    empty.Text(winrt::hstring(std::wstring(podlens::Tr("page.recent_empty"))));
    empty.Opacity(0.7);
    empty.Margin(winrt::Microsoft::UI::Xaml::Thickness{0, 24, 0, 0});
    RecentList().Children().Append(empty);
    return;
  }
  for (auto const& item : recent_) {
    RecentList().Children().Append(MakeEpisodeCard(item, true));
  }
}

void MainWindow::RenderDownloadedPage() {
  DownloadedList().Children().Clear();
  std::vector<RecentRow> rows;
  for (auto const& item : recent_) {
    if (item.episode.downloaded) rows.push_back(item);
  }
  if (rows.empty()) {
    muxc::TextBlock empty;
    empty.Text(winrt::hstring(std::wstring(podlens::Tr("page.downloaded_empty"))));
    empty.Opacity(0.7);
    empty.Margin(winrt::Microsoft::UI::Xaml::Thickness{0, 24, 0, 0});
    DownloadedList().Children().Append(empty);
    return;
  }
  for (auto const& item : rows) {
    DownloadedList().Children().Append(MakeEpisodeCard(item, true));
  }
}

void MainWindow::RenderShowsPage() {
  ShowsGrid().Items().Clear();
  for (auto const& feed : feeds_) {
    auto tile = MakeShowTile(feed);
    std::string const feed_id = feed.id;
    tile.Width(150);
    tile.Margin(winrt::Microsoft::UI::Xaml::Thickness{4, 4, 16, 16});
    tile.Tapped([weak = get_weak(), feed_id](auto&&, auto&&) {
      if (auto window = weak.get()) window->SelectFeedById(feed_id);
    });
    ShowsGrid().Items().Append(tile);
  }
}

void MainWindow::SelectFeedById(std::string const& feed_id) {
  selected_feed_id_ = feed_id;
  ReloadEpisodes();
  ShowPage(Page::Episodes);
  for (uint32_t i = 0; i < Nav().MenuItems().Size(); ++i) {
    if (auto const item =
            Nav().MenuItems().GetAt(i).try_as<muxc::NavigationViewItem>()) {
      auto const tag = winrt::unbox_value_or<winrt::hstring>(item.Tag(), {});
      if (to_utf8(std::wstring(tag)) == feed_id) {
        suppress_nav_ = true;
        Nav().SelectedItem(item);
        suppress_nav_ = false;
        break;
      }
    }
  }
}

void MainWindow::UpdatePlayerInfo() {
  for (auto const& item : recent_) {
    if (item.episode.id == selected_episode_id_) {
      PlayerTitle().Text(winrt::hstring(to_wide(item.episode.title)));
      PlayerShow().Text(winrt::hstring(to_wide(item.feed_title)));
      try {
        PlayerArtwork().Source(
            winrt::Microsoft::UI::Xaml::Media::Imaging::BitmapImage(
                winrt::Windows::Foundation::Uri(to_wide(item.artwork))));
      } catch (...) {
      }
      return;
    }
  }
  for (auto const& episode : episodes_) {
    if (episode.id == selected_episode_id_) {
      PlayerTitle().Text(winrt::hstring(to_wide(episode.title)));
    }
  }
  if (FeedRow const* feed = FeedById(selected_feed_id_)) {
    PlayerShow().Text(winrt::hstring(to_wide(feed->title)));
    try {
      PlayerArtwork().Source(
          winrt::Microsoft::UI::Xaml::Media::Imaging::BitmapImage(
              winrt::Windows::Foundation::Uri(to_wide(feed->artwork))));
    } catch (...) {
    }
  }
}

// ---- playback -------------------------------------------------------------------

void MainWindow::StartPlayback() {
  auto const episode_id = selected_episode_id_;
  if (episode_id.empty()) return;
  preview_active_ = false;
  auto const audio = FindLocalAudio(episode_id);
  if (audio.empty()) {
    SetStatus(true, std::wstring(podlens::Tr("status.need_download")));
    StartJob("download", episode_id);
    return;
  }
  try {
    auto file =
        winrt::Windows::Storage::StorageFile::GetFileFromPathAsync(audio).get();
    player_.Source(
        winrt::Windows::Media::Core::MediaSource::CreateFromStorageFile(file));
    duration_ = 0;
    ticks_since_save_ = 0;

    EpisodeRow const* episode = SelectedEpisode();
    if (episode && episode->position_sec > 5 && !episode->done) {
      player_.Position(
          std::chrono::duration_cast<winrt::Windows::Foundation::TimeSpan>(
              std::chrono::duration<double>(episode->position_sec)));
    }
    double rate = 1.0;
    if (auto const entry =
            RateSelector().SelectedItem().try_as<winrt::hstring>()) {
      rate = std::wcstod(entry->c_str(), nullptr);
      if (rate <= 0) rate = 1.0;
    }
    player_.PlaybackRate(static_cast<double>(rate));
    player_.Play();
    PlayPauseGlyph().Glyph(winrt::hstring(L"\xE769"));
    position_timer_.Start();
    SetSmtcStatus(true);
    UpdateSmtcMetadata();
  } catch (winrt::hresult_error const& e) {
    SetStatus(false, e.message().c_str());
  } catch (std::exception const& e) {
    SetStatus(false, to_wide(e.what()));
  }
}

void MainWindow::TogglePlayPause() {
  if (!player_ || !player_.Source()) {
    StartPlayback();
    return;
  }
  auto const session = player_.PlaybackSession();
  if (session.PlaybackState() ==
      winrt::Windows::Media::Playback::MediaPlaybackState::Paused) {
    player_.Play();
    PlayPauseGlyph().Glyph(winrt::hstring(L"\xE769"));
    if (!position_timer_.IsRunning()) position_timer_.Start();
    SetSmtcStatus(true);
  } else {
    player_.Pause();
    PlayPauseGlyph().Glyph(winrt::hstring(L"\xE768"));
    SavePosition(false);
    SetSmtcStatus(false);
  }
}

void MainWindow::StopPlayback(bool save) {
  if (save) SavePosition(false);
  position_timer_.Stop();
  if (player_) {
    player_.Pause();
    // Drop the media along with the pause: a stale Source would let
    // TogglePlayPause resurrect the previous episode after a switch.
    player_.Source(nullptr);
  }
  PlayPauseGlyph().Glyph(winrt::hstring(L"\xE768"));
  SetSmtcStatus(false);
}

void MainWindow::TickPlayer() {
  if (!player_ || !player_.Source()) return;
  auto const session = player_.PlaybackSession();
  if (!session) return;
  double const position = TimeSpanSeconds(session.Position());

  double const natural = TimeSpanSeconds(player_.NaturalDuration());
  if (natural > 0 && std::isfinite(natural)) duration_ = natural;

  syncing_ui_ = true;
  SeekSlider().Maximum(duration_ > 0 ? duration_ : 100);
  SeekSlider().Value(position);
  PositionText().Text(winrt::hstring(FormatSeconds(position)));
  DurationText().Text(winrt::hstring(FormatSeconds(duration_)));
  syncing_ui_ = false;

  HighlightRunningSegment(position);
  UpdateSmtcTimeline(position);

  if (duration_ > 0 && position >= duration_ - 0.75) {
    if (preview_active_) {
      // a preview has no library row: stop and point at Subscribe
      preview_active_ = false;
      player_.Pause();
      PlayPauseGlyph().Glyph(winrt::hstring(L"\xE768"));
      SetSmtcStatus(false);
      SetStatus(true, std::wstring(podlens::Tr("status.preview_ended")));
      return;
    }
    SavePosition(true);
    player_.Pause();
    PlayPauseGlyph().Glyph(winrt::hstring(L"\xE768"));
    SetSmtcStatus(false);
    SetStatus(true, std::wstring(podlens::Tr("status.playback_done")));
    ReloadEpisodes();
    // continuous playback: roll into the next (older) episode of the feed
    for (size_t i = 0; i + 1 < episodes_.size(); ++i) {
      if (episodes_[i].id == selected_episode_id_) {
        SetStatus(true, std::wstring(podlens::Tr("status.autoplay_next")));
        EpisodeList().SelectedIndex(static_cast<uint32_t>(i + 1));
        StartPlayback();
        break;
      }
    }
    return;
  }
  if (++ticks_since_save_ >= 5) {
    ticks_since_save_ = 0;
    SavePosition(false);
  }
}

void MainWindow::SavePosition(bool done) {
  if (!api_ || selected_episode_id_.empty() || !player_ || !player_.Source()) {
    return;
  }
  double const position =
      player_ && player_.PlaybackSession()
          ? TimeSpanSeconds(player_.PlaybackSession().Position())
          : 0.0;
  (void)api_->position_save_async(
      selected_episode_id_, std::to_string(static_cast<long long>(position)),
      done ? "1" : "0", [](rivet_app::Result<bool>&&) {});
}

namespace {
winrt::Windows::Foundation::TimeSpan SecondsAsTimeSpan(double seconds) {
  return std::chrono::duration_cast<winrt::Windows::Foundation::TimeSpan>(
      std::chrono::duration<double>(seconds));
}
}  // namespace

// Remote transport commands arriving through system media controls (taskbar
// flyout, hardware keys, Bluetooth headsets).
void MainWindow::RemotePlay() {
  if (!player_ || !player_.Source()) return;
  auto const session = player_.PlaybackSession();
  if (session &&
      session.PlaybackState() ==
          winrt::Windows::Media::Playback::MediaPlaybackState::Paused) {
    player_.Play();
    PlayPauseGlyph().Glyph(winrt::hstring(L"\xE769"));
    if (!position_timer_.IsRunning()) position_timer_.Start();
    SetSmtcStatus(true);
  }
}

void MainWindow::RemotePause() {
  if (!player_ || !player_.Source()) return;
  auto const session = player_.PlaybackSession();
  if (session &&
      session.PlaybackState() ==
          winrt::Windows::Media::Playback::MediaPlaybackState::Playing) {
    player_.Pause();
    PlayPauseGlyph().Glyph(winrt::hstring(L"\xE768"));
    SavePosition(false);
    SetSmtcStatus(false);
  }
}

void MainWindow::SeekTo(double seconds) {
  if (!player_ || !player_.Source()) return;
  seconds = std::max(seconds, 0.0);
  if (duration_ > 0) seconds = std::min(seconds, duration_);
  player_.Position(SecondsAsTimeSpan(seconds));
  syncing_ui_ = true;
  if (duration_ > 0) SeekSlider().Value(seconds);
  PositionText().Text(winrt::hstring(FormatSeconds(seconds)));
  syncing_ui_ = false;
  UpdateSmtcTimeline(seconds);
}

void MainWindow::SetSmtcStatus(bool playing) {
  try {
    if (!player_) return;
    auto smtc = player_.SystemMediaTransportControls();
    smtc.PlaybackStatus(playing ? winrt::Windows::Media::MediaPlaybackStatus::Playing
                                : winrt::Windows::Media::MediaPlaybackStatus::Paused);
  } catch (...) {
    // SMTC hiccups must never take playback down with them.
  }
}

void MainWindow::UpdateSmtcMetadata() {
  try {
    if (!player_) return;
    std::wstring title;
    std::wstring show;
    std::wstring artwork;
    for (auto const& episode : episodes_) {
      if (episode.id == selected_episode_id_) {
        title = to_wide(episode.title);
        break;
      }
    }
    auto const feed_id = SelectedFeedId();
    for (auto const& feed : feeds_) {
      if (feed.id == feed_id) {
        show = to_wide(feed.title);
        artwork = to_wide(feed.artwork);
        break;
      }
    }
    auto updater = player_.SystemMediaTransportControls().DisplayUpdater();
    updater.Type(winrt::Windows::Media::MediaPlaybackType::Music);
    auto music = updater.MusicProperties();
    music.Title(title);
    music.Artist(show);
    if (artwork.rfind(L"http", 0) == 0) {
      try {
        updater.Thumbnail(
            winrt::Windows::Storage::Streams::RandomAccessStreamReference::CreateFromUri(
                winrt::Windows::Foundation::Uri(artwork)));
      } catch (...) {
        // a broken artwork URL just means no cover in the flyout
      }
    }
    updater.Update();
  } catch (...) {
  }
}

void MainWindow::UpdateSmtcTimeline(double position_seconds) {
  try {
    if (!player_ || !player_.Source()) return;
    auto smtc = player_.SystemMediaTransportControls();
    winrt::Windows::Media::SystemMediaTransportControlsTimelineProperties timeline;
    timeline.StartTime(std::chrono::seconds{0});
    timeline.MinSeekTime(std::chrono::seconds{0});
    timeline.Position(SecondsAsTimeSpan(position_seconds));
    if (duration_ > 0) {
      timeline.EndTime(SecondsAsTimeSpan(duration_));
      timeline.MaxSeekTime(SecondsAsTimeSpan(duration_));
    }
    smtc.UpdateTimelineProperties(timeline);
  } catch (...) {
  }
}

// ---- episode extras -----------------------------------------------------------

void MainWindow::LoadChapters() {
  chapters_.clear();
  UpdateChapterButtons();
  if (!api_ || selected_episode_id_.empty()) return;
  auto const dispatcher = DispatcherQueue();
  auto const weak = get_weak();
  auto const episode_id = selected_episode_id_;
  (void)api_->episode_chapters_async(
      episode_id,
      [dispatcher, weak, episode_id](
          rivet_app::Result<std::vector<std::vector<std::string>>> result) {
        dispatcher.TryEnqueue([weak, result, episode_id]() mutable {
          if (auto window = weak.get()) {
            try {
              if (window->selected_episode_id_ != episode_id) return;
              window->chapters_.clear();
              for (auto const& row : result.get()) {
                if (row.size() < 2) continue;
                ChapterMark mark;
                mark.start = std::wcstod(to_wide(row[0]).c_str(), nullptr);
                mark.title = to_wide(row[1]);
                window->chapters_.push_back(mark);
              }
              window->UpdateChapterButtons();
            } catch (std::exception const&) {
            }
          }
        });
      });
}

void MainWindow::UpdateChapterButtons() {
  bool const has = !chapters_.empty();
  ChapterBackButton().Visibility(has ? Visibility::Visible : Visibility::Collapsed);
  ChapterForwardButton().Visibility(has ? Visibility::Visible
                                        : Visibility::Collapsed);
}

void MainWindow::SkipChapter(int direction) {
  if (chapters_.empty() || !player_ || !player_.Source()) return;
  double const position = TimeSpanSeconds(player_.PlaybackSession().Position());
  if (direction > 0) {
    for (auto const& mark : chapters_) {
      if (mark.start > position + 0.5) {
        SeekTo(mark.start);
        return;
      }
    }
    return;
  }
  // backward: to the current chapter's start, or the previous one when
  // already near the beginning of this chapter
  size_t idx = 0;
  double current_start = chapters_.front().start;
  for (size_t i = 0; i < chapters_.size(); ++i) {
    if (chapters_[i].start <= position) {
      current_start = chapters_[i].start;
      idx = i;
    } else {
      break;
    }
  }
  SeekTo(position - current_start < 5 && idx > 0 ? chapters_[idx - 1].start
                                                 : current_start);
}

void MainWindow::ResumeLast() {
  if (!api_) return;
  auto const dispatcher = DispatcherQueue();
  auto const weak = get_weak();
  (void)api_->resume_last_async(
      [dispatcher, weak](rivet_app::Result<std::vector<std::string>> result) {
        dispatcher.TryEnqueue([weak, result]() mutable {
          if (auto window = weak.get()) {
            try {
              auto const row = result.get();
              if (row.size() < 3) return;
              window->pending_resume_feed_id_ = row[0];
              window->pending_resume_episode_id_ = row[1];
              window->ReloadFeeds();
            } catch (std::exception const&) {
            }
          }
        });
      });
}

void MainWindow::AutoRefreshTick() {
  if (!api_) return;
  auto const dispatcher = DispatcherQueue();
  auto const weak = get_weak();
  (void)api_->feed_refresh_all_async(
      [dispatcher, weak](rivet_app::Result<std::int64_t> result) {
        dispatcher.TryEnqueue([weak, result]() mutable {
          if (auto window = weak.get()) {
            try {
              auto const added = result.get();
              window->ReloadFeeds();
              if (added > 0) {
                window->SetStatus(true,
                                  std::wstring(podlens::Tr("status.refreshed")) +
                                      to_wide(std::to_string(added)) +
                                      std::wstring(podlens::Tr("status.refreshed_suffix")));
              }
            } catch (std::exception const&) {
            }
          }
        });
      });
}

std::wstring MainWindow::FormatTime(double seconds) {
  return FormatSeconds(seconds);
}

// ---- helpers --------------------------------------------------------------------

void MainWindow::SetStatus(bool ok, std::wstring const& message) {
  // The status strip carries everything quietly; the InfoBar interrupts only
  // for errors and dismisses itself, so healthy states never take up space.
  StatusLine().Text(winrt::hstring(message));
  if (ok) {
    StatusLine().Foreground(Microsoft::UI::Xaml::Application::Current()
                                .Resources()
                                .Lookup(box_value(winrt::hstring(L"AppInkSoftBrush")))
                                .as<Microsoft::UI::Xaml::Media::Brush>());
  } else {
    StatusLine().Foreground(
        Microsoft::UI::Xaml::Media::SolidColorBrush(winrt::Windows::UI::Color{0xFF, 0xB3, 0x26, 0x1E}));
    ErrorBar().Message(winrt::hstring(message));
    ErrorBar().IsOpen(true);
    error_bar_timer_.Stop();
    error_bar_timer_.Start();
  }
}

std::string MainWindow::SelectedFeedId() {
  return selected_feed_id_;
}

std::string MainWindow::SelectedEpisodeId() {
  auto const index = EpisodeList().SelectedIndex();
  if (index < 0 || index >= static_cast<int>(episodes_.size())) return {};
  return episodes_[static_cast<size_t>(index)].id;
}

// The backend caches audio under <data-dir>\audio\<episodeId>*; the host
// resolves the file by id prefix so playback needs no extra RPC. The
// PODLENS_DATA_DIR override must match the backend's (dev/test only).
// getenv_s: MSVC deprecates plain getenv (C4996) and the project treats
// warnings as errors.
std::wstring MainWindow::FindLocalAudio(std::string const& episode_id) {
  std::filesystem::path base;
  size_t required = 0;
  if (::getenv_s(&required, nullptr, 0, "PODLENS_DATA_DIR") == 0 && required > 0) {
    std::string value(required, '\0');
    size_t written = 0;
    ::getenv_s(&written, value.data(), value.size(), "PODLENS_DATA_DIR");
    value.resize(written > 0 ? written - 1 : 0); // drop the terminating null
    base = std::filesystem::path(value);
  }
  if (base.empty()) {
    PWSTR profile = nullptr;
    if (::SHGetKnownFolderPath(FOLDERID_Profile, 0, nullptr, &profile) != S_OK) {
      return {};
    }
    base = std::filesystem::path(profile) / L".podlens";
    ::CoTaskMemFree(profile);
  }
  std::filesystem::path dir = base / L"audio";
  std::error_code ec;
  if (!std::filesystem::exists(dir, ec)) return {};
  for (auto const& entry : std::filesystem::directory_iterator(dir, ec)) {
    auto const name = entry.path().filename().string();
    if (name.rfind(episode_id, 0) == 0) {
      return entry.path().wstring();
    }
  }
  return {};
}

void MainWindow::StartJob(std::string const& kind, std::string const& episode_id) {
  if (!api_ || episode_id.empty()) return;
  SetStatus(true, std::wstring(podlens::Tr("status.job_running")));
  auto const dispatcher = DispatcherQueue();
  auto const weak = get_weak();

  std::thread([weak, dispatcher, kind, episode_id]() mutable {
    std::string job_id;
    try {
      if (!weak.get()) return;
      std::future<std::string> f;
      if (kind == "download") f = weak.get()->api_->episode_download(episode_id);
      else if (kind == "transcribe") f = weak.get()->api_->episode_transcribe(episode_id);
      else if (kind == "translate") f = weak.get()->api_->episode_translate(episode_id);
      else if (kind == "pipeline") f = weak.get()->api_->episode_pipeline(episode_id);
      else f = weak.get()->api_->episode_summarize(episode_id);
      job_id = f.get();
    } catch (std::exception const& e) {
      auto message = std::string(e.what());
      dispatcher.TryEnqueue([weak, message] {
        if (auto window = weak.get()) window->SetStatus(false, to_wide(message));
      });
      return;
    }
    // poll until the job leaves "running"
    for (;;) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1200));
      if (!weak.get()) return;
      try {
        auto const status_json = weak.get()->api_->job_status(job_id).get();
        if (status_json.find("\"running\"") == std::string::npos) break;
      } catch (std::exception const&) {
        break;
      }
    }
    dispatcher.TryEnqueue([weak, kind, episode_id] {
      if (auto window = weak.get()) {
        window->ReloadRecent();
        window->ReloadEpisodes();
        if (window->selected_episode_id_ == episode_id) {
          if (kind == "transcribe" || kind == "translate" ||
              kind == "pipeline") {
            window->LoadTranscript();
          }
          if (kind == "summarize" || kind == "pipeline") {
            window->LoadSummary(episode_id);
          }
        }
      }
    });
  }).detach();
}

// ---- XAML event handlers ---------------------------------------------------------

void MainWindow::Nav_ItemInvoked(winrt::Windows::Foundation::IInspectable const&,
                                 muxc::NavigationViewItemInvokedEventArgs const&) {
  // Selection semantics are handled in Nav_SelectionChanged.
}

void MainWindow::Nav_SelectionChanged(
    winrt::Windows::Foundation::IInspectable const&,
    muxc::NavigationViewSelectionChangedEventArgs const&) {
  if (!initialized_ || suppress_nav_) return;
  auto const item = Nav().SelectedItem().try_as<muxc::NavigationViewItem>();
  if (!item) return;
  auto const tag = winrt::unbox_value_or<winrt::hstring>(item.Tag(), {});
  std::wstring const key(tag);
  if (key == L"settings") {
    RunSettingsDialog();
    return;
  }
  if (key == L"home") { ShowPage(Page::Home); return; }
  if (key == L"discover") { ShowPage(Page::Discover); return; }
  if (key == L"library-recent") { ShowPage(Page::Recent); return; }
  if (key == L"library-shows") { ShowPage(Page::Shows); return; }
  if (key == L"library-downloaded") { ShowPage(Page::Downloaded); return; }
  if (!key.empty()) {
    // a subscription: the feed's episode page
    selected_feed_id_ = to_utf8(key);
    ReloadEpisodes();
    ShowPage(Page::Episodes);
  }
}

void MainWindow::EpisodeList_SelectionChanged(
    winrt::Windows::Foundation::IInspectable const&,
    Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const&) {
  if (!initialized_) return;
  if (!selected_episode_id_.empty()) StopPlayback(true);
  selected_episode_id_ = SelectedEpisodeId();
  transcript_.clear();
  RenderTranscript();
  RenderSummary();
  RenderDetailHeader();
  UpdateDetailVisibility();
  UpdateActionButtons();
  if (!selected_episode_id_.empty()) {
    LoadTranscript();
    LoadSummary(selected_episode_id_);
    LoadChapters();
  }
  UpdatePlayerInfo();
}

void MainWindow::Bilingual_Click(winrt::Windows::Foundation::IInspectable const&,
                                 Microsoft::UI::Xaml::RoutedEventArgs const&) {
  bilingual_ = BilingualToggle().IsChecked().Value();
  ApplyMode();
}

void MainWindow::AddFeed_Click(winrt::Windows::Foundation::IInspectable const&,
                               Microsoft::UI::Xaml::RoutedEventArgs const&) {
  if (!api_) return;
  auto const dispatcher = DispatcherQueue();
  auto const weak = get_weak();

  muxc::TextBox input;
  input.PlaceholderText(winrt::hstring(L"https://example.com/feed.xml"));
  input.Width(420);

  muxc::ContentDialog dialog;
  dialog.Title(box_value(winrt::hstring(std::wstring(podlens::Tr("menu.add_feed")))));
  dialog.Content(input);
  dialog.PrimaryButtonText(winrt::hstring(std::wstring(podlens::Tr("dialog.primary_add"))));
  dialog.CloseButtonText(winrt::hstring(std::wstring(podlens::Tr("dialog.cancel"))));
  dialog.XamlRoot(Content().XamlRoot());

  auto op = dialog.ShowAsync();
  op.Completed([weak, input, dispatcher](
                   winrt::Windows::Foundation::IAsyncOperation<
                       muxc::ContentDialogResult> const& sender,
                   winrt::Windows::Foundation::AsyncStatus) {
    if (sender.GetResults() != muxc::ContentDialogResult::Primary) {
      return;
    }
    auto const url = to_utf8(std::wstring(input.Text()));
    if (url.empty()) return;
    std::thread([weak, dispatcher, url]() mutable {
      std::string error;
      if (weak.get()) {
        try {
          weak.get()->api_->feed_add(url).get();
        } catch (std::exception const& e) {
          error = e.what();
        }
      }
      dispatcher.TryEnqueue([weak, error] {
        if (auto window = weak.get()) {
          if (error.empty()) {
            window->SetStatus(true, std::wstring(podlens::Tr("status.feed_added")));
            window->ReloadFeeds();
            window->ReloadRecent();
          } else {
            window->SetStatus(false, to_wide(error));
          }
        }
      });
    }).detach();
  });
}

void MainWindow::AddFromCatalog(std::string const& url) {
  if (!api_) return;
  auto const dispatcher = DispatcherQueue();
  auto const weak = get_weak();
  std::thread([weak, dispatcher, url]() mutable {
    std::string add_error;
    if (weak.get()) {
      try {
        weak.get()->api_->feed_add(url).get();
      } catch (std::exception const& e) {
        add_error = e.what();
      }
    }
    dispatcher.TryEnqueue([weak, add_error] {
      if (auto window = weak.get()) {
        if (add_error.empty()) {
          window->SetStatus(true, std::wstring(podlens::Tr("status.feed_added")));
          window->ReloadFeeds();
          window->LoadDiscover();  // refresh the panel in place
        } else {
          window->SetStatus(false, to_wide(add_error));
        }
      }
    });
  }).detach();
}

void MainWindow::LoadDiscover() {
  if (!api_) return;
  auto const dispatcher = DispatcherQueue();
  auto const weak = get_weak();

  std::thread([weak, dispatcher]() mutable {
    std::string error;
    std::vector<std::vector<std::string>> rows;
    if (weak.get()) {
      try {
        rows = weak.get()->api_->catalog_list().get();
      } catch (std::exception const& e) {
        error = e.what();
      }
    }
    dispatcher.TryEnqueue([weak, rows = std::move(rows), error]() mutable {
      if (auto window = weak.get()) {
        if (!error.empty()) {
          window->SetStatus(false, to_wide(error));
          return;
        }
        window->RenderCatalog(rows, /*grouped=*/true);
        window->SetStatus(true, std::wstring(podlens::Tr("discover.title")));
        window->ShowPage(Page::Discover);
      }
    });
  }).detach();
}

// One card renderer for both discover surfaces: the curated catalog
// (grouped by category, with the standing note) and full-directory search
// results (flat list plus a back button). Cards carry an audition button
// next to Add — try the newest episode before subscribing.
void MainWindow::RenderCatalog(std::vector<std::vector<std::string>> const& rows,
                               bool grouped) {
  DiscoverList().Children().Clear();

  if (!grouped) {
    muxc::Button back;
    back.Content(box_value(winrt::hstring(std::wstring(podlens::Tr("discover.back")))));
    back.Margin(winrt::Microsoft::UI::Xaml::Thickness{0, 0, 0, 8});
    back.Click([weak = get_weak()](auto&&, auto&&) {
      if (auto window = weak.get()) window->LoadDiscover();
    });
    DiscoverList().Children().Append(back);
  }

  std::wstring current_category;
  for (auto const& row : rows) {
    if (row.size() < 7) continue;
    std::wstring const category = to_wide(row[1]);
    if (grouped && category != current_category) {
      current_category = category;
      auto label = SectionLabel(std::wstring(podlens::Tr(
          ("cat." + row[1]).c_str())));
      if (label.Text().size() == 0) label.Text(winrt::hstring(category));
      label.Margin(winrt::Microsoft::UI::Xaml::Thickness{0, 12, 0, 2});
      DiscoverList().Children().Append(label);
    }

    muxc::TextBlock title;
    title.Text(winrt::hstring(to_wide(row[2])));
    title.FontSize(15);
    title.FontWeight(Microsoft::UI::Text::FontWeights::SemiBold());
    muxc::TextBlock description;
    description.Text(winrt::hstring(to_wide(row[3])));
    description.FontSize(13);
    description.TextWrapping(Microsoft::UI::Xaml::TextWrapping::Wrap);
    description.Opacity(0.8);
    muxc::TextBlock homepage;
    homepage.Text(winrt::hstring(to_wide(row[4])));
    homepage.FontSize(12);
    homepage.Opacity(0.6);
    muxc::Border artwork_host = MakeArtwork(to_wide(row.size() > 7 ? row[7] : ""), 56);
    artwork_host.VerticalAlignment(Microsoft::UI::Xaml::VerticalAlignment::Top);

    // column 4 is the feed URL (column 5 is the show's homepage — the two
    // were swapped here once, subscribing people to web pages)
    auto const feed_url = to_utf8(to_wide(row[4]));
    bool const added = row[6] == "1";

    muxc::StackPanel actions;
    actions.Orientation(Microsoft::UI::Xaml::Controls::Orientation::Horizontal);
    actions.Spacing(8);
    if (!added) {
      muxc::Button preview;
      preview.Content(box_value(winrt::hstring(std::wstring(podlens::Tr("discover.preview")))));
      preview.Click([weak = get_weak(), feed_url](auto&&, auto&&) {
        if (auto window = weak.get()) window->PreviewFromCatalog(feed_url);
      });
      actions.Children().Append(preview);
    }
    muxc::Button action;
    action.Content(box_value(winrt::hstring(std::wstring(podlens::Tr(
        added ? "discover.added" : "discover.add")))));
    action.IsEnabled(!added);
    action.Click([weak = get_weak(), feed_url](auto&&, auto&&) {
      if (auto window = weak.get()) window->AddFromCatalog(feed_url);
    });
    actions.Children().Append(action);

    muxc::Grid lines;
    lines.ColumnSpacing(12);
    lines.ColumnDefinitions().Append(muxc::ColumnDefinition());
    lines.ColumnDefinitions().Append(muxc::ColumnDefinition());
    lines.ColumnDefinitions().GetAt(1).Width(
        {0, Microsoft::UI::Xaml::GridUnitType::Auto});
    muxc::StackPanel text;
    text.Spacing(2);
    text.Children().Append(title);
    text.Children().Append(description);
    text.Children().Append(homepage);
    muxc::Grid::SetColumn(text, 0);
    muxc::Grid::SetColumn(actions, 1);
    lines.Children().Append(text);
    lines.Children().Append(actions);

    muxc::Border card;
    card.Child(lines);
    card.CornerRadius(winrt::Microsoft::UI::Xaml::CornerRadius{8});
    card.Padding(winrt::Microsoft::UI::Xaml::Thickness{14, 10, 14, 10});
    card.Margin(winrt::Microsoft::UI::Xaml::Thickness{0, 0, 0, 8});
    card.Background(Microsoft::UI::Xaml::Application::Current()
                        .Resources()
                        .Lookup(box_value(winrt::hstring(L"AppCardBrush")))
                        .as<Microsoft::UI::Xaml::Media::Brush>());
    DiscoverList().Children().Append(card);
  }

  if (grouped) {
    muxc::TextBlock note;
    note.Text(winrt::hstring(std::wstring(podlens::Tr("discover.note"))));
    note.FontSize(12);
    note.Opacity(0.6);
    note.TextWrapping(Microsoft::UI::Xaml::TextWrapping::Wrap);
    note.Margin(winrt::Microsoft::UI::Xaml::Thickness{0, 8, 0, 0});
    DiscoverList().Children().Append(note);
  }
}

void MainWindow::DiscoverSearch_Click(winrt::Windows::Foundation::IInspectable const&,
                                      Microsoft::UI::Xaml::RoutedEventArgs const&) {
  auto const query = std::wstring(DiscoverSearchBox().Text());
  if (!query.empty()) RunDiscoverSearch(query);
}

void MainWindow::DiscoverSearch_KeyDown(winrt::Windows::Foundation::IInspectable const&,
                                        winrt::Microsoft::UI::Xaml::Input::KeyRoutedEventArgs const& args) {
  if (args.Key() == winrt::Windows::System::VirtualKey::Enter) {
    auto const query = std::wstring(DiscoverSearchBox().Text());
    if (!query.empty()) RunDiscoverSearch(query);
  }
}

void MainWindow::RunDiscoverSearch(std::wstring const& query) {
  if (!api_) return;
  SetStatus(true, std::wstring(podlens::Tr("discover.searching")));
  auto const dispatcher = DispatcherQueue();
  auto const weak = get_weak();
  std::thread([weak, dispatcher, query = to_utf8(query)]() mutable {
    std::string error;
    std::vector<std::vector<std::string>> rows;
    if (weak.get()) {
      try {
        rows = weak.get()->api_->catalog_search(query).get();
      } catch (std::exception const& e) {
        error = e.what();
      }
    }
    dispatcher.TryEnqueue([weak, rows = std::move(rows), error]() mutable {
      if (auto window = weak.get()) {
        if (!error.empty()) {
          window->SetStatus(false,
                            std::wstring(podlens::Tr("discover.search_failed")) + to_wide(error));
          return;
        }
        window->RenderCatalog(rows, /*grouped=*/false);
      }
    });
  }).detach();
}

// Try before you subscribe: peek the feed (no library write), download the
// newest episode's audio into the preview cache, then play the local file
// outside the library context.
void MainWindow::PreviewFromCatalog(std::string const& url) {
  if (!api_ || url.empty()) return;
  SetStatus(true, std::wstring(podlens::Tr("status.preview_fetch")));
  auto const dispatcher = DispatcherQueue();
  auto const weak = get_weak();
  std::thread([weak, dispatcher, url]() mutable {
    std::string error;
    std::vector<std::vector<std::string>> rows;
    std::string audio_path;
    if (weak.get()) {
      try {
        rows = weak.get()->api_->feed_peek(url).get();
        if (rows.empty() || rows[0].size() < 7 ||
            rows[0][3].find_first_not_of(" \t\r\n") == std::string::npos) {
          error =
              to_utf8(std::wstring(podlens::Tr("status.preview_no_enclosure")));
        } else {
          dispatcher.TryEnqueue([weak] {
            if (auto window = weak.get()) {
              window->SetStatus(true, std::wstring(podlens::Tr("status.preview_download")));
            }
          });
          audio_path = weak.get()->api_->preview_audio(rows[0][3]).get();
        }
      } catch (std::exception const& e) {
        error = e.what();
      }
    }
    dispatcher.TryEnqueue([weak, rows = std::move(rows),
                           audio_path = std::move(audio_path), error]() mutable {
      if (auto window = weak.get()) {
        if (!error.empty()) {
          window->SetStatus(false,
                            std::wstring(podlens::Tr("status.preview_failed")) + to_wide(error));
          return;
        }
        auto const& row = rows[0];
        window->StartPreviewPlayback(to_wide(audio_path), to_wide(row[4]),
                                     to_wide(row[0]), to_wide(row[2]));
      }
    });
  }).detach();
}

void MainWindow::StartPreviewPlayback(std::wstring const& path, std::wstring const& title,
                                      std::wstring const& show, std::wstring const& artwork) {
  try {
    // No library row: no position restore, no saves, no autoplay chain —
    // the player bar carries the peeked show identity with a 试听 marker.
    preview_active_ = true;
    selected_episode_id_.clear();
    transcript_.clear();
    RenderTranscript();
    UpdateDetailVisibility();

    auto file =
        winrt::Windows::Storage::StorageFile::GetFileFromPathAsync(path).get();
    player_.Source(
        winrt::Windows::Media::Core::MediaSource::CreateFromStorageFile(file));
    duration_ = 0;
    ticks_since_save_ = 0;
    double rate = 1.0;
    if (auto const entry = RateSelector().SelectedItem().try_as<winrt::hstring>()) {
      rate = std::wcstod(entry->c_str(), nullptr);
      if (rate <= 0) rate = 1.0;
    }
    player_.PlaybackRate(static_cast<double>(rate));
    player_.Play();
    PlayPauseGlyph().Glyph(winrt::hstring(L"\xE769"));
    position_timer_.Start();

    PlayerTitle().Text(title);
    PlayerShow().Text(show + L" · " + std::wstring(podlens::Tr("discover.preview")));
    try {
      PlayerArtwork().Source(
          winrt::Microsoft::UI::Xaml::Media::Imaging::BitmapImage(
              winrt::Windows::Foundation::Uri(artwork)));
    } catch (...) {
    }
    SetSmtcStatus(true);
    UpdateSmtcMetadata();
    SetStatus(true, std::wstring(podlens::Tr("status.preview_playing")) + title);
  } catch (winrt::hresult_error const& e) {
    SetStatus(false, e.message().c_str());
  } catch (std::exception const& e) {
    SetStatus(false, to_wide(e.what()));
  }
}

void MainWindow::RefreshFeed_Click(winrt::Windows::Foundation::IInspectable const&,
                                   Microsoft::UI::Xaml::RoutedEventArgs const&) {
  auto const feed_id = SelectedFeedId();
  if (feed_id.empty() || !api_) return;
  SetStatus(true, std::wstring(podlens::Tr("status.job_running")));
  auto const dispatcher = DispatcherQueue();
  auto const weak = get_weak();
  std::thread([weak, dispatcher, feed_id]() mutable {
    std::string error;
    if (weak.get()) {
      try {
        weak.get()->api_->feed_refresh(feed_id).get();
      } catch (std::exception const& e) {
        error = e.what();
      }
    }
    dispatcher.TryEnqueue([weak, error] {
      if (auto window = weak.get()) {
        if (error.empty()) {
          window->SetStatus(true, std::wstring(podlens::Tr("status.ready")));
        } else {
          window->SetStatus(false, to_wide(error));
        }
        window->ReloadFeeds();
        window->ReloadRecent();
        if (window->page_ == Page::Episodes) window->ReloadEpisodes();
      }
    });
  }).detach();
}

void MainWindow::RemoveFeed_Click(winrt::Windows::Foundation::IInspectable const&,
                                  Microsoft::UI::Xaml::RoutedEventArgs const&) {
  auto const feed_id = SelectedFeedId();
  if (feed_id.empty() || !api_) return;
  auto const dispatcher = DispatcherQueue();
  auto const weak = get_weak();

  muxc::ContentDialog dialog;
  dialog.Title(box_value(winrt::hstring(std::wstring(podlens::Tr("dialog.remove_feed_title")))));
  dialog.Content(box_value(winrt::hstring(std::wstring(podlens::Tr("dialog.remove_feed_text")))));
  dialog.PrimaryButtonText(winrt::hstring(std::wstring(podlens::Tr("dialog.remove_feed_primary"))));
  dialog.CloseButtonText(winrt::hstring(std::wstring(podlens::Tr("dialog.cancel"))));
  dialog.XamlRoot(Content().XamlRoot());

  auto op = dialog.ShowAsync();
  op.Completed([weak, dispatcher, feed_id](
                   winrt::Windows::Foundation::IAsyncOperation<
                       muxc::ContentDialogResult> const& sender,
                   winrt::Windows::Foundation::AsyncStatus) {
    if (sender.GetResults() != muxc::ContentDialogResult::Primary) return;
    std::thread([weak, dispatcher, feed_id]() mutable {
      std::string error;
      if (weak.get()) {
        try {
          weak.get()->api_->feed_remove(feed_id).get();
        } catch (std::exception const& e) {
          error = e.what();
        }
      }
      dispatcher.TryEnqueue([weak, error, feed_id] {
        if (auto window = weak.get()) {
          if (error.empty()) {
            window->SetStatus(true, std::wstring(podlens::Tr("status.feed_removed")));
          } else {
            window->SetStatus(false, to_wide(error));
          }
          if (window->selected_feed_id_ == feed_id) {
            window->selected_feed_id_.clear();
            window->selected_episode_id_.clear();
            window->ShowPage(Page::Home);
          }
          window->ReloadFeeds();
          window->ReloadRecent();
        }
      });
    }).detach();
  });
}

void MainWindow::RefreshAll_Click(winrt::Windows::Foundation::IInspectable const&,
                                  Microsoft::UI::Xaml::RoutedEventArgs const&) {
  if (!api_) return;
  auto const dispatcher = DispatcherQueue();
  auto const weak = get_weak();
  std::thread([weak, dispatcher]() mutable {
    std::string error;
    if (weak.get()) {
      try {
        weak.get()->api_->feed_refresh_all().get();
      } catch (std::exception const& e) {
        error = e.what();
      }
    }
    dispatcher.TryEnqueue([weak, error] {
      if (auto window = weak.get()) {
        if (error.empty()) {
          window->SetStatus(true, std::wstring(podlens::Tr("status.refreshed")));
          window->ReloadFeeds();
          window->ReloadEpisodes();
        } else {
          window->SetStatus(false, to_wide(error));
        }
      }
    });
  }).detach();
}

void MainWindow::Download_Click(winrt::Windows::Foundation::IInspectable const&,
                                Microsoft::UI::Xaml::RoutedEventArgs const&) {
  StartJob("download", SelectedEpisodeId());
}

void MainWindow::Transcribe_Click(winrt::Windows::Foundation::IInspectable const&,
                                  Microsoft::UI::Xaml::RoutedEventArgs const&) {
  StartJob("transcribe", SelectedEpisodeId());
}

void MainWindow::Translate_Click(winrt::Windows::Foundation::IInspectable const&,
                                 Microsoft::UI::Xaml::RoutedEventArgs const&) {
  StartJob("translate", SelectedEpisodeId());
}

void MainWindow::Summarize_Click(winrt::Windows::Foundation::IInspectable const&,
                                 Microsoft::UI::Xaml::RoutedEventArgs const&) {
  StartJob("summarize", SelectedEpisodeId());
}

void MainWindow::PlayPause_Click(winrt::Windows::Foundation::IInspectable const&,
                                 Microsoft::UI::Xaml::RoutedEventArgs const&) {
  TogglePlayPause();
}

void MainWindow::Pipeline_Click(winrt::Windows::Foundation::IInspectable const&,
                                Microsoft::UI::Xaml::RoutedEventArgs const&) {
  // Runs the real episode-pipeline RPC; the cost estimate is already shown
  // inline in the detail pane (LoadEstimate), so there is no extra confirm
  // dialog like on Linux. Progress rides the shared poll-until-done loop.
  StartJob("pipeline", SelectedEpisodeId());
}

void MainWindow::RemoveAudio_Click(winrt::Windows::Foundation::IInspectable const&,
                                   Microsoft::UI::Xaml::RoutedEventArgs const&) {
  auto const episode_id = SelectedEpisodeId();
  if (episode_id.empty() || !api_) return;
  auto const dispatcher = DispatcherQueue();
  auto const weak = get_weak();
  (void)api_->episode_remove_audio_async(
      episode_id,
      [dispatcher, weak](rivet_app::Result<bool> result) {
        // completions run off the UI thread; dispatch before touching XAML
        dispatcher.TryEnqueue([weak, result]() mutable {
          if (auto window = weak.get()) {
            try {
              (void)result.get();
              window->SetStatus(
                  true, std::wstring(podlens::Tr("status.audio_removed")));
            } catch (std::exception const& e) {
              window->SetStatus(false, to_wide(e.what()));
            }
            window->ReloadEpisodes();
          }
        });
      });
}

void MainWindow::MarkMenu_Click(winrt::Windows::Foundation::IInspectable const&,
                                  Microsoft::UI::Xaml::RoutedEventArgs const&) {
  EpisodeRow const* episode = nullptr;
  for (auto const& candidate : episodes_) {
    if (candidate.id == selected_episode_id_) {
      episode = &candidate;
      break;
    }
  }
  if (!episode || !api_) return;
  auto const id = episode->id;
  auto const done = episode->done ? "0" : "1";
  (void)api_->episode_set_done_async(
      id, done, [weak = get_weak()](rivet_app::Result<bool>) {
        if (auto window = weak.get()) {
          window->ReloadEpisodes();
          window->ReloadFeeds();
        }
      });
}

void MainWindow::MarkFromMenu(
    winrt::Windows::Foundation::IInspectable const& sender, wchar_t const* done) {
  auto item = sender.try_as<muxc::MenuFlyoutItem>();
  if (!item || !api_) return;
  auto const id = to_utf8(std::wstring(
      winrt::unbox_value_or<winrt::hstring>(item.Tag(), winrt::hstring(L""))));
  if (id.empty()) return;
  (void)api_->episode_set_done_async(
      id, to_utf8(done), [weak = get_weak()](rivet_app::Result<bool>) {
        if (auto window = weak.get()) {
          window->ReloadEpisodes();
          window->ReloadFeeds();
        }
      });
}

void MainWindow::ChapterBack_Click(winrt::Windows::Foundation::IInspectable const&,
                                   Microsoft::UI::Xaml::RoutedEventArgs const&) {
  SkipChapter(-1);
}

void MainWindow::ChapterForward_Click(winrt::Windows::Foundation::IInspectable const&,
                                      Microsoft::UI::Xaml::RoutedEventArgs const&) {
  SkipChapter(1);
}

void MainWindow::SkipBack_Click(winrt::Windows::Foundation::IInspectable const&,
                                Microsoft::UI::Xaml::RoutedEventArgs const&) {
  if (player_ && player_.PlaybackSession()) {
    SeekTo(TimeSpanSeconds(player_.PlaybackSession().Position()) - 15.0);
  }
}

void MainWindow::SkipForward_Click(winrt::Windows::Foundation::IInspectable const&,
                                   Microsoft::UI::Xaml::RoutedEventArgs const&) {
  if (player_ && player_.PlaybackSession()) {
    SeekTo(TimeSpanSeconds(player_.PlaybackSession().Position()) + 30.0);
  }
}

void MainWindow::SleepOption_Click(winrt::Windows::Foundation::IInspectable const& sender,
                                   Microsoft::UI::Xaml::RoutedEventArgs const&) {
  auto item = sender.try_as<muxc::MenuFlyoutItem>();
  if (!item) return;
  int const minutes = static_cast<int>(std::wcstol(
      winrt::unbox_value_or<winrt::hstring>(item.Tag(), winrt::hstring(L"0")).c_str(),
      nullptr, 10));
  sleep_timer_.Stop();
  if (minutes <= 0) {
    sleep_minutes_ = 0;
    SetStatus(true, std::wstring(podlens::Tr("player.sleep_off")));
    return;
  }
  sleep_minutes_ = minutes;
  sleep_timer_.Interval(std::chrono::duration_cast<winrt::Windows::Foundation::TimeSpan>(
      std::chrono::minutes{minutes}));
  sleep_timer_.Start();
  wchar_t message[128];
  swprintf(message, 128, podlens::Tr("player.sleep_set").data(), minutes);
  SetStatus(true, message);
}

void MainWindow::PlayerAccel_Invoked(
    Microsoft::UI::Xaml::Input::KeyboardAccelerator const& sender,
    Microsoft::UI::Xaml::Input::KeyboardAcceleratorInvokedEventArgs const& args) {
  // Never hijack keystrokes while the user is typing (feed URL, settings)
  // or picking from a menu.
  try {
    auto focused = Microsoft::UI::Xaml::Input::FocusManager::GetFocusedElement();
    if (focused.try_as<muxc::TextBox>() || focused.try_as<muxc::MenuFlyoutItem>()) {
      return;
    }
  } catch (...) {
  }
  // The modifiers enum's owning namespace shifts between Windows SDK
  // versions; deriving it from the getter keeps the comparison portable.
  auto const key = sender.Key();
  auto const modifiers = sender.Modifiers();
  using Mods = std::remove_const_t<std::remove_reference_t<decltype(modifiers)>>;
  if (key == winrt::Windows::System::VirtualKey::Space &&
      modifiers == Mods::None) {
    TogglePlayPause();
    args.Handled(true);
  } else if (key == winrt::Windows::System::VirtualKey::Left &&
             modifiers == Mods::Control) {
    if (player_ && player_.PlaybackSession()) {
      SeekTo(TimeSpanSeconds(player_.PlaybackSession().Position()) - 15.0);
    }
    args.Handled(true);
  } else if (key == winrt::Windows::System::VirtualKey::Right &&
             modifiers == Mods::Control) {
    if (player_ && player_.PlaybackSession()) {
      SeekTo(TimeSpanSeconds(player_.PlaybackSession().Position()) + 30.0);
    }
    args.Handled(true);
  }
}

void MainWindow::FocusMode_Click(
    winrt::Windows::Foundation::IInspectable const&,
    Microsoft::UI::Xaml::RoutedEventArgs const&) {
  focus_listening_ = FocusModeButton().IsChecked().Value();
  PageColumn().Visibility(focus_listening_ ? Visibility::Collapsed : Visibility::Visible);
  ContentDivider().Visibility(focus_listening_ ? Visibility::Collapsed : Visibility::Visible);
  muxc::Grid::SetColumn(DetailColumn(), focus_listening_ ? 0 : 2);
  muxc::Grid::SetColumnSpan(DetailColumn(), focus_listening_ ? 3 : 1);
  Nav().PaneDisplayMode(focus_listening_
      ? muxc::NavigationViewPaneDisplayMode::LeftMinimal
      : muxc::NavigationViewPaneDisplayMode::Left);
  FocusModeButton().Content(box_value(winrt::hstring(std::wstring(
      podlens::Tr(focus_listening_ ? "focus.exit" : "focus.enter")))));
  muxc::ToolTipService::SetToolTip(
      FocusModeButton(), box_value(winrt::hstring(std::wstring(
          podlens::Tr(focus_listening_ ? "focus.exit_hint" : "focus.enter_hint")))));
}

void MainWindow::FollowTranscript_Click(
    winrt::Windows::Foundation::IInspectable const&,
    Microsoft::UI::Xaml::RoutedEventArgs const&) {
  follow_transcript_ = FollowTranscriptButton().IsChecked().Value();
  if (follow_transcript_) ScrollToCurrentTranscript();
}

void MainWindow::ReturnToCurrent_Click(
    winrt::Windows::Foundation::IInspectable const&,
    Microsoft::UI::Xaml::RoutedEventArgs const&) {
  follow_transcript_ = true;
  FollowTranscriptButton().IsChecked(true);
  ScrollToCurrentTranscript();
}

void MainWindow::SeekSlider_ValueChanged(
    winrt::Windows::Foundation::IInspectable const&,
    Microsoft::UI::Xaml::Controls::Primitives::RangeBaseValueChangedEventArgs const&) {
  if (!initialized_ || user_seeking_ || syncing_ui_) return;
  if (player_ && player_.Source()) {
    player_.Position(
        std::chrono::duration_cast<winrt::Windows::Foundation::TimeSpan>(
            std::chrono::duration<double>(SeekSlider().Value())));
  }
}

void MainWindow::SeekSlider_PointerPressed(
    winrt::Windows::Foundation::IInspectable const&,
    Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const&) {
  user_seeking_ = true;
}

void MainWindow::SeekSlider_PointerReleased(
    winrt::Windows::Foundation::IInspectable const&,
    Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const&) {
  if (user_seeking_ && player_ && player_.Source()) {
    player_.Position(
        std::chrono::duration_cast<winrt::Windows::Foundation::TimeSpan>(
            std::chrono::duration<double>(SeekSlider().Value())));
  }
  user_seeking_ = false;
}

void MainWindow::Rate_SelectionChanged(
    winrt::Windows::Foundation::IInspectable const&,
    Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const&) {
  if (!initialized_ || !player_) return;
  if (auto const entry = RateSelector().SelectedItem().try_as<winrt::hstring>()) {
    double const rate = std::wcstod(entry->c_str(), nullptr);
    if (rate > 0) player_.PlaybackRate(rate);
  }
}

void MainWindow::Settings_Click(winrt::Windows::Foundation::IInspectable const&,
                                Microsoft::UI::Xaml::RoutedEventArgs const&) {
  RunSettingsDialog();
}

void MainWindow::CheckUpdates_Click(winrt::Windows::Foundation::IInspectable const&,
                                    Microsoft::UI::Xaml::RoutedEventArgs const&) {
  RunUpdateCheck(/*silent=*/false);
}

// The Help menu's About entry: podlens keeps its About facts (version,
// license, support links) inside the settings dialog, so this routes there.
void MainWindow::MenuAbout_Click(winrt::Windows::Foundation::IInspectable const&,
                                 Microsoft::UI::Xaml::RoutedEventArgs const&) {
  RunSettingsDialog();
}

void MainWindow::RunSettingsDialog() {
  if (!api_) return;
  auto const dispatcher = DispatcherQueue();
  auto const weak = get_weak();
  std::thread([weak, dispatcher]() mutable {
    std::string error;
    std::string version;
    std::vector<std::vector<std::string>> rows;
    if (weak.get()) {
      try {
        rows = weak.get()->api_->settings_list().get();
      } catch (std::exception const& e) {
        error = e.what();
      }
      try {
        version = weak.get()->api_->get_version().get();
      } catch (...) {
        // The About block degrades to no version line; settings still show.
      }
    }
    dispatcher.TryEnqueue(
        [weak, rows = std::move(rows), version = std::move(version),
         error]() mutable {
          if (auto window = weak.get()) {
            window->ShowSettingsDialog(rows, version, error);
          }
        });
  }).detach();
}

void MainWindow::ShowSettingsDialog(std::vector<std::vector<std::string>> rows,
                                    std::string const& version,
                                    std::string const& error) {
  if (!error.empty()) {
    SetStatus(false, to_wide(error));
    return;
  }

  // one labeled TextBox per setting; Save pushes every row through settings-set
  std::vector<std::pair<std::string, muxc::TextBox>> fields;
  muxc::StackPanel panel;
  panel.Spacing(12);
  for (auto const& row : rows) {
    if (row.size() < 2) continue;

    muxc::TextBlock key_label;
    key_label.Text(winrt::hstring(to_wide(row[0])));
    key_label.FontSize(13);
    key_label.FontWeight(Microsoft::UI::Text::FontWeights::SemiBold());

    muxc::TextBox input;
    input.Text(winrt::hstring(to_wide(row[1])));
    input.Width(380);
    winrt::Microsoft::UI::Xaml::Automation::AutomationProperties::SetName(
        input, winrt::hstring(to_wide(row[0])));

    muxc::StackPanel line;
    line.Spacing(4);
    line.Children().Append(key_label);
    line.Children().Append(input);
    if (row.size() >= 3 && !row[2].empty()) {
      muxc::TextBlock description;
      description.Text(winrt::hstring(to_wide(row[2])));
      description.FontSize(11);
      description.Opacity(0.65);
      description.TextWrapping(Microsoft::UI::Xaml::TextWrapping::Wrap);
      line.Children().Append(description);
    }
    panel.Children().Append(line);
    fields.emplace_back(row[0], input);
  }

  muxc::TextBlock hint;
  hint.Text(winrt::hstring(std::wstring(podlens::Tr("settings.reload_hint"))));
  hint.FontSize(12);
  hint.Opacity(0.6);
  panel.Children().Append(hint);

  // About block (the mac settings sheet's facts): version, license and the
  // support links, so a bug report starts one click away.
  muxc::StackPanel about;
  about.Spacing(2);
  if (!version.empty()) {
    muxc::TextBlock about_version;
    about_version.Text(L"PodLens v" + to_wide(version));
    about_version.FontSize(13);
    about_version.FontWeight(Microsoft::UI::Text::FontWeights::SemiBold());
    about.Children().Append(about_version);
  }
  muxc::TextBlock license;
  license.Text(winrt::hstring(std::wstring(podlens::Tr("about.license"))));
  license.FontSize(11);
  license.Opacity(0.7);
  about.Children().Append(license);
  auto website = muxc::HyperlinkButton{};
  website.Content(winrt::box_value(
      winrt::hstring(std::wstring(podlens::Tr("about.website")))));
  website.NavigateUri(winrt::Windows::Foundation::Uri{L"https://podlens.jrtx.site"});
  website.Padding({0, 0, 0, 0});
  about.Children().Append(website);
  auto issue = muxc::HyperlinkButton{};
  issue.Content(winrt::box_value(
      winrt::hstring(std::wstring(podlens::Tr("about.issue")))));
  issue.NavigateUri(winrt::Windows::Foundation::Uri{
      L"https://github.com/turinglambdaai/podlens/issues"});
  issue.Padding({0, 0, 0, 0});
  about.Children().Append(issue);
  panel.Children().Append(about);

  muxc::ScrollViewer scroll;
  scroll.Content(panel);
  scroll.MaxHeight(420);

  muxc::ContentDialog dialog;
  dialog.Title(box_value(winrt::hstring(std::wstring(podlens::Tr("dialog.settings_title")))));
  dialog.Content(scroll);
  dialog.PrimaryButtonText(winrt::hstring(std::wstring(podlens::Tr("dialog.save"))));
  dialog.CloseButtonText(winrt::hstring(std::wstring(podlens::Tr("dialog.cancel"))));
  dialog.XamlRoot(Content().XamlRoot());

  auto const weak = get_weak();
  auto op = dialog.ShowAsync();
  op.Completed([weak, fields = std::move(fields)](
                   winrt::Windows::Foundation::IAsyncOperation<
                       muxc::ContentDialogResult> const& sender,
                   winrt::Windows::Foundation::AsyncStatus) {
    if (sender.GetResults() != muxc::ContentDialogResult::Primary) return;
    auto const window = weak.get();
    if (!window || !window->api_) return;
    try {
      for (auto const& [key, input] : fields) {
        window->api_->settings_set(key, to_utf8(std::wstring(input.Text()))).get();
      }
      window->SetStatus(true, std::wstring(podlens::Tr("status.saved")));
    } catch (std::exception const& e) {
      window->SetStatus(false, to_wide(e.what()));
    }
  });
}

}  // namespace winrt::RivetHost::implementation
