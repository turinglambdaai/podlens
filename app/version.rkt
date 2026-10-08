#lang racket/base

;; Single source of the application version for the backend State, the CLI
;; output and update checks. rivet.rktd is the release-identity truth source
;; and must carry the same version; keep the two in sync until Rivet exposes
;; the deployment identity to the backend (upstream turinglambdaai/rivet).

(provide app-version)

(define app-version "1.3.0")
