#pragma once

// I18n.h — tiny embedded zh/en string table for the PodLens WinUI host.
//
// v1 keeps the UI language a build-time constant (Chinese default); the
// en column is the fallback map for an English build. Tr() does a linear
// scan over a constexpr table — the table is small and only consulted
// during UI construction and status updates, so lookup cost is irrelevant.
//
// To add a string: append an Entry, then reference it via Tr("key").

#include <cstring>
#include <string_view>

namespace podlens {

enum class Lang {
  Zh,
  En,
};

// Build-time language switch. Flip to Lang::En for an English build.
inline constexpr Lang kUiLang = Lang::Zh;

struct Entry {
  char const* key;
  wchar_t const* zh;
  wchar_t const* en;
};

inline constexpr Entry kStrings[] = {
    {"app.title", L"PodLens — 播客工作台", L"PodLens — Podcast Workbench"},
    {"menu.subs", L"订阅", L"Subscriptions"},
    {"menu.help", L"帮助", L"Help"},
    {"menu.add_feed", L"添加订阅…", L"Add Feed…"},
    {"menu.refresh_all", L"刷新全部订阅", L"Refresh All Feeds"},
    {"menu.refresh_feed", L"刷新当前订阅", L"Refresh This Feed"},
    {"menu.remove_feed", L"删除当前订阅", L"Remove This Feed"},
    {"menu.settings", L"设置…", L"Settings…"},
    {"menu.check_updates", L"检查更新…", L"Check for Updates…"},
    {"menu.open_releases", L"打开发布页", L"Open Releases Page"},
    {"nav.pane_title", L"订阅列表", L"Subscriptions"},
    {"nav.add_feed", L"添加订阅", L"Add Feed"},
    {"episodes.header", L"单集", L"Episodes"},
    {"episodes.count_suffix", L" 集", L" episodes"},
    {"episodes.empty", L"还没有单集，试试菜单「订阅 → 刷新当前订阅」。",
     L"No episodes yet — try Subscriptions → Refresh This Feed."},
    {"feeds.empty", L"还没有订阅，点左侧「添加订阅」导入一个 RSS 地址。",
     L"No subscriptions yet — add a feed with the + item in the sidebar."},
    {"action.play", L"播放", L"Play"},
    {"action.pause", L"暂停", L"Pause"},
    {"action.download", L"下载", L"Download"},
    {"action.transcribe", L"转写", L"Transcribe"},
    {"action.translate", L"翻译", L"Translate"},
    {"action.summarize", L"总结", L"Summarize"},
    {"action.remove_audio", L"删除音频", L"Delete Audio"},
    {"transcript.header", L"转写", L"Transcript"},
    {"transcript.empty", L"还没有转写文本，点击「转写」生成。",
     L"No transcript yet — click Transcribe to generate one."},
    {"transcript.zh_only", L"只看中文", L"Translation only"},
    {"summary.header", L"摘要", L"Summary"},
    {"summary.tldr", L"TL;DR", L"TL;DR"},
    {"summary.points", L"要点", L"Key points"},
    {"summary.topics", L"话题", L"Topics"},
    {"summary.empty", L"还没有摘要，点击「总结」生成。",
     L"No summary yet — click Summarize to generate one."},
    {"summary.parse_error", L"摘要内容解析失败。", L"Failed to parse the summary."},
    {"badge.downloaded", L"已下载", L"Downloaded"},
    {"badge.transcript", L"已转写", L"Transcribed"},
    {"badge.transcript_running", L"转写中", L"Transcribing"},
    {"badge.transcript_error", L"转写失败", L"Transcribe failed"},
    {"badge.summary", L"已总结", L"Summarized"},
    {"badge.summary_running", L"总结中", L"Summarizing"},
    {"badge.summary_error", L"总结失败", L"Summarize failed"},
    {"badge.translated", L"有译文", L"Translated"},
    {"badge.done", L"已听完", L"Played"},
    {"detail.none", L"未选择单集", L"No episode selected"},
    {"detail.hint", L"选择左侧单集查看转写与摘要。",
     L"Pick an episode on the left to see its transcript and summary."},
    {"detail.position_prefix", L"听到 ", L"at "},
    {"status.starting", L"正在启动内嵌 Racket CS…", L"Starting embedded Racket CS…"},
    {"status.started", L"任务已开始", L"Job started"},
    {"status.done", L"完成", L"done"},
    {"status.failed", L"失败", L"failed"},
    {"status.saved", L"设置已保存", L"Settings saved"},
    {"status.feed_added", L"已添加订阅", L"Subscription added"},
    {"status.feed_removed", L"已删除订阅", L"Subscription removed"},
    {"status.refreshed", L"刷新完成，新增 ", L"Refresh complete, "},
    {"status.refreshed_suffix", L" 集", L" new episodes"},
    {"status.no_feed", L"请先选择一个订阅", L"Select a subscription first"},
    {"status.no_audio", L"本地还没有音频，开始下载…", L"No local audio yet — downloading…"},
    {"status.audio_missing", L"下载已完成，但在本地音频目录找不到文件。",
     L"Download finished but the local audio file was not found."},
    {"status.audio_removed", L"已删除本地音频", L"Local audio deleted"},
    {"status.playing", L"正在播放", L"Playing"},
    {"status.playback_done", L"播放结束，进度已保存", L"Playback finished — position saved"},
    {"status.playback_failed", L"播放失败", L"Playback failed"},
    {"status.releases_opening", L"正在打开发布页…", L"Opening the releases page…"},
    {"status.dialog_busy", L"已有对话框打开，请先关闭。", L"A dialog is already open — close it first."},
    {"dialog.add_feed_title", L"添加订阅", L"Add Feed"},
    {"dialog.url_placeholder", L"播客 RSS 地址，https://…", L"Podcast RSS URL, https://…"},
    {"dialog.primary_add", L"添加", L"Add"},
    {"dialog.cancel", L"取消", L"Cancel"},
    {"dialog.save", L"保存", L"Save"},
    {"dialog.close", L"关闭", L"Close"},
    {"dialog.remove_feed_title", L"删除订阅", L"Remove Subscription"},
    {"dialog.remove_feed_text", L"删除后该订阅与全部单集记录都会移除，确定？",
     L"Remove this subscription and all its episodes?"},
    {"dialog.remove_feed_primary", L"删除", L"Remove"},
    {"dialog.settings_title", L"设置", L"Settings"},
    {"menu.discover", L"发现", L"Discover"},
    {"discover.title", L"发现播客", L"Discover Podcasts"},
    {"discover.note", L"精选英文播客目录，点「添加」即订阅；不会自动订阅任何节目。",
     L"A curated list of English podcasts — add what you like; nothing is subscribed automatically."},
    {"discover.add", L"添加", L"Add"},
    {"discover.added", L"已订阅", L"Added"},
    {"cat.tech", L"科技", L"Tech"},
    {"cat.security", L"安全", L"Security"},
    {"cat.science", L"科学", L"Science"},
    {"cat.design", L"设计", L"Design"},
    {"cat.business", L"商业", L"Business"},
    {"cat.news", L"资讯", L"News"},
    {"status.ready", L"就绪", L"Ready"},
    {"status.job_running", L"任务已在后台开始，完成后列表会自动刷新",
     L"Job started — the list refreshes when it finishes"},
    {"status.need_download", L"本地还没有音频，已开始下载", L"No local audio yet — download started"},
    {"status.settings_hint", L"设置保存在 ~/.podlens/config.json，可在设置对话框中修改",
     L"Settings live in ~/.podlens/config.json"},
    {"detail.empty", L"这一集还没有逐句稿。点上方「转写」开始（需要先在设置里配好 API key）。",
     L"No transcript yet — press Transcribe in the command bar (needs an API key in Settings)."},
    {"dialog.add", L"添加", L"Add"},
};

// Look up a UI string in the build-time language. Unknown keys return an
// empty string rather than throwing — UI text is never worth a crash.
inline std::wstring_view Tr(char const* key) {
  for (auto const& entry : kStrings) {
    if (std::strcmp(entry.key, key) == 0) {
      return kUiLang == Lang::Zh ? std::wstring_view(entry.zh) : std::wstring_view(entry.en);
    }
  }
  return std::wstring_view(L"");
}

}  // namespace podlens
