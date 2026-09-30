#lang racket/base

;; Feed parsing, RFC 822 dates and iTunes durations.

(require rackunit
         racket/file
         (file "../app/core/feed.rkt"))

(define fixture
  #"<?xml version=\"1.0\" encoding=\"UTF-8\"?>
<rss xmlns:itunes=\"http://www.itunes.com/dtds/podcast-1.0.dtd\" version=\"2.0\">
<channel>
  <title>Darknet Diaries</title>
  <link>https://darknetdiaries.com</link>
  <description>True stories from the dark side of the Internet.</description>
  <itunes:author>Jack Rhysider</itunes:author>
  <itunes:image href=\"https://example.com/podcast.jpg\"/>
  <item>
    <title>EP 100: How to Slow Down</title>
    <guid isPermaLink=\"false\">dd-100</guid>
    <pubDate>Mon, 26 Jun 2023 07:00:00 +0000</pubDate>
    <itunes:duration>2941</itunes:duration>
    <enclosure url=\"https://feeds.example.com/dd/ep100.mp3\" length=\"46980000\" type=\"audio/mpeg\"/>
    <description><![CDATA[<p>Some <b>HTML</b> blurb.</p>]]></description>
  </item>
  <item>
    <title>EP 99: Azimuth</title>
    <guid>dd-99</guid>
    <pubDate>Tue, 13 Jun 2023 07:00:00 -0700</pubDate>
    <itunes:duration>1:02:30</itunes:duration>
    <enclosure url=\"https://feeds.example.com/dd/ep99.mp3\" length=\"60000000\" type=\"audio/mpeg\"/>
  </item>
</channel>
</rss>")

(test-case "rfc822 dates"
  (check-equal? (rfc822->epoch "Mon, 26 Jun 2023 07:00:00 +0000") 1687762800)
  (check-equal? (rfc822->epoch "Tue, 13 Jun 2023 07:00:00 -0700") 1686639600)
  (check-false (rfc822->epoch "not a date")))

(test-case "itunes durations"
  (check-equal? (itunes-duration->seconds "2941") 2941)
  (check-equal? (itunes-duration->seconds "1:02:30") 3750)
  (check-equal? (itunes-duration->seconds "04:30") 270)
  (check-false (itunes-duration->seconds "nonsense")))

(test-case "parse feed"
  (define f (parse-feed fixture))
  (check-equal? (hash-ref f 'title) "Darknet Diaries")
  (check-equal? (hash-ref f 'author) "Jack Rhysider")
  (check-equal? (hash-ref f 'artwork-url) "https://example.com/podcast.jpg")
  (define items (hash-ref f 'items))
  (check-equal? (length items) 2)
  (check-equal? (hash-ref (car items) 'title) "EP 100: How to Slow Down")
  (check-equal? (hash-ref (car items) 'enclosure-url) "https://feeds.example.com/dd/ep100.mp3")
  (check-equal? (hash-ref (car items) 'enclosure-length) 46980000)
  (check-equal? (hash-ref (car items) 'duration-sec) 2941)
  (check-equal? (hash-ref (car items) 'pub-date-epoch) 1687762800)
  (check-equal? (hash-ref (car items) 'description) "Some HTML blurb."))
