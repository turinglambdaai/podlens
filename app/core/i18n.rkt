#lang racket/base

;; Backend-facing strings (CLI output, notify events, progress lines).
;; The native hosts read shared/i18n/{zh,en}.json for UI labels; this table
;; only covers what the Racket side says.

(require racket/string)

(provide tr)

(define table
  (hasheq
   ;; CLI
   'added '("已添加订阅" . "Subscription added")
   'refreshed '("刷新完成，新增 {0} 集" . "Refreshed, {0} new episodes")
   'no-feeds '("还没有订阅，先用 add <RSS 地址> 添加" . "No subscriptions yet — add one with add <RSS-URL>")
   'no-episode '("找不到该集数" . "No such episode")
   'no-feed '("找不到该订阅" . "No such feed")
   'ambiguous-id '("ID 前缀不唯一：{0}" . "Ambiguous id prefix: {0}")
   'no-results '("没有匹配的播客" . "No matching podcasts")
   'downloaded '("下载完成" . "Download complete")
   'transcribed '("转写完成，共 {0} 段" . "Transcribed, {0} segments")
   'translated '("翻译完成，共 {0} 段" . "Translated, {0} segments")
   'summarized '("总结完成" . "Summary ready")
   'pipelined '("听懂完成：逐句稿、翻译、总结已就绪" . "Episode understood — transcript, translation and summary are ready")
   'marked-done '("已标记为已听" . "Marked as played")
   'marked-undone '("已标记为未听" . "Marked as unplayed")
   'nothing-to-resume '("还没有可恢复的播放记录" . "Nothing to resume yet")
   'exported '("已导出到 {0}" . "Exported to {0}")
   'export-no-transcript '("这一集还没有逐句稿，先转写再导出" . "No transcript yet — transcribe before exporting")
   'found-n '("共 {0} 处匹配" . "{0} matches")
   'need-transcribe '("还没有转写，先执行 transcribe" . "No transcript yet — run transcribe first")
   'position-saved '("播放进度已保存" . "Position saved")
   'config-set '("已保存 {0} = {1}" . "Saved {0} = {1}")
   'config-reset '("已恢复默认 {0} = {1}" . "Reset {0} = {1}")
   'unknown-key '("未知配置项 {0}" . "Unknown setting {0}")
   'up-to-date '("已是最新版本" . "PodLens is up to date")
   'update-available '("发现新版本 {0}" . "Update available: {0}")
   'update-failed '("更新检查失败：{0}" . "Update check failed: {0}")
   'update-dev '("开发构建，未配置更新公钥" . "Developer build — no update key configured")
   'usage '("用法见 help" . "See help for usage")
   ;; doctor
   'doctor-data '("数据目录可写" . "data directory writable")
   'doctor-api '("API 已配置" . "API configured")
   'doctor-no-api '("未配置 API key（Settings 里设置）" . "API key not configured (set it in Settings)")
   'doctor-ffmpeg '("ffmpeg 可用，长音频可自动分段" . "ffmpeg found — long audio can be split")
   'doctor-no-ffmpeg '("未安装 ffmpeg，超过 24 MiB 的单集无法转写" . "ffmpeg missing — episodes over 24 MiB cannot be transcribed")))

;; {0} {1} placeholders
(define (tr lang key . args)
  (define pair (hash-ref table key (cons key key)))
  (define template (if (equal? lang "en") (cdr pair) (car pair)))
  (for/fold ([s template]) ([a (in-list args)] [i (in-naturals)])
    (string-replace s (format "{~a}" i) (format "~a" a))))
