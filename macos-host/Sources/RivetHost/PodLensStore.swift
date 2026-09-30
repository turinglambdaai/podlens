import AVFoundation
import SwiftUI

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

    @Published var feeds: [Feed] = []
    @Published var episodes: [Episode] = []
    @Published var selectedFeed: Feed?
    @Published var selectedEpisode: Episode?
    @Published var segments: [Segment] = []
    @Published var summary: Summary?
    @Published var jobs: [String: Job] = [:]
    @Published var statusLine = ""
    @Published var bootStatus = ""
    @Published var translationMode = 0 // 0 bilingual, 1 zh only, 2 en only
    @Published var updateOutcome: Result<UpdateService.CheckResult, Error>?
    @Published var playing = false
    @Published var currentTime: Double = 0
    @Published var duration: Double = 0
    @Published var rate: Float = 1.0

    private var api: RivetAPI?
    var apiForSettings: RivetAPI? { api }
    private var player: AVPlayer?
    private var positionTimer: Timer?

    // MARK: boot

    func bind(api: RivetAPI) {
        self.api = api
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
            loadFeeds()
        case .open_url(let url):
            if let u = URL(string: url) { NSWorkspace.shared.open(u) }
        case .update_available:
            break // surfaced through updateOutcome
        }
    }

    // MARK: feeds & episodes

    func loadFeeds() {
        guard let api else { return }
        Task {
            do {
                let rows = try await api.feed_list()
                feeds = rows.map {
                    Feed(id: $0[0], title: $0[1], author: $0[2], artworkURL: $0[3],
                         episodeCount: Int($0[4]) ?? 0, latestTitle: $0[5], latestPub: $0[6])
                }
            } catch {
                statusLine = L("fetchFailed") + " \(error)"
            }
        }
    }

    func selectFeed(_ feed: Feed) {
        selectedFeed = feed
        loadEpisodes(feedId: feed.id)
    }

    func loadEpisodes(feedId: String) {
        guard let api else { return }
        Task {
            do {
                let rows = try await api.episode_list(feed_id: feedId)
                episodes = rows.map {
                    Episode(id: $0[0], title: $0[1], pubDisplay: $0[2], durationSec: $0[3],
                            downloaded: $0[4] == "1", transcriptStatus: $0[5],
                            summaryStatus: $0[6], positionSec: Double($0[7]) ?? 0,
                            done: $0[8] == "1", hasTranslation: $0[9] == "1")
                }
            } catch {
                statusLine = L("fetchFailed") + " \(error)"
            }
        }
    }

    func addFeed(url: String) {
        guard let api, !url.trimmingCharacters(in: .whitespaces).isEmpty else { return }
        Task {
            do {
                _ = try await api.feed_add(url: url)
                loadFeeds()
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
            loadFeeds()
        }
    }

    func refreshAll() {
        guard let api else { return }
        Task {
            do {
                let n = try await api.feed_refresh_all()
                statusLine = "\(n)"
                loadFeeds()
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
        withEpisode(episode.id) { [weak self] api in
            let rows = try await api.episode_transcript(id: episode.id)
            let segs = rows.enumerated().map { i, row in
                Segment(id: i, start: Double(row[0]) ?? 0, end: Double(row[1]) ?? 0,
                        text: row[2], translation: row[3])
            }
            await MainActor.run { self?.segments = segs }
        }
    }

    func loadSummary(_ episode: Episode) {
        withEpisode(episode.id) { [weak self] api in
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
            await MainActor.run { self?.summary = parsed }
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

    /// The backend keeps the cache under ~/.podlens/audio/<episodeId><ext>;
    /// the host resolves the file by id prefix (no extra RPC needed).
    private func localAudioURL(for episodeID: String) -> URL? {
        let audioDir = FileManager.default.homeDirectoryForCurrentUser
            .appendingPathComponent(".podlens/audio", isDirectory: true)
        guard let names = try? FileManager.default.contentsOfDirectory(atPath: audioDir.path) else {
            return nil
        }
        guard let name = names.first(where: { $0.hasPrefix(episodeID) }) else { return nil }
        return audioDir.appendingPathComponent(name)
    }

    func startPlayback(for episode: Episode) {
        positionTimer?.invalidate()
        if let url = localAudioURL(for: episode.id) {
            let item = AVPlayerItem(url: url)
            let player = player ?? AVPlayer()
            player.replaceCurrentItem(with: item)
            self.player = player
            if episode.positionSec > 5 {
                player.seek(to: CMTime(seconds: episode.positionSec, preferredTimescale: 600))
            }
            player.rate = rate
            playing = true

            positionTimer = Timer.scheduledTimer(withTimeInterval: 5, repeats: true) { [weak self] _ in
                Task { @MainActor in
                    guard let self, let p = self.player, let cur = p.currentItem else { return }
                    self.currentTime = p.currentTime().seconds
                    self.duration = cur.duration.seconds.isFinite ? cur.duration.seconds : 0
                    self.savePosition(self.currentTime, done: false)
                }
            }
        } else {
            statusLine = L("downloading")
            startJob("download", episode: episode)
        }
    }

    func togglePlay() {
        guard let player else { return }
        if playing {
            savePosition(player.currentTime().seconds, done: false)
            player.pause()
            playing = false
        } else {
            player.play()
            playing = true
        }
    }

    func seek(to seconds: Double) {
        player?.seek(to: CMTime(seconds: seconds, preferredTimescale: 600))
        currentTime = seconds
    }

    func setRate(_ r: Float) {
        rate = r
        player?.rate = playing ? r : 0
    }

    func currentSegmentIndex() -> Int? {
        guard !segments.isEmpty else { return nil }
        return segments.firstIndex { currentTime >= $0.start && currentTime < $0.end }
    }
}
