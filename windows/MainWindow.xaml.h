#pragma once

#include "pch.h"
#include "MainWindow.g.h"
#include "GeneratedBackend.hpp"

#include <map>
#include <memory>
#include <string>
#include <vector>

namespace winrt::RivetHost::implementation {

struct MainWindow : MainWindowT<MainWindow> {
  MainWindow();

  // XAML event handlers (MainWindow.xaml).
  void AddFeed_Click(winrt::Windows::Foundation::IInspectable const&,
                     Microsoft::UI::Xaml::RoutedEventArgs const&);
  void Discover_Click(winrt::Windows::Foundation::IInspectable const&,
                      Microsoft::UI::Xaml::RoutedEventArgs const&);
  void RefreshAll_Click(winrt::Windows::Foundation::IInspectable const&,
                        Microsoft::UI::Xaml::RoutedEventArgs const&);
  void Download_Click(winrt::Windows::Foundation::IInspectable const&,
                      Microsoft::UI::Xaml::RoutedEventArgs const&);
  void Transcribe_Click(winrt::Windows::Foundation::IInspectable const&,
                        Microsoft::UI::Xaml::RoutedEventArgs const&);
  void Translate_Click(winrt::Windows::Foundation::IInspectable const&,
                       Microsoft::UI::Xaml::RoutedEventArgs const&);
  void Summarize_Click(winrt::Windows::Foundation::IInspectable const&,
                       Microsoft::UI::Xaml::RoutedEventArgs const&);
  void Play_Click(winrt::Windows::Foundation::IInspectable const&,
                  Microsoft::UI::Xaml::RoutedEventArgs const&);
  void Settings_Click(winrt::Windows::Foundation::IInspectable const&,
                      Microsoft::UI::Xaml::RoutedEventArgs const&);
  void CheckUpdates_Click(winrt::Windows::Foundation::IInspectable const&,
                          Microsoft::UI::Xaml::RoutedEventArgs const&);
  void FeedList_SelectionChanged(winrt::Windows::Foundation::IInspectable const&,
                                 Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const&);
  void EpisodeList_SelectionChanged(winrt::Windows::Foundation::IInspectable const&,
                                    Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const&);
  void Mode_SelectionChanged(winrt::Windows::Foundation::IInspectable const&,
                             Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const&);

 private:
  winrt::fire_and_forget InitializeBackendAsync();
  void ReloadFeeds();
  void RenderFeeds(std::vector<std::vector<std::string>> rows);
  void RenderEpisodes(std::vector<std::vector<std::string>> rows);
  void RenderDetail();
  void ReloadEpisodes();
  void LoadTranscript();
  void LoadSummary(std::string const& episode_id);
  void StartJob(std::string const& kind, std::string const& episode_id);
  std::string SelectedFeedId();
  std::string SelectedEpisodeId();
  std::wstring FindLocalAudio(std::string const& episode_id);
  void SetStatus(bool ok, std::wstring const& message);
  void UpdateButtons();

  struct FeedRow {
    std::string id;
    std::string title;
    std::string count;
  };
  struct EpisodeRow {
    std::string id;
    std::string title;
    std::string pub;
    std::string transcript_status;
    std::string summary_status;
    bool downloaded = false;
    bool has_translation = false;
  };

  std::shared_ptr<rivet::windows::Backend> backend_;
  std::unique_ptr<rivet_app::API> api_;
  std::vector<FeedRow> feeds_;
  std::vector<EpisodeRow> episodes_;
  std::vector<std::vector<std::string>> transcript_;
  std::vector<std::vector<std::string>> settings_;
  std::map<std::string, std::string> summary_by_episode_;
  std::string selected_episode_id_;
  std::atomic<bool> play_state_{false};
};

}  // namespace winrt::RivetHost::implementation

namespace winrt::RivetHost::factory_implementation {

struct MainWindow : MainWindowT<MainWindow, implementation::MainWindow> {};

}  // namespace winrt::RivetHost::factory_implementation
