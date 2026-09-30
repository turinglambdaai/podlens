#lang racket/base

;; Podcast feed (RSS 2.0 + iTunes tags) fetching and parsing.
;; Everything the library stores comes out of here: title, author, artwork,
;; and one record per episode (guid, enclosure, publication date, duration).

(require xml
         racket/match
         racket/date
         racket/file
         racket/format
         racket/port
         racket/string
         (only-in xml cdata? cdata-string)
         "http.rkt")

(provide fetch-feed
         parse-feed
         rfc822->epoch
         itunes-duration->seconds)

;; ---- xexpr navigation helpers ----------------------------------------------

;; xml->xexpr emits (tag children...) for attribute-less elements; normalize
;; every element to (tag attrs children...) so navigation is uniform.
(define (normalize-node v)
  (cond
    [(not (pair? v)) v]
    [(symbol? (car v))
     (let* ([tag (car v)]
            [rest (cdr v)]
            [has-attrs (and (pair? rest) (list? (car rest)) (not (string? (car rest))))]
            [attrs (if has-attrs (car rest) '())]
            [children (if has-attrs (cdr rest) rest)])
       (list tag
             attrs
             (map normalize-node (if (list? children) children '()))))]
    [(list? v) (map normalize-node v)]
    [else v]))

(define (element? v)
  (and (pair? v) (symbol? (car v)) (list? (cadr v)) (list? (caddr v))))

(define (element-tag e) (car e))
(define (element-attrs e) (cadr e))
(define (element-children e) (caddr e))

(define (tag-matches? e wanted)
  (and (element? e)
       (let ([t (symbol->string (element-tag e))])
         (string-ci=? (regexp-replace* #rx"^[^:]*:" t "") wanted))))

;; All descendant+self elements whose local name (namespace prefix stripped,
;; case-insensitive) equals wanted, in document order.
(define (find-all wanted x)
  (let walk ([node x] [acc '()])
    (cond
      [(element? node)
       (let* ([acc (if (tag-matches? node wanted) (append acc (list node)) acc)])
         (for/fold ([acc acc])
                   ([child (in-list (element-children node))])
           (walk child acc)))]
      [(list? node) (for/fold ([acc acc]) ([child (in-list node)]) (walk child acc))]
      [else acc])))

(define (find-first wanted x)
  (match (find-all wanted x)
    [(cons e _) e]
    [_ #f]))

;; Concatenated text of an element's direct string/CDATA children.
;; CDATA survives xml->xexpr as a struct, not a plain xexpr node.
(define (element-text e)
  (if (element? e)
      (string-trim
       (string-append*
        (for/list ([c (in-list (element-children e))])
          (cond
            [(string? c) c]
            [(cdata? c)
             (regexp-replace* #rx"^<!\\[CDATA\\[|\\]\\]>$" (cdata-string c) "")]
            [else ""]))))
      ""))

(define (attr e name)
  (define hit (assq (string->symbol name) (element-attrs e)))
  (and hit
       (let ([v (cdr hit)])
         (cond
           [(string? v) v]
           [(and (list? v) (= 1 (length v)) (string? (car v))) (car v)]
           [else #f]))))

;; ---- dates & durations -------------------------------------------------------

(define months
  '(("jan" . 1) ("feb" . 2) ("mar" . 3) ("apr" . 4) ("may" . 5) ("jun" . 6)
    ("jul" . 7) ("aug" . 8) ("sep" . 9) ("oct" . 10) ("nov" . 11) ("dec" . 12)))

;; RFC 822-ish dates as used by feeds:
;;   "Mon, 15 Jul 2026 09:30:00 +0000" / "15 Jul 2026 09:30:00 GMT"
;; Returns epoch seconds in UTC, or #f when unparseable.
;;
;; find-seconds with date? #f interprets the wall clock as UTC regardless of
;; the machine's time zone (verified: same answer on a +08:00 host); the
;; numeric offset is then applied explicitly. The offset regexp must be #px —
;; #rx does not support {4}, and that branch silently never fired before,
;; which made every non-zero offset (and the old test asserting it) wrong.
(define (rfc822->epoch s)
  (with-handlers ([exn:fail? (lambda (_) #f)])
    (define m
      (regexp-match
       #px"([0-9]{1,2})[ -]([A-Za-z]{3,})[ -]([0-9]{2,4})[ T]+([0-9]{1,2}):([0-9]{2})(?::([0-9]{2}))?\\s*([+-][0-9]{4}|[A-Za-z]+)?"
       (string-trim s)))
    (and m
         (let* ([day (string->number (list-ref m 1))]
                [mon (cdr (assoc (string-downcase (substring (list-ref m 2) 0 3)) months))]
                [year* (string->number (list-ref m 3))]
                [year (if (< year* 100) (+ 2000 year*) year*)]
                [hh (string->number (list-ref m 4))]
                [mm (string->number (list-ref m 5))]
                [ss (or (and (list-ref m 6) (string->number (list-ref m 6))) 0)]
                [tz (list-ref m 7)]
                [offset
                 (cond
                   [(not tz) 0]
                   [(regexp-match #px"^[+-][0-9]{4}$" tz)
                    (define sign (if (equal? (substring tz 0 1) "+") 1 -1))
                    (* sign (+ (* (string->number (substring tz 1 3)) 3600)
                               (* (string->number (substring tz 3 5)) 60)))]
                   [else 0])] ; GMT/UT/Z/unknown → UTC
                [wall-as-utc (find-seconds ss mm hh day mon year #f)])
           ;; "+hh:mm" means the wall clock runs ahead of UTC
           (- wall-as-utc offset)))))

;; "1:02:03" / "04:30" / "3725" → seconds
(define (itunes-duration->seconds s)
  (define parts (string-split (string-trim s) ":"))
  (cond
    [(null? parts) #f]
    [(and (= 1 (length parts)) (string->number (string-trim s)))
     (string->number (string-trim s))]
    [else
     (let loop ([xs (reverse parts)] [mult 1] [acc 0])
       (if (null? xs)
           (and (> mult 1) acc)
           (let ([n (string->number (string-trim (car xs)))])
             (if n
                 (loop (cdr xs) (* mult 60) (+ acc (* n mult)))
                 #f))))]))

;; ---- feed parsing ----------------------------------------------------------

;; → hash with title, author, description, link, artwork-url, items
;; items: list of hashes with guid, title, pub-date-epoch, pub-date-display,
;; enclosure-url, enclosure-length, enclosure-type, duration-sec, description
(define (parse-feed xml-bytes)
  ;; Real-world feeds (NPR especially) ship bare ampersands ("Barnes & Noble")
  ;; that Racket's strict XML lexer rejects. Repair them before parsing:
  ;; escape every & not already opening a numeric or named entity.
  (define repaired
    (regexp-replace* #px"&(?![a-zA-Z#][a-zA-Z0-9]{1,10};)" xml-bytes #"&amp;"))
  (define doc
    (normalize-node
     (with-input-from-bytes repaired
       (lambda () (xml->xexpr (document-element (read-xml)))))))
  (define channel (or (find-first "channel" doc) doc))
  ;; itunes:image / <image> both carry local name "image"; prefer one with
  ;; an href attribute (the iTunes form).
  (define image-el (find-first "image" channel))
  (define image-href (and image-el (attr image-el "href")))
  (define image-plain
    (and image-el (element-text (or (find-first "url" image-el) image-el))))
  (define (item-record item)
    (define enc (find-first "enclosure" item))
    (define dur-raw (let ([d (find-first "duration" item)]) (and d (element-text d))))
    (define pub-raw (element-text (or (find-first "pubDate" item) '(pubDate () ""))))
    (define epoch (and (non-empty-string? pub-raw) (rfc822->epoch pub-raw)))
    (hasheq 'guid (element-text (or (find-first "guid" item) '(guid () "")))
            'title (element-text (or (find-first "title" item) '(title () "")))
            'pub-date-epoch (or epoch 0)
            'pub-date-display pub-raw
            'enclosure-url (or (and enc (attr enc "url")) "")
            'enclosure-length
            (let ([l (and enc (attr enc "length"))]) (and l (string->number l)))
            'enclosure-type (or (and enc (attr enc "type")) "")
            'duration-sec (and dur-raw (itunes-duration->seconds dur-raw))
            'description
            (let ([d (find-first "description" item)])
              (if d (regexp-replace* #rx"<[^>]*>" (element-text d) "") ""))))
  (hasheq 'title (element-text (or (find-first "title" channel) '(title () "")))
          'author
          (element-text (or (find-first "author" channel) '(author () "")))
          'description
          (let ([d (find-first "description" channel)])
            (if d (regexp-replace* #rx"<[^>]*>" (element-text d) "") ""))
          'link (element-text (or (find-first "link" channel) '(link () "")))
          'artwork-url (or image-href image-plain "")
          'items (map item-record (find-all "item" channel))))

;; Fetch and parse. Raises exn:fail on network errors or non-XML payloads.
(define (fetch-feed url)
  (define-values (code _headers body) (http-get-bytes url))
  (unless (= code 200)
    (error 'fetch-feed "feed request failed (~a): ~a" code url))
  (with-handlers ([exn:fail? (lambda (e)
                               (error 'fetch-feed "not a valid RSS feed: ~a" url))])
    (parse-feed body)))
