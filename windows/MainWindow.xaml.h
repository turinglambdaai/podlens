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
  void Nav_ItemInvoked(winrt::Windows::Foundation::IInspectable const&,
                       Microsoft::UI::Xaml::Controls::NavigationViewItemInvokedEventArgs const&);
  void Nav_SelectionChanged(winrt::Windows::Foundation::IInspectable const&,
                            Microsoft::UI::Xaml::Controls::NavigationViewSelectionChangedEventArgs const&);
  void AddFeed_Click(winrt::Windows::Foundation::IInspectable const&,
                     Microsoft::UI::Xaml::RoutedEventArgs const&);
  void Discover_Click(winrt::Windows::Foundation::IInspectable const&,
                      Microsoft::UI::Xaml::RoutedEventArgs const&);
  void RefreshFeed_Click(winrt::Windows::Foundation::IInspectable const&,
                         Microsoft::UI::Xaml::RoutedEventArgs const&);
  void RemoveFeed_Click(winrt::Windows::Foundation::IInspectable const&,
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
  void Settings_Click(winrt::Windows::Foundation::IInspectable const&,
                      Microsoft::UI::Xaml::RoutedEventArgs const&);
  void CheckUpdates_Click(winrt::Windows::Foundation::IInspectable const&,
                          Microsoft::UI::Xaml::RoutedEventArgs const&);
  void PlayPause_Click(winrt::Windows::Foundation::IInspectable const&,
                       Microsoft::UI::Xaml::RoutedEventArgs const&);
  void SeekSlider_ValueChanged(winrt::Windows::Foundation::IInspectable const&,
                               Microsoft::UI::Xaml::Controls::Primitives::RangeBaseValueChangedEventArgs const&);
  void SeekSlider_PointerPressed(winrt::Windows::Foundation::IInspectable const&,
                                 Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const&);
  void SeekSlider_PointerReleased(winrt::Windows::Foundation::IInspectable const&,
                                  Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const&);
  void Rate_SelectionChanged(winrt::Windows::Foundation::IInspectable const&,
                             Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const&);
  void TranscriptTab_Click(winrt::Windows::Foundation::IInspectable const&,
                           Microsoft::UI::Xaml::RoutedEventArgs const&);
  void SummaryTab_Click(winrt::Windows::Foundation::IInspectable const&,
                        Microsoft::UI::Xaml::RoutedEventArgs const&);
  void EpisodeList_SelectionChanged(winrt::Windows::Foundation::IInspectable const&,
                                    Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const&);
  void Mode_SelectionChanged(winrt::Windows::Foundation::IInspectable const&,
                             Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const&);

 private:
  winrt::fire_and_forget InitializeBackendAsync();

  // view switching
  enum class View { Detail, Discover };
  void ShowView(View view);

  // data loading + rendering
  void ReloadFeeds();
  void RenderFeeds(std::vector<std::vector<std::string>> rows);
  void ReloadEpisodes();
  void RenderEpisodes(std::vector<std::vector<std::string>> rows);
  void LoadTranscript();
  void RenderTranscript();
  void ApplyMode();
  void LoadSummary(std::string const& episode_id);
  void RenderSummary();
  void RenderDetailHeader();
  void UpdateDetailVisibility();
  void UpdateActionButtons();
  void HighlightRunningSegment(double position_seconds);

  // playback
  void StartPlayback();
  void TogglePlayPause();
  void StopPlayback(bool save);
  void TickPlayer();
  void SavePosition(bool done);
  static std::wstring FormatTime(double seconds);

  void StartJob(std::string const& kind, std::string const& episode_id);
  std::string SelectedFeedId();
  std::string SelectedEpisodeId();
  std::wstring FindLocalAudio(std::string const& episode_id);
  void SetStatus(bool ok, std::wstring const& message);
  // flows shared by nav items and dialogs (winrt reference params cannot be
  // null, so nav dispatch calls these instead of the event handlers)
  void LoadDiscover();
  void AddFromCatalog(std::string const& url);
  void RunSettingsDialog();
  void RunUpdateCheck();
  void ShowSettingsDialog(std::vector<std::vector<std::string>> rows,
                          std::string const& error);

  struct FeedRow {
    std::string id;
    std::string title;
    std::string count;
  };
  struct EpisodeRow {
    std::string id;
    std::string title;
    std::string pub;
    double position_sec = 0;
    double duration_sec = 0;
    bool downloaded = false;
    bool done = false;
    bool has_translation = false;
    std::string transcript_status;
    std::string summary_status;
  };

  std::shared_ptr<rivet::windows::Backend> backend_;
  std::unique_ptr<rivet_app::API> api_;
  std::vector<FeedRow> feeds_;
  std::vector<EpisodeRow> episodes_;
  std::vector<std::vector<std::string>> transcript_;
  std::map<std::string, std::string> summary_by_episode_;
  std::string selected_episode_id_;
  View view_ = View::Detail;
  uint32_t mode_ = 0;  // 0 bilingual, 1 translation only, 2 original only
  bool summary_tab_active_ = false;

  // segment borders for playback highlight; index == transcript_ index
  std::vector<Microsoft::UI::Xaml::Controls::Border> segment_borders_;
  std::vector<Microsoft::UI::Xaml::Controls::TextBlock> translation_blocks_;
  int highlighted_segment_ = -1;
  // XAML fires SelectionChanged while the constructor is still wiring items
  // up; guards keep those early callbacks from touching half-built state.
  bool initialized_ = false;

  // playback state; the player is a member so position/rate survive toggles
  winrt::Windows::Media::Playback::MediaPlayer player_{nullptr};
  winrt::Microsoft::UI::Dispatching::DispatcherQueueTimer position_timer_{nullptr};
  winrt::Microsoft::UI::Dispatching::DispatcherQueueTimer error_bar_timer_{nullptr};
  double duration_ = 0;
  bool user_seeking_ = false;
  bool syncing_ui_ = false;
  int ticks_since_save_ = 0;
};

}  // namespace winrt::RivetHost::implementation

namespace winrt::RivetHost::factory_implementation {

struct MainWindow : MainWindowT<MainWindow, implementation::MainWindow> {};

}  // namespace winrt::RivetHost::factory_implementation
