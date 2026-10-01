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

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cwchar>
#include <fstream>
#include <future>
#include <thread>
#include <type_traits>

namespace winrt::RivetHost::implementation {
namespace {

using Microsoft::UI::Xaml::Visibility;
namespace mux = winrt::Microsoft::UI::Xaml;
namespace muxc = winrt::Microsoft::UI::Xaml::Controls;

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

// WinRT TimeSpans tick in 100 ns units; playback math speaks in seconds.
double TimeSpanSeconds(winrt::Windows::Foundation::TimeSpan const& span) {
  return static_cast<double>(span.count()) / 1e7;
}

}  // namespace

MainWindow::MainWindow() {
  InitializeComponent();

  Title(winrt::hstring(std::wstring(podlens::Tr("app.title"))));
  SubtitleText().Text(std::wstring(podlens::Tr("app.subtitle")));
  StatusLine().Text(std::wstring(podlens::Tr("status.starting")));
  AddFeedButton().Content(box_value(winrt::hstring(
      L"＋ " + std::wstring(podlens::Tr("nav.add_feed")))));
  DiscoverButton().Content(box_value(winrt::hstring(
      std::wstring(podlens::Tr("menu.discover")))));
  SettingsNavLabel().Text(std::wstring(podlens::Tr("menu.settings")));
  UpdatesNavLabel().Text(std::wstring(podlens::Tr("menu.check_updates")));
  EpisodesHeader().Text(std::wstring(podlens::Tr("episodes.header")));
  DetailEmptyTitle().Text(std::wstring(podlens::Tr("detail.none")));
  DetailEmptyHint().Text(std::wstring(podlens::Tr("detail.hint")));
  DetailEmptyCta().Content(box_value(winrt::hstring(
      std::wstring(podlens::Tr("empty.discover_cta")))));
  EpisodesEmptyTitle().Text(std::wstring(podlens::Tr("empty.episodes_title")));
  EpisodesEmptyHint().Text(std::wstring(podlens::Tr("empty.episodes_hint")));
  DownloadButton().Content(box_value(winrt::hstring(
      std::wstring(podlens::Tr("action.download")))));
  TranscribeButton().Content(box_value(winrt::hstring(
      std::wstring(podlens::Tr("action.transcribe")))));
  TranslateButton().Content(box_value(winrt::hstring(
      std::wstring(podlens::Tr("action.translate")))));
  SummarizeButton().Content(box_value(winrt::hstring(
      std::wstring(podlens::Tr("action.summarize")))));
  ModeLabel().Text(std::wstring(podlens::Tr("mode.label")));
  TranscriptTab().Content(box_value(winrt::hstring(
      std::wstring(podlens::Tr("tab.transcript")))));
  SummaryTab().Content(box_value(winrt::hstring(
      std::wstring(podlens::Tr("tab.summary")))));

  // Filling the selectors fires SelectionChanged while the window is still
  // being constructed; initialized_ keeps those callbacks harmless.
  for (wchar_t const* label :
       {L"1.0×", L"1.25×", L"1.5×", L"1.75×", L"2.0×", L"2.5×", L"3.0×"}) {
    RateSelector().Items().Append(box_value(winrt::hstring(label)));
  }
  RateSelector().SelectedIndex(0);
  for (char const* key :
       {"mode.bilingual", "mode.translation", "mode.original"}) {
    ModeSelector().Items().Append(box_value(winrt::hstring(
        std::wstring(podlens::Tr(key)))));
  }
  ModeSelector().SelectedIndex(0);
  TranscriptTab().IsChecked(true);

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
              window->SetStatus(true, to_wide(update->value));
            } else if (auto const changed =
                           std::get_if<rivet_app::Episodes_changedEvent>(&event)) {
              window->ReloadFeeds();
              window->ReloadEpisodes();
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

// ---- view switching ---------------------------------------------------------

void MainWindow::ShowView(View view) {
  view_ = view;
  UpdateDetailVisibility();
}

void MainWindow::UpdateDetailVisibility() {
  bool const feeds_empty = feeds_.empty();
  bool const has_episode = !selected_episode_id_.empty();

  DetailEmpty().Visibility(
      feeds_empty || (!has_episode && view_ == View::Detail)
          ? Visibility::Visible
          : Visibility::Collapsed);
  if (feeds_empty) {
    DetailEmptyTitle().Text(std::wstring(podlens::Tr("empty.feeds_title")));
    DetailEmptyHint().Text(std::wstring(podlens::Tr("empty.feeds_hint")));
  } else {
    DetailEmptyTitle().Text(std::wstring(podlens::Tr("detail.none")));
    DetailEmptyHint().Text(std::wstring(podlens::Tr("detail.hint")));
  }

  DiscoverView().Visibility(view_ == View::Discover ? Visibility::Visible
                                                    : Visibility::Collapsed);
  DetailViewHost().Visibility(
      (view_ == View::Detail && has_episode) ? Visibility::Visible
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

void MainWindow::RenderFeeds(std::vector<std::vector<std::string>> rows) {
  feeds_.clear();
  Nav().MenuItems().Clear();
  for (auto const& row : rows) {
    if (row.size() < 5) continue;
    FeedRow feed;
    feed.id = row[0];
    feed.title = row[1];
    feed.artwork = row.size() > 3 ? row[3] : "";
    feed.count = row[4];
    feeds_.push_back(feed);

    muxc::NavigationViewItem item;
    item.Tag(box_value(winrt::hstring(to_wide(feed.id))));
    item.Content(box_value(winrt::hstring(
        to_wide(feed.title) + L"  (" + to_wide(feed.count) + L")")));
    muxc::FontIcon icon;
    icon.Glyph(winrt::hstring(L"\xE8F1"));
    item.Icon(icon);
    Nav().MenuItems().Append(item);
  }
  // Setting item.IsSelected before the pane's containers are realized is
  // silently dropped; SelectedItem goes through the view and fires
  // SelectionChanged reliably.
  if (!feeds_.empty()) {
    Nav().SelectedItem(Nav().MenuItems().GetAt(0));
  }
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
    if (row.size() < 10) continue;
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
    EpisodeList().Items().Append(card);
  }

  bool const empty = episodes_.empty();
  EpisodesEmpty().Visibility(empty ? Visibility::Visible : Visibility::Collapsed);
  EpisodeList().Visibility(empty ? Visibility::Collapsed : Visibility::Visible);
  UpdateDetailVisibility();
  UpdateActionButtons();
}

void MainWindow::RenderDetailHeader() {
  EpisodeRow const* episode = nullptr;
  for (auto const& candidate : episodes_) {
    if (candidate.id == selected_episode_id_) {
      episode = &candidate;
      break;
    }
  }
  if (!episode) return;
  DetailTitle().Text(winrt::hstring(to_wide(episode->title)));
  std::wstring meta = to_wide(episode->pub);
  if (episode->duration_sec > 0) {
    meta += L" · " + FormatSeconds(episode->duration_sec);
  }
  if (episode->position_sec > 5 && !episode->done) {
    meta += L" · " + std::wstring(podlens::Tr("detail.position_prefix")) +
            FormatSeconds(episode->position_sec);
  }
  DetailMeta().Text(winrt::hstring(meta));
  DurationText().Text(winrt::hstring(FormatSeconds(episode->duration_sec)));
}

void MainWindow::UpdateActionButtons() {
  EpisodeRow const* episode = nullptr;
  for (auto const& candidate : episodes_) {
    if (candidate.id == selected_episode_id_) {
      episode = &candidate;
      break;
    }
  }
  if (!episode) {
    DownloadButton().IsEnabled(false);
    TranscribeButton().IsEnabled(false);
    TranslateButton().IsEnabled(false);
    SummarizeButton().IsEnabled(false);
    return;
  }
  bool const transcript_running = episode->transcript_status == "running";
  bool const summary_running = episode->summary_status == "running";
  DownloadButton().IsEnabled(!episode->downloaded);
  TranscribeButton().IsEnabled(!transcript_running &&
                               episode->transcript_status != "done");
  TranslateButton().IsEnabled(episode->transcript_status == "done" &&
                              !episode->has_translation);
  SummarizeButton().IsEnabled(episode->transcript_status == "done" &&
                              !summary_running && episode->summary_status != "done");
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
    translation.Visibility(mode_ == 2 ? Visibility::Collapsed : Visibility::Visible);

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
    row.Margin(winrt::Microsoft::UI::Xaml::Thickness{0, 0, 0, 2});
    row.Tag(box_value(winrt::hstring(std::to_wstring(i))));
    row.Tapped([weak = get_weak()](winrt::Windows::Foundation::IInspectable const& sender,
                                   Microsoft::UI::Xaml::Input::TappedRoutedEventArgs const&) {
      auto const border = sender.as<muxc::Border>();
      auto const index = std::stoi(std::wstring(
          winrt::unbox_value<winrt::hstring>(border.Tag())));
      if (auto window = weak.get()) {
        if (window->player_ && index < static_cast<int>(window->transcript_.size())) {
          auto const& seg = window->transcript_[static_cast<size_t>(index)];
          double const start = std::wcstod(to_wide(seg[0]).c_str(), nullptr);
          window->player_.Position(
              std::chrono::duration_cast<winrt::Windows::Foundation::TimeSpan>(
                  std::chrono::duration<double>(start)));
        }
      }
    });
    segment_borders_.push_back(row);
    TranscriptList().Items().Append(row);
  }
}

void MainWindow::ApplyMode() {
  for (auto const& block : translation_blocks_) {
    block.Visibility(mode_ == 2 ? Visibility::Collapsed : Visibility::Visible);
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
    TranscriptList().ScrollIntoView(border);
  }
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

// ---- playback -------------------------------------------------------------------

void MainWindow::StartPlayback() {
  auto const episode_id = selected_episode_id_;
  if (episode_id.empty()) return;
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

    EpisodeRow const* episode = nullptr;
    for (auto const& candidate : episodes_) {
      if (candidate.id == episode_id) {
        episode = &candidate;
        break;
      }
    }
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
  if (player_) player_.Pause();
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
    SavePosition(true);
    player_.Pause();
    PlayPauseGlyph().Glyph(winrt::hstring(L"\xE768"));
    SetSmtcStatus(false);
    SetStatus(true, std::wstring(podlens::Tr("status.playback_done")));
    ReloadEpisodes();
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
  if (auto const item = Nav().SelectedItem().try_as<muxc::NavigationViewItem>()) {
    auto const tag = winrt::unbox_value_or<winrt::hstring>(item.Tag(), {});
    return to_utf8(std::wstring(tag));
  }
  return {};
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
    ::getenv_s(&required, value.data(), value.size(), "PODLENS_DATA_DIR");
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
        window->ReloadEpisodes();
        if (window->selected_episode_id_ == episode_id) {
          if (kind == "transcribe" || kind == "translate") window->LoadTranscript();
          if (kind == "summarize") window->LoadSummary(episode_id);
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
  if (!initialized_) return;
  auto const item = Nav().SelectedItem().try_as<muxc::NavigationViewItem>();
  if (!item) return;
  auto const tag = winrt::unbox_value_or<winrt::hstring>(item.Tag(), {});
  std::wstring const key(tag);
  if (key == L"settings") {
    RunSettingsDialog();
    return;
  }
  if (key == L"updates") {
    RunUpdateCheck();
    return;
  }
  // a feed: load its episodes and show the detail view
  if (!key.empty()) {
    ShowView(View::Detail);
    ReloadEpisodes();
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
  summary_tab_active_ = false;
  TranscriptTab().IsChecked(true);
  SummaryTab().IsChecked(false);
  TranscriptList().Visibility(Visibility::Visible);
  SummaryView().Visibility(Visibility::Collapsed);
  if (!selected_episode_id_.empty()) {
    LoadTranscript();
    LoadSummary(selected_episode_id_);
  }
}

void MainWindow::Mode_SelectionChanged(
    winrt::Windows::Foundation::IInspectable const&,
    Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const&) {
  if (!initialized_) return;
  mode_ = ModeSelector().SelectedIndex() < 0 ? 0
                                             : static_cast<uint32_t>(ModeSelector().SelectedIndex());
  ApplyMode();
}

void MainWindow::TranscriptTab_Click(winrt::Windows::Foundation::IInspectable const&,
                                     Microsoft::UI::Xaml::RoutedEventArgs const&) {
  summary_tab_active_ = false;
  TranscriptTab().IsChecked(true);
  SummaryTab().IsChecked(false);
  TranscriptList().Visibility(Visibility::Visible);
  SummaryView().Visibility(Visibility::Collapsed);
}

void MainWindow::SummaryTab_Click(winrt::Windows::Foundation::IInspectable const&,
                                  Microsoft::UI::Xaml::RoutedEventArgs const&) {
  summary_tab_active_ = true;
  SummaryTab().IsChecked(true);
  TranscriptTab().IsChecked(false);
  TranscriptList().Visibility(Visibility::Collapsed);
  SummaryView().Visibility(Visibility::Visible);
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
          } else {
            window->SetStatus(false, to_wide(error));
          }
        }
      });
    }).detach();
  });
}

void MainWindow::Discover_Click(winrt::Windows::Foundation::IInspectable const&,
                                Microsoft::UI::Xaml::RoutedEventArgs const&) {
  LoadDiscover();
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
        window->DiscoverList().Children().Clear();
        std::wstring current_category;
        for (auto const& row : rows) {
          if (row.size() < 7) continue;
          std::wstring const category = to_wide(row[1]);
          if (category != current_category) {
            current_category = category;
            auto label = SectionLabel(std::wstring(podlens::Tr(
                ("cat." + row[1]).c_str())));
            if (label.Text().size() == 0) label.Text(winrt::hstring(category));
            label.Margin(winrt::Microsoft::UI::Xaml::Thickness{0, 12, 0, 2});
            window->DiscoverList().Children().Append(label);
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

          bool const added = row[6] == "1";
          muxc::Button action;
          action.Content(box_value(winrt::hstring(std::wstring(podlens::Tr(
              added ? "discover.added" : "discover.add")))));
          action.IsEnabled(!added);
          auto const feed_url = to_utf8(to_wide(row[5]));
          action.Click([weak, feed_url](auto&&, auto&&) {
            if (auto window = weak.get()) window->AddFromCatalog(feed_url);
          });

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
          muxc::Grid::SetColumn(action, 1);
          lines.Children().Append(text);
          lines.Children().Append(action);

          muxc::Border card;
          card.Child(lines);
          card.CornerRadius(winrt::Microsoft::UI::Xaml::CornerRadius{8});
          card.Padding(winrt::Microsoft::UI::Xaml::Thickness{14, 10, 14, 10});
          card.Margin(winrt::Microsoft::UI::Xaml::Thickness{0, 0, 0, 8});
          card.Background(Microsoft::UI::Xaml::Application::Current()
                              .Resources()
                              .Lookup(box_value(winrt::hstring(L"AppCardBrush")))
                              .as<Microsoft::UI::Xaml::Media::Brush>());
          window->DiscoverList().Children().Append(card);
        }

        muxc::TextBlock note;
        note.Text(winrt::hstring(std::wstring(podlens::Tr("discover.note"))));
        note.FontSize(12);
        note.Opacity(0.6);
        note.TextWrapping(Microsoft::UI::Xaml::TextWrapping::Wrap);
        note.Margin(winrt::Microsoft::UI::Xaml::Thickness{0, 8, 0, 0});
        window->DiscoverList().Children().Append(note);

        window->SetStatus(true, std::wstring(podlens::Tr("discover.title")));
        window->ShowView(View::Discover);
      }
    });
  }).detach();
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
        window->ReloadEpisodes();
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
      dispatcher.TryEnqueue([weak, error] {
        if (auto window = weak.get()) {
          if (error.empty()) {
            window->SetStatus(true, std::wstring(podlens::Tr("status.feed_removed")));
          } else {
            window->SetStatus(false, to_wide(error));
          }
          window->ReloadFeeds();
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
  RunUpdateCheck();
}

void MainWindow::RunUpdateCheck() {
  if (!api_) return;
  auto const dispatcher = DispatcherQueue();
  auto const weak = get_weak();
  std::thread([weak, dispatcher]() mutable {
    std::string status;
    std::string error;
    if (weak.get()) {
      try {
        status = weak.get()->api_->update_check().get();
      } catch (std::exception const& e) {
        error = e.what();
      }
    }
    dispatcher.TryEnqueue([weak, status, error] {
      if (auto window = weak.get()) {
        if (error.empty()) {
          window->SetStatus(true, to_wide(status));
        } else {
          window->SetStatus(false, to_wide(error));
        }
      }
    });
  }).detach();
}

void MainWindow::RunSettingsDialog() {
  if (!api_) return;
  auto const dispatcher = DispatcherQueue();
  auto const weak = get_weak();
  std::thread([weak, dispatcher]() mutable {
    std::string error;
    std::vector<std::vector<std::string>> rows;
    if (weak.get()) {
      try {
        rows = weak.get()->api_->settings_list().get();
      } catch (std::exception const& e) {
        error = e.what();
      }
    }
    dispatcher.TryEnqueue([weak, rows = std::move(rows), error]() mutable {
      if (auto window = weak.get()) {
        window->ShowSettingsDialog(rows, error);
      }
    });
  }).detach();
}

void MainWindow::ShowSettingsDialog(std::vector<std::vector<std::string>> rows,
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
