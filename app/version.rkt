#lang racket/base

;; Single source of the application version for the backend State, the CLI
;; output and update checks. The root VERSION file and rivet.rktd must carry
;; the same version; scripts/check-release-version.sh enforces the alignment
;; on every release (until Rivet exposes the deployment identity to the
;; backend, upstream turinglambdaai/rivet).

(provide app-version)

(define app-version "1.5.0")
