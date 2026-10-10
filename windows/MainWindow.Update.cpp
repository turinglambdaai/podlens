// Online update flow for the PodLens window (docs/UPDATE.md), ported from
// taskly's MainWindow.Update.cpp — the family shape: silent throttled
// launch check, manual check, 400 ms progress polling, and the detached
// handoff-script install. The handoff swaps the downloaded portable zip
// over the install directory itself; MSI installs (Program Files, no write
// access for the in-place swap) keep the manual path: a dialog pointing at
// the releases page.

#include "pch.h"
#include "MainWindow.xaml.h"
#include "GeneratedBackend.hpp"
#include "I18n.h"

#include <shellapi.h>

#include <chrono>
#include <cwchar>
#include <cwctype>
#include <fstream>
#include <functional>
#include <optional>
#include <string>

namespace winrt::RivetHost::implementation {
namespace {

namespace mx = winrt::Microsoft::UI::Xaml;
namespace mxc = winrt::Microsoft::UI::Xaml::Controls;

constexpr wchar_t kReleasesUrl[] =
    L"https://github.com/turinglambdaai/podlens/releases";

std::int64_t epoch_seconds() {
  FILETIME now{};
  ::GetSystemTimeAsFileTime(&now);
  ULARGE_INTEGER value{};
  value.LowPart = now.dwLowDateTime;
  value.HighPart = now.dwHighDateTime;
  // FILETIME is 100 ns ticks since 1601-01-01; the constant converts to
  // unix epoch seconds.
  return static_cast<std::int64_t>(
      (value.QuadPart - 116444736000000000ULL) / 10000000ULL);
}

std::wstring lowercase(std::wstring text) {
  for (auto& c : text) {
    c = static_cast<wchar_t>(::towlower(c));
  }
  return text;
}

// %TEMP%\podlens-update — handoff script, extract dir, and markers live
// here. Update state that must not pollute ~/.podlens/ (docs/UPDATE.md:
// an update never touches the data directory).
std::filesystem::path update_work_dir() {
  wchar_t buffer[MAX_PATH]{};
  std::filesystem::path base;
  auto const length = ::GetEnvironmentVariableW(L"TEMP", buffer, MAX_PATH);
  if (length > 0 && length < MAX_PATH) {
    base = std::filesystem::path(buffer);
  } else {
    auto const fallback = ::GetEnvironmentVariableW(L"TMP", buffer, MAX_PATH);
    if (fallback > 0 && fallback < MAX_PATH) {
      base = std::filesystem::path(buffer);
    }
  }
  if (base.empty()) {
    return {};
  }
  auto const dir = base / L"podlens-update";
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  return dir;
}

std::filesystem::path update_failure_marker() {
  return update_work_dir() / L"update-failed.txt";
}

std::filesystem::path update_success_marker() {
  return update_work_dir() / L"update-success.txt";
}

// The detached handoff batch. ASCII on purpose: cmd parses batches in the
// ANSI code page, so paths travel as arguments instead of being embedded in
// the script. Args: %1 zip, %2 install dir, %3 exe, %4 failure marker,
// %5 success marker. The zip is flat (release.yml packs
// dist\podlens-windows-x64\*, so RivetHost.exe / res / runtime sit at the
// archive root). Order matters: extract and verify BEFORE touching the old
// install, so a corrupt zip can never break the running copy.
std::string update_handoff_batch(unsigned long pid) {
  std::string const pid_text = std::to_string(pid);
  std::string batch;
  batch += "@echo off\r\n";
  batch += "rem PodLens update handoff - auto-generated, safe to delete.\r\n";
  batch += "set /a n=0\r\n";
  batch += ":wait\r\n";
  batch += "tasklist /FI \"PID eq " + pid_text + "\" 2>nul | find \"" +
           pid_text + "\" >nul\r\n";
  batch += "if errorlevel 1 goto extract\r\n";
  batch += "ping -n 2 127.0.0.1 >nul\r\n";
  batch += "set /a n+=1\r\n";
  batch += "if %n% LSS 30 goto wait\r\n";
  // The app never freed itself within 30 s: leave it running and report.
  batch += "> \"%~4\" echo process-exit-timeout\r\n";
  batch += "exit /b 1\r\n";
  batch += ":extract\r\n";
  batch += "set \"extract=%~dp0extract\"\r\n";
  batch += "if exist \"%extract%\" rmdir /s /q \"%extract%\"\r\n";
  batch += "mkdir \"%extract%\" 2>nul\r\n";
  batch += "tar -xf \"%~1\" -C \"%extract%\"\r\n";
  batch += "if errorlevel 1 goto fail_extract\r\n";
  batch += "if exist \"%~2.old\" rmdir /s /q \"%~2.old\"\r\n";
  batch += "move \"%~2\" \"%~2.old\" >nul 2>&1\r\n";
  batch += "if errorlevel 1 goto fail_rename\r\n";
  batch += "robocopy \"%extract%\" \"%~2\" /E /MOVE /NFL /NDL /NJH /NJS /NP >nul\r\n";
  batch += "if errorlevel 8 goto fail_swap\r\n";
  batch += "start \"\" \"%~3\"\r\n";
  batch += "> \"%~5\" echo ok\r\n";
  batch += "exit /b 0\r\n";
  batch += ":fail_swap\r\n";
  batch += "> \"%~4\" echo swap-failed (%errorlevel%)\r\n";
  batch += "move \"%~2.old\" \"%~2\" >nul 2>&1\r\n";
  batch += "goto restart_old\r\n";
  batch += ":fail_rename\r\n";
  batch += "> \"%~4\" echo rename-failed (%errorlevel%)\r\n";
  batch += "goto restart_old\r\n";
  batch += ":fail_extract\r\n";
  batch += "> \"%~4\" echo extract-failed (%errorlevel%)\r\n";
  batch += ":restart_old\r\n";
  batch += "start \"\" \"%~3\"\r\n";
  batch += "exit /b 1\r\n";
  return batch;
}

void open_releases_page() {
  ::ShellExecuteW(nullptr, L"open", kReleasesUrl, nullptr, nullptr,
                  SW_SHOWNORMAL);
}

// printf-style expansion of the i18n table's %s placeholders (the table
// itself stays plain data; sizing first keeps long paths unclipped).
std::wstring FormatUpdateMessage(wchar_t const* key, std::wstring const& arg) {
  std::wstring const format(podlens::Tr(key));
  int const size = swprintf(nullptr, 0, format.c_str(), arg.c_str());
  if (size <= 0) {
    return format;
  }
  std::wstring buffer(static_cast<size_t>(size) + 1, L'\0');
  swprintf(buffer.data(), static_cast<size_t>(size) + 1, format.c_str(),
           arg.c_str());
  buffer.resize(static_cast<size_t>(size));
  return buffer;
}

}  // namespace

// ---------- entry points ------------------------------------------------------

// Startup path: report a previous failed install first, then read the
// update settings once and schedule the throttled silent check (a few
// seconds after launch, at most once per 4 h; docs/UPDATE.md silent mode).
void MainWindow::StartAutoUpdateCheck() {
  if (backend_ == nullptr || !backend_->running()) {
    return;
  }
  auto const dispatcher = DispatcherQueue();
  auto const weak = get_weak();
  std::thread([weak, dispatcher]() mutable {
    std::vector<std::vector<std::string>> rows;
    if (weak.get()) {
      try {
        rows = weak.get()->api_->settings_list().get();
      } catch (std::exception const&) {
        // defaults below: silent checks on, never checked
      }
    }
    dispatcher.TryEnqueue([weak, rows = std::move(rows)] {
      auto const window = weak.get();
      if (!window) {
        return;
      }
      for (auto const& row : rows) {
        if (row.size() < 2) continue;
        if (row[0] == "check-updates-enabled") {
          window->check_updates_enabled_ = row[1] != "false";
        } else if (row[0] == "last-update-check") {
          try {
            window->last_update_check_ = std::stoll(row[1]);
          } catch (...) {
            window->last_update_check_ = 0;  // unset or non-numeric
          }
        }
      }
      // let the window settle before phoning home
      window->check_timer_ = window->DispatcherQueue().CreateTimer();
      window->check_timer_.Interval(std::chrono::seconds{3});
      window->check_timer_.IsRepeating(false);
      window->check_timer_.Tick([weak](auto&&, auto&&) {
        if (auto current = weak.get()) {
          current->RunSilentUpdateCheck();
        }
      });
      window->check_timer_.Start();
    });
  }).detach();
}

void MainWindow::RunSilentUpdateCheck() {
  if (backend_ == nullptr || !backend_->running() || update_downloading_) {
    return;
  }
  if (IsDevCopy()) {
    return;  // dev copies never phone home (docs/UPDATE.md silent mode)
  }
  if (IsMsiInstall()) {
    return;  // MSI installs take the manual path, never the silent one
  }
  if (!check_updates_enabled_) {
    return;  // the user turned automatic checks off (settings)
  }
  // Host-side throttle: at most one silent check per 4 h, persisted as the
  // `last-update-check` setting (unix epoch seconds). Manual checks bypass
  // this entirely.
  if (epoch_seconds() - last_update_check_ < 4 * 60 * 60) {
    return;  // throttled
  }
  RunUpdateCheck(/*silent=*/true);
}

// A manual check surfaces every outcome; a silent (launch-time) check never
// nags — it only reports an available update through the consent dialog.
void MainWindow::RunUpdateCheck(bool silent) {
  if (backend_ == nullptr || !backend_->running() || update_downloading_) {
    return;
  }
  if (IsDevCopy()) {
    if (!silent) {
      ShowUpdateDialog(L"PodLens",
                       std::wstring(podlens::Tr("update.dev_copy")),
                       std::wstring(podlens::Tr("menu.open_releases")),
                       std::wstring(podlens::Tr("dialog.cancel")),
                       [] { open_releases_page(); });
    }
    return;
  }
  if (IsMsiInstall()) {
    if (!silent) {
      ShowUpdateDialog(L"PodLens",
                       std::wstring(podlens::Tr("update.msi_installed")),
                       std::wstring(podlens::Tr("menu.open_releases")),
                       std::wstring(podlens::Tr("dialog.cancel")),
                       [] { open_releases_page(); });
    }
    return;
  }
  if (!silent) {
    SetStatus(true, std::wstring(podlens::Tr("update.checking")));
  }
  auto const dispatcher = DispatcherQueue();
  auto const weak = get_weak();
  std::thread([weak, dispatcher, silent]() mutable {
    bool ok = false;
    rivet_app::UpdateCheck check{};
    std::string failure;
    if (weak.get()) {
      try {
        check = weak.get()->api_->update_check().get();
        ok = true;
      } catch (std::exception const& e) {
        failure = e.what();
      }
    }
    dispatcher.TryEnqueue([weak, silent, ok, check,
                           failure = std::move(failure)]() mutable {
      // Stamp the throttle on every terminal path (docs/UPDATE.md triggers).
      auto window = weak.get();
      if (!window) {
        return;
      }
      window->RecordUpdateCheck();
      window->HandleUpdateCheckResult(ok, check, failure, silent);
    });
  }).detach();
}

void MainWindow::HandleUpdateCheckResult(
    bool ok, rivet_app::UpdateCheck const& check, std::string const& failure,
    bool silent) {
  if (!silent) {
    SetStatus(true, L"");  // clear the "Checking for updates…" status line
  }
  if (!ok) {
    if (!silent) {
      SetStatus(false, to_wide(failure));
    }
    return;
  }
  if (check.status == "available") {
    auto const version =
        check.available_version.value_or(check.current_version);
    ShowUpdateDialog(
        std::wstring(podlens::Tr("update.available_title")),
        FormatUpdateMessage(L"update.available_body", to_wide(version)),
        std::wstring(podlens::Tr("update.download")),
        std::wstring(podlens::Tr("dialog.cancel")),
        [weak = get_weak()] {
          if (auto window = weak.get()) {
            window->StartDownload();
          }
        });
    return;
  }
  if (silent) {
    return;  // up-to-date / errors never nag in silent mode
  }
  if (check.status == "up-to-date") {
    ShowUpdateDialog(std::wstring(podlens::Tr("menu.check_updates")),
                     std::wstring(podlens::Tr("update.up_to_date")), L"",
                     std::wstring(podlens::Tr("dialog.close")), nullptr);
    return;
  }
  // status "error": the record's reason beats the bare status word
  auto const detail = to_wide(check.error.value_or(check.status));
  SetStatus(false, detail);
}

void MainWindow::RecordUpdateCheck() {
  if (backend_ == nullptr || !backend_->running()) {
    return;
  }
  auto const dispatcher = DispatcherQueue();
  auto const weak = get_weak();
  std::thread([weak, dispatcher]() mutable {
    if (!weak.get()) {
      return;
    }
    try {
      weak.get()->api_->settings_set("last-update-check",
                                     std::to_string(epoch_seconds()))
          .get();
    } catch (...) {
      // Throttle stamping is best-effort.
    }
  }).detach();
}

// ---------- download -----------------------------------------------------------

// Nothing downloads before the user picks "Download" — an update is an
// offer, never an ambient side effect.
void MainWindow::StartDownload() {
  if (backend_ == nullptr || !backend_->running() || update_downloading_) {
    return;
  }
  update_downloading_ = true;
  SetStatus(true, std::wstring(podlens::Tr("update.downloading")));

  auto const dispatcher = DispatcherQueue();
  auto const weak = get_weak();
  auto backend = backend_;
  std::thread([weak, dispatcher]() mutable {
    std::string failure;
    if (weak.get()) {
      try {
        weak.get()->api_->start_download().get();
      } catch (std::exception const& e) {
        failure = e.what();
      }
    }
    if (!failure.empty()) {
      dispatcher.TryEnqueue([weak, failure = std::move(failure)] {
        if (auto window = weak.get()) {
          window->FailDownload(failure);
        }
      });
    }
  }).detach();

  // Poll update_state while the backend downloads (taskly's 400 ms timer).
  update_timer_ = DispatcherQueue().CreateTimer();
  update_timer_.Interval(std::chrono::milliseconds{400});
  update_timer_.Tick([weak, backend](auto&&, auto&&) {
    if (auto window = weak.get()) {
      window->PollUpdateState(backend);
    }
  });
  update_timer_.Start();
}

void MainWindow::PollUpdateState(
    std::shared_ptr<rivet::windows::Backend> const& backend) {
  auto const dispatcher = DispatcherQueue();
  auto const weak = get_weak();
  std::thread([weak, dispatcher, backend]() mutable {
    std::optional<rivet_app::UpdateState> state;
    try {
      rivet_app::API api(*backend);
      state = api.update_state().get();
    } catch (...) {
      return;  // a transient RPC failure is not a download failure; the
               // next tick retries
    }
    dispatcher.TryEnqueue([weak, state = std::move(state)]() mutable {
      if (auto window = weak.get()) {
        window->HandleUpdatePoll(state);
      }
    });
  }).detach();
}

void MainWindow::HandleUpdatePoll(
    std::optional<rivet_app::UpdateState> const& state) {
  if (!update_downloading_ || !state) {
    return;
  }
  if (state->phase == "downloaded") {
    StopUpdateTimer();
    update_downloading_ = false;
    auto const path = to_wide(state->downloaded_path.value_or(""));
    if (path.empty()) {
      FailDownload("downloaded file path missing");
      return;
    }
    SetStatus(true, std::wstring(podlens::Tr("update.downloaded")));
    ShowInstallConsent(path);
    return;
  }
  if (state->phase == "error") {
    StopUpdateTimer();
    update_downloading_ = false;
    FailDownload(state->message.value_or("download failed"));
    return;
  }
  wchar_t text[128];
  swprintf(text, 128, podlens::Tr("update.downloading_percent").data(),
           static_cast<int>(state->percent));
  SetStatus(true, text);
}

void MainWindow::StopUpdateTimer() {
  if (update_timer_) {
    update_timer_.Stop();
    update_timer_ = nullptr;
  }
}

void MainWindow::FailDownload(std::string const& message) {
  update_downloading_ = false;
  StopUpdateTimer();
  SetStatus(true, L"");
  ShowUpdateDialog(L"PodLens",
                   FormatUpdateMessage(L"update.install_failed",
                                       to_wide(message)),
                   L"", std::wstring(podlens::Tr("dialog.close")), nullptr);
}

// ---------- install handoff ------------------------------------------------------

// Installing is a separate, explicit step: the download is complete and
// verified (backend-side, size + SHA-256 against the signed manifest), and
// only the user's "Quit and install" closes the app.
void MainWindow::ShowInstallConsent(std::wstring const& path) {
  ShowUpdateDialog(
      std::wstring(podlens::Tr("update.downloaded")),
      std::wstring(podlens::Tr("update.ready_body")),
      std::wstring(podlens::Tr("update.quit_and_install")),
      std::wstring(podlens::Tr("dialog.cancel")),
      [weak = get_weak(), path] {
        if (auto window = weak.get()) {
          window->InstallDownloadedUpdate(path);
        }
      });
}

void MainWindow::InstallDownloadedUpdate(std::wstring const& zip_path) {
  auto const work = update_work_dir();
  if (work.empty()) {
    ShowUpdateDialog(L"PodLens",
                     FormatUpdateMessage(L"update.install_failed", L"temp"),
                     L"", std::wstring(podlens::Tr("dialog.close")), nullptr);
    return;
  }
  std::error_code ec;
  auto const script = work / L"update-install.cmd";
  // Stale markers from earlier attempts must not survive this one.
  std::filesystem::remove(update_failure_marker(), ec);
  std::filesystem::remove(update_success_marker(), ec);

  {
    std::ofstream file(script, std::ios::binary | std::ios::trunc);
    if (!file) {
      ShowUpdateDialog(
          L"PodLens",
          FormatUpdateMessage(L"update.install_failed", L"script"), L"",
          std::wstring(podlens::Tr("dialog.close")), nullptr);
      return;
    }
    auto const batch = update_handoff_batch(::GetCurrentProcessId());
    file.write(batch.data(), static_cast<std::streamsize>(batch.size()));
    file.close();
    if (!file) {
      ShowUpdateDialog(
          L"PodLens",
          FormatUpdateMessage(L"update.install_failed", L"script"), L"",
          std::wstring(podlens::Tr("dialog.close")), nullptr);
      return;
    }
  }

  std::wstring const install_dir =
      executable_path().parent_path().wstring();
  std::wstring const exe = executable_path().wstring();
  std::wstring const parameters =
      L"/c call \"" + script.wstring() + L"\" \"" + zip_path + L"\" \"" +
      install_dir + L"\" \"" + exe + L"\" \"" +
      update_failure_marker().wstring() + L"\" \"" +
      update_success_marker().wstring() + L"\"";
  SHELLEXECUTEINFOW info{};
  info.cbSize = sizeof(info);
  info.fMask = SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI;
  info.lpVerb = L"open";
  info.lpFile = L"cmd.exe";
  info.lpParameters = parameters.c_str();
  info.nShow = SW_HIDE;
  if (!ShellExecuteExW(&info)) {
    ShowUpdateDialog(
        L"PodLens",
        FormatUpdateMessage(L"update.install_failed", L"handoff"), L"",
        std::wstring(podlens::Tr("dialog.close")), nullptr);
    return;
  }
  // The handoff script takes it from here: it waits for this process to
  // exit, swaps the zip in, relaunches, and leaves markers behind either
  // way (a failure marker is HandleInstallMarkers' input on next launch).
  winrt::Microsoft::UI::Xaml::Application::Current().Exit();
}

// Surface a previous failed install once: read the error the handoff script
// left behind, clear the marker, and report. A success marker means the
// swapped-in version launched — retire the .old fallback copy.
void MainWindow::HandleInstallMarkers() {
  auto const work = update_work_dir();
  if (work.empty()) {
    return;
  }
  std::error_code ec;
  auto const success = update_success_marker();
  if (std::filesystem::exists(success, ec)) {
    std::filesystem::remove(success, ec);
    try {
      auto const install_dir = executable_path().parent_path();
      std::filesystem::remove_all(
          std::filesystem::path(install_dir.wstring() + L".old"), ec);
    } catch (...) {
      // The fallback copy is expendable; never block startup on it.
    }
    return;
  }
  auto const marker = update_failure_marker();
  if (!std::filesystem::exists(marker, ec)) {
    return;
  }
  std::ifstream file(marker);
  std::string code;
  std::getline(file, code);
  file.close();
  std::filesystem::remove(marker, ec);
  ShowUpdateDialog(
      L"PodLens",
      FormatUpdateMessage(L"update.install_failed",
                          to_wide(code.empty() ? std::string("unknown")
                                               : code)),
      L"", std::wstring(podlens::Tr("dialog.close")), nullptr);
}

// ZIP便携版语义: a copy without the packaged layout (embedded backend at
// res/core.zo) or one running from a temp path is a dev copy — auto-update
// is unavailable there.
bool MainWindow::IsDevCopy() {
  try {
    auto const install_dir = executable_path().parent_path();
    std::error_code ec;
    if (!std::filesystem::exists(install_dir / L"res" / L"core.zo", ec)) {
      return true;
    }
    return lowercase(install_dir.wstring()).find(L"temp") !=
           std::wstring::npos;
  } catch (...) {
    return true;
  }
}

// MSI 安装版语义: the MSI lands under Program Files, where the in-place
// zip swap has no write access — those installs get the manual path
// (per-user zip installs stay fully automatic).
bool MainWindow::IsMsiInstall() {
  wchar_t buffer[32768]{};
  auto const under_program_files = [&](wchar_t const* variable) {
    DWORD const length = ::GetEnvironmentVariableW(variable, buffer, 32768);
    if (length == 0 || length >= 32768) return false;
    std::wstring program_files = lowercase(std::wstring{buffer});
    while (!program_files.empty() && program_files.back() == L'\\') {
      program_files.pop_back();
    }
    if (program_files.empty()) return false;
    std::wstring const directory =
        lowercase(executable_path().parent_path().wstring());
    return directory == program_files ||
           directory.rfind(program_files + L"\\", 0) == 0;
  };
  return under_program_files(L"ProgramFiles") ||
         under_program_files(L"ProgramFiles(x86)") ||
         under_program_files(L"ProgramW6432");
}

// ---------- update dialogs -------------------------------------------------------

// One dialog helper for the whole update flow. An empty primary label hides
// the primary button (WinUI), so info dialogs pass L"". A failed ShowAsync
// (another dialog already up) is swallowed instead of stacking modals.
void MainWindow::ShowUpdateDialog(std::wstring const& title,
                                  std::wstring const& body,
                                  std::wstring const& primary_button,
                                  std::wstring const& close_button,
                                  std::function<void()> on_primary) {
  muxc::ContentDialog dialog;
  dialog.Title(winrt::box_value(winrt::hstring(title)));
  dialog.Content(winrt::box_value(winrt::hstring(body)));
  if (!primary_button.empty()) {
    dialog.PrimaryButtonText(winrt::hstring(primary_button));
    dialog.DefaultButton(mxc::ContentDialogButton::Primary);
  }
  dialog.CloseButtonText(winrt::hstring(close_button));
  dialog.XamlRoot(Content().XamlRoot());

  auto const weak = get_weak();
  auto op = dialog.ShowAsync();
  op.Completed([weak, on_primary = std::move(on_primary)](
                   winrt::Windows::Foundation::IAsyncOperation<
                       mxc::ContentDialogResult> const& sender,
                   winrt::Windows::Foundation::AsyncStatus) {
    mxc::ContentDialogResult result{mxc::ContentDialogResult::None};
    try {
      result = sender.GetResults();
    } catch (...) {
      return;  // another dialog is already up; the flow can be retried
    }
    if (result == mxc::ContentDialogResult::Primary && weak.get() &&
        on_primary) {
      on_primary();
    }
  });
}

}  // namespace winrt::RivetHost::implementation
