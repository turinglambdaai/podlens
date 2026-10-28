import AVFoundation
import MediaPlayer
import SwiftUI
import UserNotifications

/// What the content column shows; the sidebar List selection drives it.
enum SidebarItem: Hashable {
    case home
    case discover
    case shows
    case recent
    case downloaded
    case feed(Feed)
}

/// An episode across feeds — the shape home/library lanes consume
/// (episode-recent RPC row: episode columns prefixed with feed context).
struct RecentEpisode: Identifiable, Hashable {
    let id: String
    let feedId: String
    let feedTitle: String
    let artworkURL: String
    let title: String
    let pubDisplay: String
    let durationSec: String
    let downloaded: Bool
    let transcriptStatus: String
    let summaryStatus: String
    let positionSec: Double
    let done: Bool
    let hasTranslation: Bool
    let description: String

    /// Seconds parsed from the wire's integer-string duration ("" → 0).
    var duration: Double { Double(durationSec) ?? 0 }
}

/// All client state lives here; views stay dumb. Every backend round trip
/// goes through the generated RivetAPI; backend events arrive via
/// handleEvent and update job progress live.
@MainActor
final class PodLensStore: ObservableObject {
    struct Feed: Identifiable, Hashable {
        let id: String
        let title: String
        let author: String
        let artworkURL: String
        let episodeCount: Int
        let latestTitle: String
        let latestPub: String
        let unplayed: Int
    }

    struct Episode: Identifiable, Hashable {
        let id: String
        let title: String
        let pubDisplay: String
        let durationSec: String
        let downloaded: Bool
        let transcriptStatus: String
        let summaryStatus: String
        let positionSec: Double
        let done: Bool
        let hasTranslation: Bool
        let description: String
    }

    struct Chapter: Identifiable, Hashable {
        let id: Int
        let start: Double
        let title: String
    }

    /// What a "understand this episode" run would cost, for the header hint.
    struct Estimate {
        let durationSec: Double?
        let transcriptKnown: Bool
        let sentences: Int?
        let chars: Int?
        let translated: Bool
        let summarized: Bool
    }

    struct Segment: Identifiable, Hashable {
        let id: Int
        let start: Double
        let end: Double
        let text: String
        let translation: String
    }

    struct Summary {
        let tldr: String
        let keyPoints: [String]
        let quotes: [(String, String)]
        let topics: [String]
    }

    struct Job: Identifiable {
        let id: String
        let kind: String
        var pct: Int
        var message: String
        var status: String
    }

    struct CatalogEntry: Identifiable, Hashable {
        let id: String
        let category: String
        let title: String
        let description: String
        let url: String
        let homepage: String
        let added: Bool
        let artworkURL: String
    }

    /// A feed episode playable without subscribing — the Discover page's
    /// one-click audition (feed-peek row; the enclosure URL is the id).
    struct PreviewEpisode: Identifiable, Hashable {
        let enclosureURL: String
        let feedTitle: String
        let author: String
        let artworkURL: String
        let title: String
        let pubDisplay: String
        let durationSec: String

        var id: String { enclosureURL }
    }

    @Published var feeds: [Feed] = []
    @Published var episodes: [Episode] = []
    @Published var catalog: [CatalogEntry] = []
    /// The content-column destination driven by the sidebar selection.
    @Published var sidebarItem: SidebarItem = .home
    @Published var recentEpisodes: [RecentEpisode] = []
    /// Transcript/summary tab of the detail column; the player bar's
    /// transcript button jumps here.
    @Published var detailTab = 0

    @Published var searchResults: [CatalogEntry] = []
    @Published var isSearching = false
    @Published var searchError: String?
    @Published var previewEpisode: PreviewEpisode?
    @Published var previewLoading = false
    @Published var selectedFeed: Feed?
    @Published var selectedEpisode: Episode?
    @Published var segments: [Segment] = []
    @Published var summary: Summary?
    @Published var chapters: [Chapter] = []
    @Published var estimate: Estimate?
    @Published var jobs: [String: Job] = [:]
    @Published var statusLine = ""
    @Published var bootStatus = ""
    @Published var translationMode = 0 // 0 bilingual, 1 zh only, 2 en only
    @Published var updateOutcome: Result<UpdateService.CheckResult, Error>?
    @Published var playing = false
    @Published var currentTime: Double = 0
    @Published var duration: Double = 0
    @Published var rate: Float = 1.0
    @Published private(set) var sleepMinutes: Int?

    /// Rates offered in the player bar; the remote-command handler accepts
    /// the same set (Apple Podcasts tops out at 3×).
    static let playbackRates: [Float] = [1.0, 1.25, 1.5, 1.75, 2.0, 2.5, 3.0]

    private var api: RivetAPI?
    var apiForSettings: RivetAPI? { api }
    private var didAutoOpenDiscover = false
    private var player: AVPlayer?
    private var timeObserver: Any?
    private var endObserver: NSObjectProtocol?
    private var saveTicks = 0
    private var lastFollowedSegment: Int?
    private var sleepTimer: Timer?
    private var sleepFireAt: Date?
    // now-playing metadata (owned here, rendered by SystemMedia.swift)
    var npTitle = ""
    var npShow = ""
    var npArtwork: MPMediaItemArtwork?

    // MARK: boot

    func bind(api: RivetAPI) {
        self.api = api
        setupSystemMediaControls()
        scheduleAutoRefresh()
    }

    func handleEvent(_ event: RivetEvent) {
        switch event {
        case .notify(let message):
            statusLine = message
        case .job_progress(let fields):
            guard fields.count >= 4, var job = jobs[fields[0]] else { return }
            job.pct = Int(fields[2]) ?? job.pct
            job.message = fields[3]
            jobs[fields[0]] = job
        case .episodes_changed(let feedId):
            if selectedFeed?.id == feedId {
                loadEpisodes(feedId: feedId)
            }
            Task { await loadFeeds() }
            // drop finished jobs; the dictionary only needs running state
            jobs = jobs.filter { $0.value.status == "running" }
        case .open_url(let url):
            if let u = URL(string: url) { NSWorkspace.shared.open(u) }
        case .update_available:
            break // surfaced through updateOutcome
        }
    }

    // MARK: feeds & episodes

    func loadCatalog() async {
        guard let api else { return }
        do {
            let rows = try await api.catalog_list()
            catalog = rows.map {
                CatalogEntry(id: "\($0[0])|\($0[1])", category: $0[1], title: $0[2],
                             description: $0[3], url: $0[4], homepage: $0[5], added: $0[6] == "1",
                             artworkURL: $0.count > 7 ? $0[7] : "")
            }
        } catch {
            statusLine = L("fetchFailed") + " \(error)"
        }
    }

    func addFromCatalog(_ entry: CatalogEntry) {
        addFeed(url: entry.url)
    }

    /// Search the full podcast directory (iTunes Search API through the
    /// backend). Results reuse CatalogEntry so the UI renders one row type.
    func searchDirectory(_ query: String) {
        let q = query.trimmingCharacters(in: .whitespaces)
        guard api != nil, !q.isEmpty else { return }
        isSearching = true
        searchError = nil
        Task { await searchDirectoryRun(q) }
    }

    private func searchDirectoryRun(_ q: String) async {
        guard let api else { return }
        do {
            let rows = try await api.catalog_search(query: q)
            searchResults = rows.map {
                CatalogEntry(id: "\($0[0])|\($0[1])", category: $0[1], title: $0[2],
                             description: $0[3], url: $0[4], homepage: $0[5], added: $0[6] == "1",
                             artworkURL: $0.count > 7 ? $0[7] : "")
            }
            isSearching = false
        } catch {
            searchError = "\(error)"
            isSearching = false
        }
    }

    /// One-click audition from the Discover page: peek the feed without
    /// subscribing, cache the newest episode's audio and play it locally.
    /// No library state changes; picking any library episode ends the
    /// preview.
    func previewFromCatalog(_ entry: CatalogEntry) {
        guard let api, !previewLoading else { return }
        previewLoading = true
        statusLine = L("previewFetching")
        Task {
            do {
                let rows = try await api.feed_peek(url: entry.url)
                guard let row = rows.first, row.count >= 7 else {
                    await MainActor.run { self.previewLoading = false }
                    return
                }
                let enclosure = row[3]
                guard !enclosure.trimmingCharacters(in: .whitespaces).isEmpty else {
                    await MainActor.run {
                        self.previewLoading = false
                        self.statusLine = String(format: L("previewFailed"), L("previewNoEnclosure"))
                    }
                    return
                }
                let preview = PreviewEpisode(enclosureURL: enclosure, feedTitle: row[0],
                                             author: row[1], artworkURL: row[2],
                                             title: row[4], pubDisplay: row[5],
                                             durationSec: row[6])
                await MainActor.run {
                    self.previewEpisode = preview
                    self.statusLine = L("previewDownloading")
                }
                let path = try await api.preview_audio(enclosure_url: enclosure)
                await MainActor.run {
                    self.previewLoading = false
                    self.startPreviewPlayback(URL(fileURLWithPath: path), preview: preview)
                }
            } catch {
                await MainActor.run {
                    self.previewLoading = false
                    self.statusLine = String(format: L("previewFailed"), "\(error)")
                }
            }
        }
    }

    /// Preview playback: the audio file is already local (preview-audio),
    /// there is no episode id, so transcript loading and position saving
    /// all stand down until a library episode is picked again.
    private func startPreviewPlayback(_ url: URL, preview: PreviewEpisode) {
        selectedEpisode = nil
        segments = []
        summary = nil
        chapters = []
        estimate = nil
        lastFollowedSegment = nil

        let item = AVPlayerItem(url: url)
        let player = player ?? AVPlayer()
        installPlaybackObservers(player)
        player.replaceCurrentItem(with: item)
        self.player = player
        player.playImmediately(atRate: rate)
        playing = true
        saveTicks = 0

        npTitle = preview.title
        npShow = preview.feedTitle + " · " + L("preview")
        npArtwork = nil
        updateNowPlayingInfo()
        loadNowPlayingArtwork(preview.artworkURL)
        statusLine = L("previewPlaying") + preview.title
    }

    func loadFeeds() async {
        guard let api else { return }
        do {
            let rows = try await api.feed_list()
            feeds = rows.map {
                Feed(id: $0[0], title: $0[1], author: $0[2], artworkURL: $0[3],
                     episodeCount: Int($0[4]) ?? 0, latestTitle: $0[5], latestPub: $0[6],
                     unplayed: Int($0[7]) ?? 0)
            }
            await loadRecentEpisodes()
            // First launch with an empty library lands on Discover instead
            // of a blank window — once; a user who leaves is not nagged.
            if feeds.isEmpty && !didAutoOpenDiscover {
                didAutoOpenDiscover = true
                sidebarItem = .discover
            }
        } catch {
            statusLine = L("fetchFailed") + " \(error)"
        }
    }

    // MARK: cross-feed lanes (home / library pages)

    func loadRecentEpisodes() async {
        guard let api else { return }
        do {
            let rows = try await api.episode_recent()
            recentEpisodes = rows.map {
                // feed context prefix, then the episode-list columns
                RecentEpisode(id: $0[3], feedId: $0[0], feedTitle: $0[1], artworkURL: $0[2],
                              title: $0[4], pubDisplay: $0[5], durationSec: $0[6],
                              downloaded: $0[7] == "1", transcriptStatus: $0[8],
                              summaryStatus: $0[9], positionSec: Double($0[10]) ?? 0,
                              done: $0[11] == "1", hasTranslation: $0[12] == "1",
                              description: $0[13])
            }
        } catch {
            statusLine = L("fetchFailed") + " \(error)"
        }
    }

    /// The 待播 lane: episodes in progress first, then each feed's newest
    /// unplayed episode — what Apple Podcasts means by Up Next for a
    /// library without an explicit queue.
    var upNext: [RecentEpisode] {
        var seenFeeds = Set<String>()
        var lane: [RecentEpisode] = []
        for e in recentEpisodes where !e.done {
            let inProgress = e.positionSec > 5
            if inProgress {
                lane.append(e)
                seenFeeds.insert(e.feedId)
            }
        }
        for e in recentEpisodes where !e.done {
            if !seenFeeds.contains(e.feedId) {
                lane.append(e)
                seenFeeds.insert(e.feedId)
            }
        }
        return Array(lane.prefix(12))
    }

    /// Feeds with listening momentum first — the home shows grid.
    var homeShows: [Feed] {
        feeds.sorted { $0.unplayed > $1.unplayed }
    }

    var downloadedEpisodes: [RecentEpisode] {
        recentEpisodes.filter { $0.downloaded }
    }

    /// Select an episode from a cross-feed lane without navigating away:
    /// the owning feed becomes the playback context (autoplay chain, show
    /// title) while the sidebar stays on the page the user is reading.
    func selectRecent(_ item: RecentEpisode, autoplay: Bool = true) {
        let episode = Episode(id: item.id, title: item.title, pubDisplay: item.pubDisplay,
                              durationSec: item.durationSec, downloaded: item.downloaded,
                              transcriptStatus: item.transcriptStatus,
                              summaryStatus: item.summaryStatus,
                              positionSec: item.positionSec, done: item.done,
                              hasTranslation: item.hasTranslation, description: item.description)
        if let feed = feeds.first(where: { $0.id == item.feedId }) {
            selectedFeed = feed
            loadEpisodes(feedId: feed.id)
        }
        selectedEpisode = episode
        loadTranscript(episode)
        loadSummary(episode)
        loadChapters(episode)
        loadEstimate(episode)
        startPlayback(for: episode, autoplay: autoplay)
    }

    func selectFeed(_ feed: Feed) {
        selectedFeed = feed
        loadEpisodes(feedId: feed.id)
    }

    func loadEpisodes(feedId: String) {
        Task { await loadEpisodesNow(feedId: feedId) }
    }

    private func loadEpisodesNow(feedId: String) async {
        guard let api else { return }
        do {
            let rows = try await api.episode_list(feed_id: feedId)
            episodes = rows.map {
                Episode(id: $0[0], title: $0[1], pubDisplay: $0[2], durationSec: $0[3],
                        downloaded: $0[4] == "1", transcriptStatus: $0[5],
                        summaryStatus: $0[6], positionSec: Double($0[7]) ?? 0,
                        done: $0[8] == "1", hasTranslation: $0[9] == "1",
                        description: $0[10])
            }
        } catch {
            statusLine = L("fetchFailed") + " \(error)"
        }
    }

    func addFeed(url: String) {
        guard let api, !url.trimmingCharacters(in: .whitespaces).isEmpty else { return }
        let trimmed = url.trimmingCharacters(in: .whitespaces)
        Task {
            do {
                _ = try await api.feed_add(url: url)
                await MainActor.run {
                    // reflect subscription in the catalog/search rows
                    self.catalog = self.catalog.map { entry in
                        entry.url == trimmed ? CatalogEntry(id: entry.id, category: entry.category,
                                                            title: entry.title, description: entry.description,
                                                            url: entry.url, homepage: entry.homepage, added: true,
                                                            artworkURL: entry.artworkURL) : entry
                    }
                    self.searchResults = self.searchResults.map { entry in
                        entry.url == trimmed ? CatalogEntry(id: entry.id, category: entry.category,
                                                            title: entry.title, description: entry.description,
                                                            url: entry.url, homepage: entry.homepage, added: true,
                                                            artworkURL: entry.artworkURL) : entry
                    }
                }
                await loadFeeds()
            } catch {
                statusLine = L("fetchFailed") + " \(error)"
            }
        }
    }

    func removeFeed(_ feed: Feed) {
        guard let api else { return }
        Task {
            _ = try? await api.feed_remove(id: feed.id)
            if selectedFeed == feed { selectedFeed = nil; episodes = [] }
            if case .feed(let f) = sidebarItem, f == feed { sidebarItem = .shows }
            await loadFeeds()
        }
    }

    func refreshAll() {
        guard let api else { return }
        Task {
            do {
                let n = try await api.feed_refresh_all()
                statusLine = String(format: L("refreshAllDone"), n)
                await loadFeeds()
            } catch {
                statusLine = L("fetchFailed") + " \(error)"
            }
        }
    }

    // MARK: episode actions

    func selectEpisode(_ episode: Episode) {
        selectedEpisode = episode
        loadTranscript(episode)
        loadSummary(episode)
        loadChapters(episode)
        loadEstimate(episode)
        startPlayback(for: episode)
    }

    func withEpisode(_ id: String, _ body: @escaping (RivetAPI) async throws -> Void) {
        guard let api else { return }
        Task {
            do { try await body(api) }
            catch { statusLine = L("fetchFailed") + " \(error)" }
        }
    }

    func startJob(_ kind: String, episode: Episode) {
        withEpisode(episode.id) { [weak self] api in
            guard let self else { return }
            let jobId: String
            switch kind {
            case "download": jobId = try await api.episode_download(id: episode.id)
            case "transcribe": jobId = try await api.episode_transcribe(id: episode.id)
            case "translate": jobId = try await api.episode_translate(id: episode.id)
            case "pipeline": jobId = try await api.episode_pipeline(id: episode.id)
            default: jobId = try await api.episode_summarize(id: episode.id)
            }
            await MainActor.run {
                self.jobs[jobId] = Job(id: jobId, kind: kind, pct: 0, message: "", status: "running")
            }
            // completion watch: poll until the job leaves "running"
            while true {
                try? await Task.sleep(nanoseconds: 800_000_000)
                let statusJSON = (try? await api.job_status(job_id: jobId)) ?? ""
                guard let data = statusJSON.data(using: .utf8),
                      let obj = try? JSONSerialization.jsonObject(with: data) as? [String: Any],
                      let st = obj["status"] as? String else { continue }
                if st != "running" { break }
                await MainActor.run {
                    self.jobs[jobId]?.status = st
                    self.jobs[jobId]?.pct = obj["pct"] as? Int ?? 0
                }
            }
            await MainActor.run {
                if let feedId = self.selectedFeed?.id { self.loadEpisodes(feedId: feedId) }
                if self.selectedEpisode?.id == episode.id {
                    if kind == "transcribe" || kind == "translate" {
                        self.loadTranscript(episode)
                    }
                    if kind == "summarize" { self.loadSummary(episode) }
                }
            }
        }
    }

    func loadTranscript(_ episode: Episode) {
        Task { await loadTranscriptNow(episode) }
    }

    private func loadTranscriptNow(_ episode: Episode) async {
        guard let api else { return }
        do {
            let rows = try await api.episode_transcript(id: episode.id)
            let segs = rows.enumerated().map { i, row in
                Segment(id: i, start: Double(row[0]) ?? 0, end: Double(row[1]) ?? 0,
                        text: row[2], translation: row[3])
            }
            segments = segs
        } catch {
            statusLine = L("fetchFailed") + " \(error)"
        }
    }

    func loadSummary(_ episode: Episode) {
        Task { await loadSummaryNow(episode) }
    }

    private func loadSummaryNow(_ episode: Episode) async {
        guard let api else { return }
        do {
            let json = try await api.episode_summary(id: episode.id)
            var parsed: Summary?
            if let data = json.data(using: .utf8),
               let obj = try? JSONSerialization.jsonObject(with: data) as? [String: Any] {
                let quotes = ((obj["quotes"] as? [[String: Any]]) ?? []).map {
                    (($0["text"] as? String) ?? "", ($0["translation"] as? String) ?? "")
                }
                parsed = Summary(
                    tldr: obj["tldr"] as? String ?? "",
                    keyPoints: obj["key-points"] as? [String] ?? [],
                    quotes: quotes,
                    topics: obj["topics"] as? [String] ?? [])
            }
            summary = parsed
        } catch {
            statusLine = L("fetchFailed") + " \(error)"
        }
    }

    func loadChapters(_ episode: Episode) {
        Task { await loadChaptersNow(episode) }
    }

    private func loadChaptersNow(_ episode: Episode) async {
        guard let api else { return }
        do {
            let rows = try await api.episode_chapters(id: episode.id)
            let marks = rows.enumerated().map { i, row in
                Chapter(id: i, start: Double(row[0]) ?? 0, title: row[1])
            }
            chapters = marks
        } catch {
            statusLine = L("fetchFailed") + " \(error)"
        }
    }

    func loadEstimate(_ episode: Episode) {
        Task { await loadEstimateNow(episode) }
    }

    private func loadEstimateNow(_ episode: Episode) async {
        guard let api else { return }
        do {
            let json = try await api.episode_estimate(id: episode.id)
            var parsed: Estimate?
            if let data = json.data(using: .utf8),
               let obj = try? JSONSerialization.jsonObject(with: data) as? [String: Any] {
                parsed = Estimate(
                    durationSec: obj["durationSec"] as? Double,
                    transcriptKnown: (obj["transcriptKnown"] as? Bool) ?? false,
                    sentences: obj["sentences"] as? Int,
                    chars: obj["chars"] as? Int,
                    translated: (obj["translated"] as? Bool) ?? false,
                    summarized: (obj["summarized"] as? Bool) ?? false)
            }
            estimate = parsed
        } catch {
            statusLine = L("fetchFailed") + " \(error)"
        }
    }

    func markEpisode(_ episode: Episode, done: Bool) {
        withEpisode(episode.id) { [weak self] api in
            _ = try? await api.episode_set_done(id: episode.id, done: done ? "1" : "0")
            await MainActor.run {
                if let feedId = self?.selectedFeed?.id { self?.loadEpisodes(feedId: feedId) }
                Task { await self?.loadFeeds() }
            }
        }
    }

    /// Mark-done from a cross-feed lane (home cards / library rows); no
    /// feed page needs to be open for the write to land.
    func setRecentDone(_ item: RecentEpisode, done: Bool) {
        withEpisode(item.id) { [weak self] api in
            _ = try? await api.episode_set_done(id: item.id, done: done ? "1" : "0")
            await MainActor.run {
                if let feedId = self?.selectedFeed?.id { self?.loadEpisodes(feedId: feedId) }
                Task { await self?.loadFeeds() }
            }
        }
    }

    /// Whether the currently selected episode has a readable transcript —
    /// drives the player bar's transcript shortcut.
    var transcriptAvailable: Bool {
        selectedEpisode?.transcriptStatus == "done" || !segments.isEmpty
    }

    /// Reopen whatever episode last saved a playback position. Selects and
    /// seeks but does not autoplay — the listener decides when to press play.
    func resumeLast() async {
        guard let api else { return }
        do {
            let row = try await api.resume_last()
            guard row.count >= 3 else { return }
            let feedId = row[0], episodeId = row[1]
            guard let feed = feeds.first(where: { $0.id == feedId }) else { return }

            selectedFeed = feed
            await loadEpisodesNow(feedId: feed.id)
            guard let episode = episodes.first(where: { $0.id == episodeId }) else { return }

            selectedEpisode = episode
            // Keep startup client access genuinely structured instead of
            // spawning five overlapping Tasks during runtime initialization.
            await loadTranscriptNow(episode)
            await loadSummaryNow(episode)
            await loadChaptersNow(episode)
            await loadEstimateNow(episode)
            // Match the Windows/Linux hosts: restoring the reading context
            // must not initialize AVFoundation or Control Center during app
            // launch. The first Play action prepares the local media lazily.
            currentTime = episode.positionSec
            duration = Double(episode.durationSec) ?? 0
            playing = false
            statusLine = L("resumed") + " " + episode.title
        } catch {
            statusLine = L("fetchFailed") + " \(error)"
        }
    }

    /// The older episode right after the current one in the selected feed —
    /// what "autoplay next" moves to when this one ends.
    private var nextEpisodeToAutoplay: Episode? {
        guard let current = selectedEpisode,
              let idx = episodes.firstIndex(where: { $0.id == current.id }),
              idx + 1 < episodes.count
        else { return nil }
        return episodes[idx + 1]
    }

    func currentChapter() -> Chapter? {
        chapters.last { currentTime >= $0.start }
    }

    func skipChapter(_ direction: Int) {
        guard !chapters.isEmpty else { return }
        if direction < 0,
           let current = chapters.last(where: { $0.start < currentTime - 2 }),
           let idx = chapters.firstIndex(where: { $0.id == current.id }) {
            seek(to: chapters[max(0, idx - (currentTime - chapters[idx].start < 5 ? 1 : 0))].start)
        } else if let next = chapters.first(where: { $0.start > currentTime + 0.5 }) {
            seek(to: next.start)
        } else if direction < 0, let first = chapters.first {
            seek(to: first.start)
        }
    }

    func savePosition(_ seconds: Double, done: Bool) {
        guard let episode = selectedEpisode else { return }
        withEpisode(episode.id) { api in
            _ = try? await api.position_save(id: episode.id,
                                             seconds: String(format: "%.0f", seconds),
                                             done: done ? "1" : "0")
        }
    }

    // MARK: playback

    /// The backend keeps the cache under `<data-dir>/audio/<episodeId><ext>`;
    /// the host resolves the file by id prefix (no extra RPC needed).
    /// PODLENS_DATA_DIR must match the backend's override (dev/test only).
    private func localAudioURL(for episodeID: String) -> URL? {
        let base: URL
        if let override = ProcessInfo.processInfo.environment["PODLENS_DATA_DIR"],
           !override.trimmingCharacters(in: .whitespaces).isEmpty {
            base = URL(fileURLWithPath: override, isDirectory: true)
        } else {
            base = FileManager.default.homeDirectoryForCurrentUser
                .appendingPathComponent(".podlens", isDirectory: true)
        }
        let audioDir = base.appendingPathComponent("audio", isDirectory: true)
        guard let names = try? FileManager.default.contentsOfDirectory(atPath: audioDir.path) else {
            return nil
        }
        guard let name = names.first(where: { $0.hasPrefix(episodeID) }) else { return nil }
        return audioDir.appendingPathComponent(name)
    }

    func startPlayback(for episode: Episode, autoplay: Bool = true) {
        previewEpisode = nil // any library playback ends a running preview
        if let url = localAudioURL(for: episode.id) {
            let item = AVPlayerItem(url: url)
            let player = player ?? AVPlayer()
            installPlaybackObservers(player)
            player.replaceCurrentItem(with: item)
            self.player = player
            if episode.positionSec > 5 {
                player.seek(to: CMTime(seconds: episode.positionSec, preferredTimescale: 600))
            }
            if autoplay {
                player.playImmediately(atRate: rate)
                playing = true
            } else {
                player.pause()
                playing = false
                duration = item.duration.seconds.isFinite ? item.duration.seconds : 0
                currentTime = episode.positionSec
            }
            saveTicks = 0
            lastFollowedSegment = nil

            npTitle = episode.title
            npShow = selectedFeed?.title ?? ""
            npArtwork = nil
            updateNowPlayingInfo()
            loadNowPlayingArtwork()
        } else {
            statusLine = L("downloading")
            startJob("download", episode: episode)
        }
    }

    /// One periodic observer drives UI time, throttled position saves and
    /// now-playing elapsed time; one end observer closes the episode out.
    private func installPlaybackObservers(_ player: AVPlayer) {
        guard timeObserver == nil else { return }
        timeObserver = player.addPeriodicTimeObserver(
            forInterval: CMTime(seconds: 0.5, preferredTimescale: 600),
            queue: .main
        ) { [weak self] time in
            Task { @MainActor in self?.playbackTicked(time.seconds) }
        }
        endObserver = NotificationCenter.default.addObserver(
            forName: AVPlayerItem.didPlayToEndTimeNotification, object: nil, queue: .main
        ) { [weak self] _ in
            Task { @MainActor in self?.playedToEnd() }
        }
    }

    private func playbackTicked(_ seconds: Double) {
        guard let p = player, let cur = p.currentItem else { return }
        currentTime = seconds
        if cur.duration.seconds.isFinite { duration = cur.duration.seconds }
        saveTicks += 1
        if saveTicks >= 12 { // ~every 6 s of playback
            saveTicks = 0
            savePosition(currentTime, done: false)
            updateNowPlayingInfo()
        }
    }

    private func playedToEnd() {
        guard player != nil else { return }
        if previewEpisode != nil {
            // a preview has no library state to close out — just stop
            playing = false
            statusLine = L("previewEnded")
            updateNowPlayingInfo()
            return
        }
        guard selectedEpisode != nil else { return }
        playing = false
        savePosition(duration > 0 ? duration : currentTime, done: true)
        updateNowPlayingInfo()
        // continuous playback: roll into the next (older) episode of the
        // same feed, like Apple Podcasts without an Up Next queue
        if let next = nextEpisodeToAutoplay {
            statusLine = L("autoplayNext")
            selectEpisode(next)
        }
    }

    func togglePlay() {
        if player == nil || player?.currentItem == nil {
            guard let episode = selectedEpisode else { return }
            startPlayback(for: episode)
            return
        }
        guard let player else { return }
        if playing {
            savePosition(player.currentTime().seconds, done: false)
            player.pause()
            playing = false
        } else {
            player.defaultRate = rate
            player.play()
            playing = true
        }
        updateNowPlayingInfo()
    }

    func remoteCommandPlay() {
        if !playing { togglePlay() }
    }

    func remoteCommandPause() {
        if playing { togglePlay() }
    }

    func seek(to seconds: Double) {
        let clamped = min(max(seconds, 0), duration > 0 ? duration : seconds)
        player?.seek(to: CMTime(seconds: clamped, preferredTimescale: 600))
        currentTime = clamped
        updateNowPlayingInfo()
    }

    func skip(by delta: Double) {
        seek(to: currentTime + delta)
    }

    func setRate(_ r: Float) {
        rate = r
        player?.defaultRate = r
        if playing { player?.rate = r }
        updateNowPlayingInfo()
    }

    // MARK: sleep timer

    func startSleepTimer(minutes: Int) {
        cancelSleepTimer()
        sleepMinutes = minutes
        sleepFireAt = Date().addingTimeInterval(TimeInterval(minutes) * 60)
        sleepTimer = Timer.scheduledTimer(withTimeInterval: TimeInterval(minutes) * 60,
                                          repeats: false) { [weak self] _ in
            Task { @MainActor in self?.sleepTimerFired() }
        }
    }

    func cancelSleepTimer() {
        sleepTimer?.invalidate()
        sleepTimer = nil
        sleepFireAt = nil
        sleepMinutes = nil
    }

    private func sleepTimerFired() {
        sleepTimer = nil
        sleepFireAt = nil
        sleepMinutes = nil
        if playing { togglePlay() }
        statusLine = L("sleepFired")
    }

    /// Whole minutes left on the armed sleep timer, for the menu label.
    var sleepMinutesRemaining: Int? {
        guard let sleepFireAt else { return nil }
        return max(1, Int(ceil(sleepFireAt.timeIntervalSinceNow / 60)))
    }

    func currentSegmentIndex() -> Int? {
        guard !segments.isEmpty else { return nil }
        return segments.firstIndex { currentTime >= $0.start && currentTime < $0.end }
    }

    /// Index of the segment the transcript is currently scrolled to; the
    /// view follows only when this changes, so 0.5 s ticks don't yank the
    /// list while the reader is inside the same sentence.
    func segmentToFollow() -> Int? {
        let idx = currentSegmentIndex()
        if idx == lastFollowedSegment { return nil }
        lastFollowedSegment = idx
        return idx
    }

    // MARK: auto refresh + notifications

    private var refreshTimer: Timer?
    private var notificationsAllowed = false

    /// Refresh every subscription on a fixed cadence and surface new
    /// episodes as system notifications. Off-interval churn (and the
    /// permission prompt) never blocks startup: authorization is asked on
    /// the first tick, and a denial just means quieter operation.
    private func scheduleAutoRefresh() {
        refreshTimer?.invalidate()
        refreshTimer = Timer.scheduledTimer(withTimeInterval: 30 * 60, repeats: true) { [weak self] _ in
            Task { @MainActor in self?.autoRefreshTick() }
        }
    }

    private func autoRefreshTick() {
        guard let api else { return }
        requestNotificationPermission()
        Task {
            let added = (try? await api.feed_refresh_all()) ?? 0
            await MainActor.run { self.statusLine = added > 0 ? String(format: L("refreshAllDone"), Int(added)) : self.statusLine }
            await loadFeeds()
            if added > 0 {
                notifyNewEpisodes(Int(added))
            }
        }
    }

    private func requestNotificationPermission() {
        guard !notificationsAllowed else { return }
        let center = UNUserNotificationCenter.current()
        center.requestAuthorization(options: [.alert]) { granted, _ in
            Task { @MainActor in self.notificationsAllowed = granted }
        }
    }

    private func notifyNewEpisodes(_ count: Int) {
        guard notificationsAllowed else { return }
        let content = UNMutableNotificationContent()
        content.title = "PodLens"
        content.body = String(format: L("notifyNewEpisodes"), count)
        let request = UNNotificationRequest(identifier: UUID().uuidString,
                                            content: content, trigger: nil)
        UNUserNotificationCenter.current().add(request)
    }
}
