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
#include <winrt/Windows.Storage.h>
#include <winrt/Windows.Storage.Streams.h>

#include <chrono>
#include <fstream>
#include <future>
#include <thread>

namespace winrt::RivetHost::implementation {
namespace {

using Microsoft::UI::Xaml::Controls::InfoBarSeverity;

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

// Minimal JSON string extraction for the summary payload — the backend
// hands over {"tldr": "...", "key-points": [...], ...}; this pulls the
// tldr for the detail pane without pulling in a JSON library.
std::string json_tldr(std::string const& json) {
  auto const key = json.find("\"tldr\"");
  if (key == std::string::npos) return {};
  auto const colon = json.find(':', key);
  if (colon == std::string::npos) return {};
  auto const open = json.find('"', colon);
  if (open == std::string::npos) return {};
  std::string out;
  for (auto i = open + 1; i < json.size(); ++i) {
    if (json[i] == '\\' && i + 1 < json.size()) {
      out += json[i + 1];
      ++i;
      continue;
    }
    if (json[i] == '"') break;
    out += json[i];
  }
  return out;
}

}  // namespace

MainWindow::MainWindow() {
  InitializeComponent();
  Title(winrt::hstring(std::wstring(podlens::Tr("app.title"))));
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

// ---- rendering -------------------------------------------------------------

void MainWindow::RenderFeeds(std::vector<std::vector<std::string>> rows) {
  feeds_.clear();
  FeedList().Items().Clear();
  for (auto const& row : rows) {
    if (row.size() < 5) continue;
    FeedRow feed;
    feed.id = row[0];
    feed.title = row[1];
    feed.count = row[4];
    feeds_.push_back(feed);
    auto line = to_wide(feed.title) + L"  (" + to_wide(feed.count) + L")";
    FeedList().Items().Append(winrt::box_value(winrt::hstring(line)));
  }
}

void MainWindow::RenderEpisodes(std::vector<std::vector<std::string>> rows) {
  episodes_.clear();
  EpisodeList().Items().Clear();
  for (auto const& row : rows) {
    if (row.size() < 10) continue;
    EpisodeRow episode;
    episode.id = row[0];
    episode.title = row[1];
    episode.pub = row[2];
    episode.downloaded = row[4] == "1";
    episode.transcript_status = row[5];
    episode.summary_status = row[6];
    episode.has_translation = row[9] == "1";
    episodes_.push_back(episode);

    std::wstring badges;
    if (episode.downloaded) badges += L"⬇ ";
    if (episode.transcript_status == "done") badges += L"文 ";
    if (episode.transcript_status == "running") badges += L"⏳ ";
    if (episode.transcript_status == "error") badges += L"✗ ";
    if (episode.summary_status == "done") badges += L"✨ ";
    if (episode.has_translation) badges += L"译 ";
    auto line = badges + to_wide(episode.title) + L"  ·  " + to_wide(episode.pub);
    EpisodeList().Items().Append(winrt::box_value(winrt::hstring(line)));
  }
  UpdateButtons();
}

void MainWindow::RenderDetail() {
  // mode: 0 bilingual, 1 translation only, 2 original only
  uint32_t mode = 0;
  if (auto checked = ModeSelector().SelectedItem().try_as<
          Microsoft::UI::Xaml::Controls::RadioButton>()) {
    uint32_t index = 0;
    if (ModeSelector().Items().IndexOf(checked, index)) mode = index;
  }

  std::wstring text;
  if (!selected_episode_id_.empty()) {
    auto const it = summary_by_episode_.find(selected_episode_id_);
    if (it != summary_by_episode_.end() && !it->second.empty()) {
      auto const tldr = json_tldr(it->second);
      if (!tldr.empty()) {
        text += L"✨ " + to_wide(tldr) + L"\n\n";
      }
    }
  }
  if (transcript_.empty()) {
    text += std::wstring(podlens::Tr("detail.empty"));
  }
  for (auto const& seg : transcript_) {
    if (seg.size() < 4) continue;
    if (mode != 2 && !seg[3].empty()) {
      text += to_wide(seg[3]);
      if (mode == 0 && !seg[2].empty()) text += L"\n" + to_wide(seg[2]);
    } else {
      text += to_wide(seg[2]);
    }
    text += L"\n\n";
  }
  DetailText().Text(winrt::hstring(text));
}

// ---- data loading -----------------------------------------------------------

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
              window->RenderDetail();
            } catch (std::exception const&) {
              window->transcript_.clear();
              window->RenderDetail();
            }
          }
        });
      });
}

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
              window->RenderDetail();
            } catch (std::exception const&) {
              // empty summary stays absent
            }
          }
        });
      });
}

// ---- episode actions ---------------------------------------------------------

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

// ---- helpers ----------------------------------------------------------------

void MainWindow::SetStatus(bool ok, std::wstring const& message) {
  StatusBar().Severity(ok ? InfoBarSeverity::Success : InfoBarSeverity::Error);
  StatusBar().Message(winrt::hstring(message));
}

void MainWindow::UpdateButtons() {
  bool const has_episode = !selected_episode_id_.empty();
  DownloadButton().IsEnabled(has_episode);
  TranscribeButton().IsEnabled(has_episode);
  TranslateButton().IsEnabled(has_episode);
  SummarizeButton().IsEnabled(has_episode);
  PlayButton().IsEnabled(has_episode);
}

std::string MainWindow::SelectedFeedId() {
  auto const index = FeedList().SelectedIndex();
  if (index < 0 || index >= static_cast<int>(feeds_.size())) return {};
  return feeds_[static_cast<size_t>(index)].id;
}

std::string MainWindow::SelectedEpisodeId() {
  auto const index = EpisodeList().SelectedIndex();
  if (index < 0 || index >= static_cast<int>(episodes_.size())) return {};
  return episodes_[static_cast<size_t>(index)].id;
}

// The backend caches audio under %USERPROFILE%\.podlens\audio\<episodeId>*;
// the host resolves the file by id prefix so playback needs no extra RPC.
std::wstring MainWindow::FindLocalAudio(std::string const& episode_id) {
  PWSTR profile = nullptr;
  if (::SHGetKnownFolderPath(FOLDERID_Profile, 0, nullptr, &profile) != S_OK) {
    return {};
  }
  std::filesystem::path dir = std::filesystem::path(profile) / L".podlens" / L"audio";
  ::CoTaskMemFree(profile);
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

// ---- XAML event handlers ------------------------------------------------------

void MainWindow::FeedList_SelectionChanged(
    winrt::Windows::Foundation::IInspectable const&,
    Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const&) {
  transcript_.clear();
  selected_episode_id_.clear();
  RenderDetail();
  ReloadEpisodes();
}

void MainWindow::EpisodeList_SelectionChanged(
    winrt::Windows::Foundation::IInspectable const&,
    Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const&) {
  selected_episode_id_ = SelectedEpisodeId();
  transcript_.clear();
  UpdateButtons();
  RenderDetail();
  if (!selected_episode_id_.empty()) {
    LoadTranscript();
    LoadSummary(selected_episode_id_);
  }
}

void MainWindow::Mode_SelectionChanged(
    winrt::Windows::Foundation::IInspectable const&,
    Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const&) {
  RenderDetail();
}

void MainWindow::AddFeed_Click(winrt::Windows::Foundation::IInspectable const&,
                               Microsoft::UI::Xaml::RoutedEventArgs const&) {
  if (!api_) return;
  auto const dispatcher = DispatcherQueue();
  auto const weak = get_weak();

  Microsoft::UI::Xaml::Controls::TextBox input;
  input.PlaceholderText(winrt::hstring(L"https://example.com/feed.xml"));
  input.Width(420);

  auto dialog = winrt::Microsoft::UI::Xaml::Controls::ContentDialog();
  dialog.Title(winrt::box_value(winrt::hstring(std::wstring(podlens::Tr("menu.add_feed")))));
  dialog.Content(input);
  dialog.PrimaryButtonText(winrt::hstring(std::wstring(podlens::Tr("dialog.primary_add"))));
  dialog.CloseButtonText(winrt::hstring(std::wstring(podlens::Tr("dialog.cancel"))));
  dialog.XamlRoot(Content().XamlRoot());

  auto op = dialog.ShowAsync();
  op.Completed([weak, input, dispatcher](
                   winrt::Windows::Foundation::IAsyncOperation<
                       winrt::Microsoft::UI::Xaml::Controls::ContentDialogResult> const&
                       sender,
                   winrt::Windows::Foundation::AsyncStatus) {
    if (sender.GetResults() !=
        winrt::Microsoft::UI::Xaml::Controls::ContentDialogResult::Primary) {
      return;
    }
    auto const url = to_utf8(input.Text());
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

void MainWindow::Play_Click(winrt::Windows::Foundation::IInspectable const&,
                            Microsoft::UI::Xaml::RoutedEventArgs const&) {
  auto const episode_id = selected_episode_id_;
  if (episode_id.empty()) return;
  if (play_state_.exchange(false)) {
    // simple toggle: a fresh player per press keeps state trivial in v1
    play_state_.store(false);
  }
  auto const audio = FindLocalAudio(episode_id);
  if (audio.empty()) {
    SetStatus(true, std::wstring(podlens::Tr("status.need_download")));
    StartJob("download", episode_id);
    return;
  }
  try {
    winrt::Windows::Media::Playback::MediaPlayer player;
    auto file = winrt::Windows::Storage::StorageFile::GetFileFromPathAsync(audio).get();
    auto stream = winrt::Windows::Storage::Streams::RandomAccessStreamReference::CreateFromFile(file);
    player.Source(winrt::Windows::Media::Core::MediaSource::CreateFromStorageFile(file));
    player.Play();
    // Detach: the player keeps playing after this scope. One static reuse
    // point keeps v1 simple; this leaks one player per session at most.
    static winrt::Windows::Media::Playback::MediaPlayer active{nullptr};
    active = player;
    play_state_.store(true);
    SetStatus(true, std::wstring(podlens::Tr("status.playing")));
  } catch (winrt::hresult_error const& e) {
    SetStatus(false, e.message().c_str());
  }
}

void MainWindow::Settings_Click(winrt::Windows::Foundation::IInspectable const&,
                                Microsoft::UI::Xaml::RoutedEventArgs const&) {
  if (!api_) return;
  // v1: show current settings read-only with a hint to edit config.json /
  // use the CLI for changes; a full editing dialog is the 1.1 milestone.
  try {
    auto rows = api_->settings_list().get();
    std::wstring text;
    for (auto const& row : rows) {
      if (row.size() < 3) continue;
      text += to_wide(row[0]) + L" = " + to_wide(row[1]) + L"\n    " + to_wide(row[2]) + L"\n";
    }
    DetailText().Text(winrt::hstring(text));
    SetStatus(true, std::wstring(podlens::Tr("status.settings_hint")));
  } catch (std::exception const& e) {
    SetStatus(false, to_wide(e.what()));
  }
}

void MainWindow::CheckUpdates_Click(winrt::Windows::Foundation::IInspectable const&,
                                    Microsoft::UI::Xaml::RoutedEventArgs const&) {
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

}  // namespace winrt::RivetHost::implementation
