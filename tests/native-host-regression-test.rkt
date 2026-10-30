#lang racket

(require rackunit
         racket/runtime-path
         racket/string)

(define-runtime-path macos-lang-source
  "../macos-host/Sources/RivetHost/Lang.swift")
(define-runtime-path windows-host-source
  "../windows/MainWindow.xaml.cpp")

(define macos-lang (file->string macos-lang-source))
(define windows-host (file->string windows-host-source))

(test-case "macOS integer localization formats match Swift arguments"
  ;; Passing a Swift Int to Foundation's %@ object placeholder aborts while
  ;; SwiftUI evaluates the episode header. These formats are consumed with
  ;; String(format:..., Int), so they must remain integer placeholders.
  (check-regexp-match #rx"pipelineCostKnown[^\n]+%d[^\n]+%d characters" macos-lang)
  (check-false
   (regexp-match? #rx"(pipelineCostKnown|estimateChars)[^\n]+%@" macos-lang))
  ;; The minimal-detail refactor removed the header estimate line entirely;
  ;; its keys must stay gone so dev-ese numbers cannot creep back in.
  (check-false (regexp-match? #rx"\"estimateChars\"" macos-lang))
  (check-false (regexp-match? #rx"\"estimateSentences\"" macos-lang)))

(test-case "Windows data-directory lookup keeps getenv_s output length"
  ;; Querying the required size and reading the value use separate variables.
  ;; Reusing `required` left `written` at zero and silently discarded the
  ;; acceptance/user override, so downloaded audio looked missing.
  (check-true
   (string-contains?
    windows-host
    "::getenv_s(&written, value.data(), value.size(), \"PODLENS_DATA_DIR\")"))
  (check-false
   (string-contains?
    windows-host
    "::getenv_s(&required, value.data(), value.size(), \"PODLENS_DATA_DIR\")")))
