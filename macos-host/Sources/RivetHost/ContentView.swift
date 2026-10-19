import AVFoundation
import SwiftUI

typealias Episode = PodLensStore.Episode
typealias Feed = PodLensStore.Feed
typealias Segment = PodLensStore.Segment

struct ContentView: View {
    @EnvironmentObject private var model: AppModel
    @State private var showAddFeed = false

    var body: some View {
        main
            .environmentObject(model.store)
            .sheet(isPresented: $showAddFeed) {
                AddFeedSheet()
                    .environmentObject(model.store)
            }
            .sheet(isPresented: model.showSettingsBinding) {
                SettingsSheet()
                    .environmentObject(model.store)
            }
            .sheet(isPresented: Binding(
                get: { model.store.updateOutcome != nil },
                set: { if !$0 { model.store.updateOutcome = nil } })) {
                UpdateSheet()
                    .environmentObject(model.store)
            }
    }

    private var main: some View {
        NavigationSplitView {
            SidebarView(showAddFeed: $showAddFeed)
                .navigationSplitViewColumnWidth(min: 210, ideal: 230)
        } content: {
            ContentColumn()
                .navigationSplitViewColumnWidth(min: 460, ideal: 540)
        } detail: {
            DetailView()
        }
        // The player rides the whole window (Apple Podcasts style), so the
        // transcript reader stays usable while browsing any page.
        .safeAreaInset(edge: .bottom, spacing: 0) {
            PlayerBar()
        }
    }
}

extension UpdateService.Manifest: @retroactive Identifiable {
    public var id: String { version }
}

// MARK: shared formatting

func formatDuration(_ seconds: Double) -> String {
    guard seconds > 0 else { return "" }
    let total = Int(seconds)
    if total >= 3600 {
        return String(format: L("hoursMinutesShort"), total / 3600, (total % 3600) / 60)
    }
    return String(format: L("minutesShort"), total / 60)
}

/// Podcasts-style short date ("10月16日" / "Oct 16") from the feed's
/// RFC-822 timestamp; anything unparsable passes through untouched.
func formatPubDate(_ raw: String) -> String {
    let parser = DateFormatter()
    parser.locale = Locale(identifier: "en_US_POSIX")
    for format in ["E, d MMM yyyy HH:mm:ss Z", "E, d MMM yyyy HH:mm:ss zzz",
                   "yyyy-MM-dd HH:mm:ss Z", "E, d MMM yyyy"] {
        parser.dateFormat = format
        if let date = parser.date(from: raw) {
            let out = DateFormatter()
            out.locale = currentLanguage() == "zh"
                ? Locale(identifier: "zh_CN")
                : Locale(identifier: "en_US_POSIX")
            out.dateFormat = currentLanguage() == "zh" ? "M月d日" : "MMM d"
            return out.string(from: date)
        }
    }
    return raw
}

/// Status chips shared by every episode row: the Understand pipeline
/// surfaces (transcript / translation / summary) plus download and
/// played state — icons with tooltips, never text noise.
struct EpisodeStatusChips: View {
    var downloaded = false
    var transcriptStatus = "none"
    var summaryStatus = "none"
    var hasTranslation = false
    var done = false

    var body: some View {
        HStack(spacing: 8) {
            if done {
                Image(systemName: "checkmark.circle.fill")
                    .foregroundStyle(.green)
                    .help(L("done"))
            }
            if downloaded {
                Image(systemName: "arrow.down.circle.fill")
                    .foregroundStyle(.secondary)
                    .help(L("tooltipDownloaded"))
            }
            if transcriptStatus == "done" {
                Image(systemName: "text.bubble")
                    .foregroundStyle(Color.accentColor)
                    .help(L("transcriptTab"))
            }
            if transcriptStatus == "running" {
                Image(systemName: "doc.text.magnifyingglass")
                    .foregroundStyle(Color.accentColor)
                    .help(L("transcribing"))
            }
            if hasTranslation {
                Image(systemName: "globe")
                    .foregroundStyle(Color.accentColor)
                    .help(L("tooltipTranslated"))
            }
            if summaryStatus == "done" {
                Image(systemName: "sparkles")
                    .foregroundStyle(Color.accentColor)
                    .help(L("tooltipSummarized"))
            }
        }
        .font(.caption)
    }
}

/// Playback progress under rows whose episode is mid-way.
struct EpisodeProgressLine: View {
    let position: Double
    let duration: Double

    var body: some View {
        if position > 5, duration > 0 {
            ProgressView(value: min(position, duration), total: duration)
                .progressViewStyle(.linear)
                .tint(DesignTokens.accent)
        }
    }
}

// MARK: content column routing

struct ContentColumn: View {
    @EnvironmentObject private var store: PodLensStore

    var body: some View {
        switch store.sidebarItem {
        case .home: HomeView()
        case .discover: DiscoverPage()
        case .shows: ShowsGridPage()
        case .recent: RecentEpisodesPage()
        case .downloaded: DownloadedPage()
        case .feed: FeedEpisodesPage()
        }
    }
}

// MARK: sidebar

struct SidebarView: View {
    @EnvironmentObject private var store: PodLensStore
    @Binding var showAddFeed: Bool

    var body: some View {
        List(selection: Binding(
            get: { store.sidebarItem },
            set: { item in
                store.sidebarItem = item
                if case .feed(let feed) = item {
                    store.selectFeed(feed)
                }
            })) {
            Label(L("homeTab"), systemImage: "house")
                .tag(SidebarItem.home)
            Label(L("discover"), systemImage: "sparkles.rectangle.stack")
                .tag(SidebarItem.discover)

            Section(L("librarySection")) {
                Label(L("recentlyUpdated"), systemImage: "clock")
                    .tag(SidebarItem.recent)
                Label(L("showsTab"), systemImage: "square.grid.2x2")
                    .tag(SidebarItem.shows)
                Label(L("downloadedTab"), systemImage: "arrow.down.circle")
                    .tag(SidebarItem.downloaded)
            }

            Section(L("subscriptionsSection")) {
                ForEach(store.feeds) { feed in
                    HStack(spacing: 10) {
                        ArtworkView(url: feed.artworkURL)
                            .frame(width: 34, height: 34)
                        VStack(alignment: .leading, spacing: 2) {
                            Text(feed.title).lineLimit(1)
                            HStack(spacing: 4) {
                                Text("\(feed.episodeCount)")
                                    .font(.caption)
                                    .foregroundStyle(.secondary)
                                if feed.unplayed > 0 {
                                    Text("\(feed.unplayed)")
                                        .font(.caption2.bold())
                                        .foregroundStyle(.white)
                                        .padding(.horizontal, 6)
                                        .padding(.vertical, 1)
                                        .background(Color.accentColor)
                                        .clipShape(Capsule())
                                }
                            }
                        }
                    }
                    .tag(SidebarItem.feed(feed))
                    .contextMenu {
                        Button(L("removeFeed"), role: .destructive) {
                            store.removeFeed(feed)
                        }
                    }
                }
            }
        }
        .listStyle(.sidebar)
        .safeAreaInset(edge: .bottom) {
            HStack {
                Button {
                    showAddFeed = true
                } label: {
                    Image(systemName: "plus.circle")
                }
                .buttonStyle(.borderless)
                .help(L("addFeed"))

                Button {
                    store.refreshAll()
                } label: {
                    Image(systemName: "arrow.clockwise")
                }
                .buttonStyle(.borderless)
                .help(L("menuRefreshAll"))

                if case .feed(let feed) = store.sidebarItem {
                    Button {
                        store.removeFeed(feed)
                    } label: {
                        Image(systemName: "minus.circle")
                    }
                    .buttonStyle(.borderless)
                    .help(L("removeFeed"))
                }

                Spacer()
                Text(store.statusLine)
                    .font(.caption2)
                    .foregroundStyle(.secondary)
                    .lineLimit(1)
            }
            .padding(8)
        }
    }
}

// MARK: home (Podcasts-style lanes)

struct HomeView: View {
    @EnvironmentObject private var store: PodLensStore

    var body: some View {
        if store.feeds.isEmpty {
            emptyLibrary
                .navigationTitle(L("homeTab"))
        } else {
            ScrollView {
                LazyVStack(alignment: .leading, spacing: 26) {
                    LaneHeader(title: L("upNextLane"))
                    UpNextLane()
                    LaneHeader(title: L("myShowsLane"), destination: .shows)
                    ShowsGrid(shows: store.homeShows)
                    LaneHeader(title: L("latestEpisodesLane"), destination: .recent)
                    VStack(spacing: 0) {
                        ForEach(Array(store.recentEpisodes.prefix(6))) { item in
                            RecentEpisodeRow(item: item, showFeed: true)
                        }
                    }
                }
                .padding(.horizontal, 24)
                .padding(.bottom, 24)
            }
            .navigationTitle(L("homeTab"))
        }
    }

    private var emptyLibrary: some View {
        VStack(spacing: 14) {
            Image(systemName: "sparkles.rectangle.stack")
                .font(.system(size: 44))
                .foregroundStyle(.tertiary)
            Text(L("emptyHome"))
                .multilineTextAlignment(.center)
                .foregroundStyle(.secondary)
            Button(L("goToDiscover")) {
                store.sidebarItem = .discover
            }
            .buttonStyle(.borderedProminent)
                .font(.caption2.monospaced())
                .foregroundStyle(.red)
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity)
    }
}

/// Lane title with the Podcasts "see all" chevron when a library page
/// holds the full list.
struct LaneHeader: View {
    @EnvironmentObject private var store: PodLensStore
    let title: String
    var destination: SidebarItem?

    var body: some View {
        HStack {
            Text(title).font(.title3.bold())
            if let destination {
                Button {
                    store.sidebarItem = destination
                } label: {
                    Image(systemName: "chevron.right")
                        .font(.subheadline.bold())
                        .foregroundStyle(.secondary)
                }
                .buttonStyle(.borderless)
                .help(L("seeAll"))
            }
        }
    }
}

/// The 待播清单 lane: large cover cards, in-progress episodes first, then
/// each show's newest unplayed episode — Apple Podcasts' Up Next for a
/// library that has no explicit queue.
struct UpNextLane: View {
    @EnvironmentObject private var store: PodLensStore

    var body: some View {
        ScrollView(.horizontal, showsIndicators: false) {
            LazyHStack(spacing: 14) {
                ForEach(store.upNext) { item in
                    UpNextCard(item: item)
                }
            }
            .padding(.vertical, 4)
        }
    }
}

struct UpNextCard: View {
    @EnvironmentObject private var store: PodLensStore
    let item: RecentEpisode

    private var meta: String {
        let lane = item.positionSec > 5 ? L("metaContinue") : L("metaNewEpisode")
        return item.pubDisplay.isEmpty ? lane : "\(lane) · \(formatPubDate(item.pubDisplay))"
    }

    var body: some View {
        ZStack(alignment: .bottomLeading) {
            ArtworkView(url: item.artworkURL, cornerRadius: 14)
            LinearGradient(colors: [.clear, .black.opacity(0.78)],
                           startPoint: .center, endPoint: .bottom)

            VStack(alignment: .leading, spacing: 6) {
                Text(meta)
                    .font(.caption)
                    .foregroundStyle(.white.opacity(0.85))
                Text(item.title)
                    .font(.subheadline.bold())
                    .foregroundStyle(.white)
                    .lineLimit(2)
                    .multilineTextAlignment(.leading)
                HStack(spacing: 8) {
                    HStack(spacing: 5) {
                        Image(systemName: "play.fill")
                        Text(formatDuration(item.duration))
                    }
                    .font(.caption.bold())
                    .foregroundStyle(.white)
                    .padding(.horizontal, 12)
                    .padding(.vertical, 6)
                    .background(.ultraThinMaterial, in: Capsule())

                    Spacer()

                    Menu {
                        sectionActions
                    } label: {
                        Image(systemName: "ellipsis")
                            .font(.caption.bold())
                            .foregroundStyle(.white)
                            .frame(width: 26, height: 26)
                            .background(.ultraThinMaterial, in: Circle())
                    }
                    .menuStyle(.borderlessButton)
                    .fixedSize()
                }
            }
            .padding(12)
        }
        .frame(width: 280, height: 280)
        .contentShape(Rectangle())
        .onTapGesture {
            store.selectRecent(item)
        }
    }

    @ViewBuilder
    private var sectionActions: some View {
        Button(L("pipeline")) {
            store.selectRecent(item)
            if let e = store.selectedEpisode {
                store.startJob("pipeline", episode: e)
            }
        }
        Button(item.downloaded ? L("removeAudio") : L("download")) {
            if item.downloaded {
                store.withEpisode(item.id) { api in
                    _ = try? await api.episode_remove_audio(id: item.id)
                }
            } else {
                store.withEpisode(item.id) { api in
                    _ = try? await api.episode_download(id: item.id)
                }
            }
        }
        Button(item.done ? L("markUnplayed") : L("markPlayed")) {
            store.setRecentDone(item, done: !item.done)
        }
    }
}

/// Cover-tile grid for subscriptions (home lane + library Shows page).
struct ShowsGrid: View {
    @EnvironmentObject private var store: PodLensStore
    let shows: [Feed]

    var body: some View {
        LazyVGrid(columns: [GridItem(.adaptive(minimum: 132), spacing: 16)], spacing: 16) {
            ForEach(shows) { feed in
                ShowTile(feed: feed)
            }
        }
    }
}

struct ShowTile: View {
    @EnvironmentObject private var store: PodLensStore
    let feed: Feed

    var body: some View {
        VStack(alignment: .leading, spacing: 6) {
            ZStack(alignment: .topTrailing) {
                ArtworkView(url: feed.artworkURL, cornerRadius: 12)
                if feed.unplayed > 0 {
                    Text("\(feed.unplayed)")
                        .font(.caption2.bold())
                        .foregroundStyle(.white)
                        .padding(.horizontal, 7)
                        .padding(.vertical, 2)
                        .background(Color.accentColor, in: Capsule())
                        .padding(6)
                }
            }
            VStack(alignment: .leading, spacing: 1) {
                Text(feed.title)
                    .font(.callout)
                    .lineLimit(1)
                Text(String(format: L("episodesCount"), feed.episodeCount))
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }
        }
        .contentShape(Rectangle())
        .onTapGesture {
            store.selectFeed(feed)
            store.sidebarItem = .feed(feed)
        }
        .contextMenu {
            Button(L("removeFeed"), role: .destructive) {
                store.removeFeed(feed)
            }
        }
    }
}

/// Cross-feed episode row shared by home's latest lane and the library
/// pages. `showFeed` brands the row with the show name.
struct RecentEpisodeRow: View {
    @EnvironmentObject private var store: PodLensStore
    let item: RecentEpisode
    var showFeed = false

    var body: some View {
        HStack(spacing: 12) {
            ArtworkView(url: item.artworkURL)
                .frame(width: 56, height: 56)
            VStack(alignment: .leading, spacing: 4) {
                Text(item.title)
                    .font(.system(size: 13, weight: .semibold))
                    .lineLimit(2)
                Text(rowMeta)
                    .font(.caption)
                    .foregroundStyle(.secondary)
                    .lineLimit(1)
                HStack(spacing: 10) {
                    EpisodeStatusChips(downloaded: item.downloaded,
                                       transcriptStatus: item.transcriptStatus,
                                       summaryStatus: item.summaryStatus,
                                       hasTranslation: item.hasTranslation,
                                       done: item.done)
                    if !item.description.isEmpty {
                        Text(item.description)
                            .font(.caption)
                            .foregroundStyle(.tertiary)
                            .lineLimit(1)
                    }
                }
                EpisodeProgressLine(position: item.positionSec, duration: item.duration)
            }
            Spacer(minLength: 8)
            Button {
                store.selectRecent(item)
            } label: {
                Image(systemName: "play.circle")
                    .font(.title2)
            }
            .buttonStyle(.borderless)
            .help(L("play"))
        }
        .padding(.vertical, 8)
        .contentShape(Rectangle())
        .onTapGesture {
            store.selectRecent(item, autoplay: false)
        }
    }

    private var rowMeta: String {
        var parts: [String] = []
        if showFeed { parts.append(item.feedTitle) }
        parts.append(formatPubDate(item.pubDisplay))
        let d = formatDuration(item.duration)
        if !d.isEmpty { parts.append(d) }
        return parts.joined(separator: " · ")
    }
}

// MARK: library pages

struct RecentEpisodesPage: View {
    @EnvironmentObject private var store: PodLensStore

    var body: some View {
        Group {
            if store.recentEpisodes.isEmpty {
                emptyHint(L("recentEmpty"))
            } else {
                ScrollView {
                    LazyVStack(spacing: 0) {
                        ForEach(store.recentEpisodes) { item in
                            RecentEpisodeRow(item: item, showFeed: true)
                        }
                    }
                    .padding(.horizontal, 20)
                }
            }
        }
        .navigationTitle(L("recentlyUpdated"))
    }
}

struct DownloadedPage: View {
    @EnvironmentObject private var store: PodLensStore

    var body: some View {
        Group {
            if store.downloadedEpisodes.isEmpty {
                emptyHint(L("downloadedEmpty"))
            } else {
                ScrollView {
                    LazyVStack(spacing: 0) {
                        ForEach(store.downloadedEpisodes) { item in
                            RecentEpisodeRow(item: item, showFeed: true)
                        }
                    }
                    .padding(.horizontal, 20)
                }
            }
        }
        .navigationTitle(L("downloadedTab"))
    }
}

private func emptyHint(_ text: String) -> some View {
    Text(text)
        .foregroundStyle(.secondary)
        .frame(maxWidth: .infinity, maxHeight: .infinity)
}

struct ShowsGridPage: View {
    @EnvironmentObject private var store: PodLensStore

    var body: some View {
        ScrollView {
            ShowsGrid(shows: store.feeds)
                .padding(.horizontal, 20)
                .padding(.bottom, 24)
        }
        .navigationTitle(L("showsTab"))
    }
}

// MARK: discover page (was a sheet; now the content column)

struct DiscoverPage: View {
    @EnvironmentObject private var store: PodLensStore
    @State private var query = ""

    var body: some View {
        VStack(spacing: 0) {
            HStack {
                TextField(L("searchPlaceholder"), text: $query)
                    .textFieldStyle(.roundedBorder)
                    .submitLabel(.search)
                    .onSubmit { store.searchDirectory(query) }
                Button {
                    store.searchDirectory(query)
                } label: {
                    Image(systemName: "magnifyingglass")
                }
                .buttonStyle(.borderless)
                .disabled(query.trimmingCharacters(in: .whitespaces).isEmpty)
            }
            .padding(.horizontal, 20)
            .padding(.vertical, 12)

            if store.isSearching {
                ProgressView(L("searching"))
                    .frame(maxWidth: .infinity, maxHeight: .infinity)
            } else if store.searchError != nil {
                Text(L("searchFailed") + " \(store.searchError!)")
                    .font(.caption)
                    .foregroundStyle(.red)
                    .frame(maxWidth: .infinity, maxHeight: .infinity)
            } else if !store.searchResults.isEmpty {
                ScrollView {
                    LazyVStack(alignment: .leading, spacing: 8) {
                        ForEach(store.searchResults) { entry in
                            CatalogCard(entry: entry)
                        }
                    }
                    .padding(.horizontal, 20)
                    .padding(.bottom, 16)
                }
            } else {
                ScrollView {
                    LazyVStack(alignment: .leading, spacing: 22) {
                        ForEach(categories, id: \.self) { category in
                            let items = store.catalog.filter { $0.category == category }
                            if !items.isEmpty {
                                VStack(alignment: .leading, spacing: 10) {
                                    Text(categoryLabel(category))
                                        .font(.headline)
                                    ForEach(items) { entry in
                                        CatalogCard(entry: entry)
                                    }
                                }
                            }
                        }
                    }
                    .padding(.horizontal, 20)
                    .padding(.bottom, 16)
                }
            }
        }
        .navigationTitle(L("discover"))
        .task { await store.loadCatalog() }
    }

    private var categories: [String] {
        var seen: [String] = []
        for entry in store.catalog where !seen.contains(entry.category) {
            seen.append(entry.category)
        }
        return seen
    }

    private func categoryLabel(_ category: String) -> String {
        switch category {
        case "tech": return L("catTech")
        case "security": return L("catSecurity")
        case "science": return L("catScience")
        case "design": return L("catDesign")
        case "business": return L("catBusiness")
        case "news": return L("catNews")
        default: return category
        }
    }
}

struct CatalogCard: View {
    @EnvironmentObject private var store: PodLensStore
    let entry: PodLensStore.CatalogEntry

    var body: some View {
        HStack(spacing: 12) {
            ArtworkView(url: entry.artworkURL, cornerRadius: 8)
                .frame(width: 56, height: 56)
            VStack(alignment: .leading, spacing: 3) {
                Text(entry.title).font(.callout.bold())
                Text(entry.description)
                    .font(.caption)
                    .foregroundStyle(.secondary)
                    .lineLimit(2)
            }
            Spacer()
            if entry.added {
                Text(L("subscribed"))
                    .font(.caption)
                    .foregroundStyle(.secondary)
                    .padding(.horizontal, 10)
                    .padding(.vertical, 5)
                    .background(Color.gray.opacity(0.15))
                    .clipShape(Capsule())
            } else {
                Button(L("subscribe")) {
                    store.addFromCatalog(entry)
                }
                .buttonStyle(.borderedProminent)
                .controlSize(.small)
            }
        }
        .padding(10)
        .background(Color.primary.opacity(0.03))
        .clipShape(RoundedRectangle(cornerRadius: 10))
    }
}

// MARK: add feed

struct AddFeedSheet: View {
    @EnvironmentObject private var store: PodLensStore
    @Environment(\.dismiss) private var dismiss
    @State private var url = ""

    var body: some View {
        VStack(spacing: 16) {
            Text(L("addFeed")).font(.headline)
            TextField(L("feedURL"), text: $url)
                .textFieldStyle(.roundedBorder)
                .frame(minWidth: 420)
            HStack {
                Button(L("cancel")) { dismiss() }.keyboardShortcut(.cancelAction)
                Button(L("add")) {
                    store.addFeed(url: url)
                    dismiss()
                }
                .keyboardShortcut(.defaultAction)
                .buttonStyle(.borderedProminent)
            }
        }
        .padding(24)
    }
}

// MARK: episodes (one feed)

struct FeedEpisodesPage: View {
    @EnvironmentObject private var store: PodLensStore

    var body: some View {
        Group {
            if store.episodes.isEmpty {
                Text(L("recentEmpty")).foregroundStyle(.secondary)
                    .frame(maxWidth: .infinity, maxHeight: .infinity)
            } else {
                List(selection: Binding(get: { store.selectedEpisode },
                                        set: { if let e = $0 { store.selectEpisode(e) } })) {
                    ForEach(store.episodes) { episode in
                        FeedEpisodeRow(episode: episode,
                                       artworkURL: store.selectedFeed?.artworkURL ?? "")
                            .tag(episode)
                            .contextMenu {
                                Button(episode.done ? L("markUnplayed") : L("markPlayed")) {
                                    store.markEpisode(episode, done: !episode.done)
                                }
                            }
                    }
                }
                .listStyle(.inset)
            }
        }
        .navigationTitle(store.selectedFeed?.title ?? L("episodes"))
    }
}

struct FeedEpisodeRow: View {
    @EnvironmentObject private var store: PodLensStore
    let episode: Episode
    let artworkURL: String

    var body: some View {
        HStack(spacing: 12) {
            ArtworkView(url: artworkURL)
                .frame(width: 56, height: 56)
            VStack(alignment: .leading, spacing: 4) {
                Text(episode.title)
                    .font(.system(size: 13, weight: .semibold))
                    .lineLimit(2)
                HStack(spacing: 6) {
                    Text(formatPubDate(episode.pubDisplay))
                    let d = formatDuration(Double(episode.durationSec) ?? 0)
                    if !d.isEmpty { Text(d) }
                }
                .font(.caption)
                .foregroundStyle(.secondary)
                if !episode.description.isEmpty {
                    Text(episode.description)
                        .font(.caption)
                        .foregroundStyle(.tertiary)
                        .lineLimit(2)
                }
                HStack(spacing: 10) {
                    EpisodeStatusChips(downloaded: episode.downloaded,
                                       transcriptStatus: episode.transcriptStatus,
                                       summaryStatus: episode.summaryStatus,
                                       hasTranslation: episode.hasTranslation,
                                       done: episode.done)
                }
                EpisodeProgressLine(position: episode.positionSec,
                                    duration: Double(episode.durationSec) ?? 0)
            }
            Spacer(minLength: 8)
            Button {
                store.selectEpisode(episode)
            } label: {
                Image(systemName: "play.circle")
                    .font(.title2)
            }
            .buttonStyle(.borderless)
            .help(L("play"))
        }
        .padding(.vertical, 4)
    }
}

// MARK: detail (episode header + transcript + summary)

struct DetailView: View {
    @EnvironmentObject private var store: PodLensStore

    var body: some View {
        VStack(spacing: 0) {
            if let episode = store.selectedEpisode {
                EpisodeHeader(episode: episode)
                Picker("", selection: Binding(get: { store.detailTab },
                                              set: { store.detailTab = $0 })) {
                    Text(L("transcriptTab")).tag(0)
                    Text(L("summaryTab")).tag(1)
                }
                .pickerStyle(.segmented)
                .padding([.horizontal, .top], 12)

                if store.detailTab == 0 {
                    TranscriptView()
                } else {
                    SummaryView()
                }
            } else {
                Text(L("selectEpisode"))
                    .foregroundStyle(.secondary)
                    .frame(maxWidth: .infinity, maxHeight: .infinity)
            }
        }
    }
}

struct EpisodeHeader: View {
    @EnvironmentObject private var store: PodLensStore
    let episode: Episode

    var body: some View {
        HStack(alignment: .top, spacing: 14) {
            ArtworkView(url: store.selectedFeed?.artworkURL ?? "", cornerRadius: 10)
                .frame(width: 96, height: 96)

            VStack(alignment: .leading, spacing: 6) {
                Text(episode.title)
                    .font(.title3.bold())
                    .lineLimit(2)
                Text(headerMeta)
                    .font(.caption)
                    .foregroundStyle(.secondary)

                HStack(spacing: 8) {
                    Button {
                        store.startJob("pipeline", episode: episode)
                    } label: {
                        Label(L("pipeline"), systemImage: "wand.and.stars")
                    }
                    .buttonStyle(.borderedProminent)
                    .help(estimateHint)

                    Menu {
                        Button(L("download")) {
                            store.startJob("download", episode: episode)
                        }
                        Button(L("transcribe")) {
                            store.startJob("transcribe", episode: episode)
                        }
                        Button(L("translate")) {
                            store.startJob("translate", episode: episode)
                        }
                        Button(L("summarize")) {
                            store.startJob("summarize", episode: episode)
                        }
                        Divider()
                        Button(episode.done ? L("markUnplayed") : L("markPlayed")) {
                            store.markEpisode(episode, done: !episode.done)
                        }
                        if episode.downloaded {
                            Button(L("removeAudio"), role: .destructive) {
                                store.withEpisode(episode.id) { api in
                                    _ = try? await api.episode_remove_audio(id: episode.id)
                                }
                            }
                        }
                    } label: {
                        Label(L("more"), systemImage: "ellipsis.circle")
                    }

                    if let job = activeJob {
                        ProgressView(value: Double(job.pct), total: 100) {
                            Text("\(job.message.isEmpty ? L("transcribing") : job.message) \(job.pct)%")
                                .font(.caption2)
                        }
                        .frame(width: 180)
                    }
                }

                if let hint = estimateLine {
                    Text(hint)
                        .font(.caption2)
                        .foregroundStyle(.tertiary)
                }
            }
        }
        .frame(maxWidth: .infinity, alignment: .leading)
        .padding(14)
    }

    private var headerMeta: String {
        var parts: [String] = []
        if let feed = store.selectedFeed { parts.append(feed.title) }
        parts.append(episode.pubDisplay)
        let d = formatDuration(Double(episode.durationSec) ?? 0)
        if !d.isEmpty { parts.append(d) }
        if episode.downloaded { parts.append(L("tooltipDownloaded")) }
        return parts.joined(separator: " · ")
    }

    private var activeJob: PodLensStore.Job? {
        store.jobs.values.first { $0.status == "running" && $0.pct > 0 }
    }

    private var estimateLine: String? {
        guard let est = store.estimate else { return nil }
        var parts: [String] = []
        if let seconds = est.durationSec, seconds > 0 {
            parts.append(String(format: "%d min", Int(seconds / 60)))
        }
        if est.transcriptKnown, let sentences = est.sentences {
            parts.append(String(format: L("estimateSentences"), sentences))
        }
        if let chars = est.chars {
            parts.append(String(format: L("estimateChars"), chars))
        }
        if est.translated { parts.append(L("estimateTranslated")) }
        if est.summarized { parts.append(L("estimateSummarized")) }
        return parts.isEmpty ? nil : parts.joined(separator: " · ")
    }

    private var estimateHint: String {
        guard let est = store.estimate else { return L("pipeline") }
        if est.summarized { return L("pipelineNothingTodo") }
        if est.transcriptKnown, let chars = est.chars {
            return String(format: L("pipelineCostKnown"), chars)
        }
        return L("pipelineCostUnknown")
    }
}

struct TranscriptView: View {
    @EnvironmentObject private var store: PodLensStore

    var body: some View {
        ScrollViewReader { proxy in
            List {
                if store.segments.isEmpty {
                    Text(L("noTranscript"))
                        .foregroundStyle(.secondary)
                }
                ForEach(store.segments) { seg in
                    let active = store.currentSegmentIndex() == seg.id
                    VStack(alignment: .leading, spacing: 3) {
                        Text(formatTime(seg.start))
                            .font(.caption2.monospacedDigit())
                            .foregroundStyle(.tertiary)
                        Text(display(seg))
                            .font(.system(size: active ? 15 : 14))
                            .foregroundStyle(active ? Color.primary : Color.primary.opacity(0.75))
                            .background(active ? DesignTokens.accentSoft : .clear)
                        if store.translationMode != 2 && !seg.translation.isEmpty {
                            Text(seg.translation)
                                .font(.system(size: 14))
                                .foregroundStyle(active ? Color.accentColor : .secondary)
                        }
                        if store.translationMode == 2 {
                            Text(seg.text)
                                .font(.system(size: 14))
                                .foregroundStyle(active ? Color.accentColor : .secondary)
                                .opacity(store.translationMode == 2 ? 0 : 1)
                        }
                    }
                    .id(seg.id)
                    .contentShape(Rectangle())
                    .onTapGesture {
                        store.seek(to: seg.start)
                    }
                }
            }
            .onChange(of: store.currentTime) { _, _ in
                // follow only when the active sentence changes, so the
                // half-second ticks don't drag the list under the reader
                if let idx = store.segmentToFollow() {
                    proxy.scrollTo(idx, anchor: .center)
                }
            }
            .safeAreaInset(edge: .top) {
                HStack {
                    Picker("", selection: Binding(get: { store.translationMode },
                                                  set: { store.translationMode = $0 })) {
                        Text(L("bilingual")).tag(0)
                        Text(L("zhOnly")).tag(1)
                        Text(L("enOnly")).tag(2)
                    }
                    .pickerStyle(.segmented)
                    .frame(width: 260)
                    Spacer()
                }
                .padding([.horizontal, .bottom], 8)
            }
        }
    }

    private func display(_ seg: PodLensStore.Segment) -> String {
        store.translationMode == 1 && !seg.translation.isEmpty ? seg.translation : seg.text
    }

    private func formatTime(_ t: Double) -> String {
        let m = Int(t) / 60, s = Int(t) % 60
        return String(format: "%02d:%02d", m, s)
    }
}

struct SummaryView: View {
    @EnvironmentObject private var store: PodLensStore

    var body: some View {
        ScrollView {
            if let s = store.summary {
                VStack(alignment: .leading, spacing: 16) {
                    section(L("tldr")) { Text(s.tldr) }
                    if !s.keyPoints.isEmpty {
                        section(L("keyPoints")) {
                            ForEach(Array(s.keyPoints.enumerated()), id: \.offset) { _, p in
                                HStack(alignment: .top) {
                                    Text("•")
                                    Text(p)
                                }
                            }
                        }
                    }
                    if !s.quotes.isEmpty {
                        section(L("quotes")) {
                            ForEach(Array(s.quotes.enumerated()), id: \.offset) { _, q in
                                VStack(alignment: .leading, spacing: 2) {
                                    Text("“\(q.0)”").italic()
                                    Text(q.1).foregroundStyle(.secondary)
                                }
                            }
                        }
                    }
                    if !s.topics.isEmpty {
                        section(L("topics")) {
                            HStack {
                                ForEach(s.topics, id: \.self) { t in
                                    Text(t)
                                        .font(.caption)
                                        .padding(.horizontal, 8)
                                        .padding(.vertical, 3)
                                        .background(Color.accentColor.opacity(0.15))
                                        .clipShape(Capsule())
                                }
                            }
                        }
                    }
                }
                .padding(16)
            } else {
                Text(L("noSummary")).foregroundStyle(.secondary)
                    .frame(maxWidth: .infinity, maxHeight: .infinity)
            }
        }
    }

    private func section<Content: View>(_ title: String, @ViewBuilder content: () -> Content) -> some View {
        VStack(alignment: .leading, spacing: 8) {
            Text(title).font(.headline).foregroundStyle(Color.accentColor)
            content()
        }
        .frame(maxWidth: .infinity, alignment: .leading)
    }
}

// MARK: player bar (window-level, Apple Podcasts style)

struct PlayerBar: View {
    @EnvironmentObject private var store: PodLensStore

    private var hasEpisode: Bool { store.selectedEpisode != nil }

    var body: some View {
        HStack(spacing: 12) {
            nowPlaying
            transcriptShortcut

            chapterControls

            Button {
                store.skip(by: -15)
            } label: {
                Image(systemName: "gobackward.15")
            }
            .buttonStyle(.borderless)
            .keyboardShortcut(.leftArrow, modifiers: [.command])
            .disabled(!hasEpisode)
            .help(L("skipBack"))

            Button {
                store.togglePlay()
            } label: {
                Image(systemName: store.playing ? "pause.fill" : "play.fill")
                    .font(.title3)
            }
            .buttonStyle(.borderless)
            .keyboardShortcut(.space, modifiers: [])
            .disabled(!hasEpisode)
            .help(store.playing ? L("pause") : L("play"))

            Button {
                store.skip(by: 30)
            } label: {
                Image(systemName: "goforward.30")
            }
            .buttonStyle(.borderless)
            .keyboardShortcut(.rightArrow, modifiers: [.command])
            .disabled(!hasEpisode)
            .help(L("skipForward"))

            Text(formatTime(store.currentTime))
                .font(.caption.monospacedDigit())
                .foregroundStyle(hasEpisode ? .primary : .tertiary)
            Slider(value: Binding(get: { store.currentTime },
                                  set: { store.seek(to: $0) }),
                   in: 0...(max(store.duration, 1)))
                .disabled(!hasEpisode)
            Text(formatTime(store.duration))
                .font(.caption.monospacedDigit())
                .foregroundStyle(hasEpisode ? .primary : .tertiary)

            Picker("", selection: Binding(get: { store.rate },
                                          set: { store.setRate($0) })) {
                ForEach(PodLensStore.playbackRates, id: \.self) { r in
                    Text("\(r, specifier: "%g")×").tag(r)
                }
            }
            .frame(width: 72)
            .disabled(!hasEpisode)

            sleepMenu
        }
        .padding(.horizontal, 16)
        .padding(.vertical, 10)
        .background(.bar)
    }

    /// Show + title block on the left; when nothing is selected the bar
    /// still renders (Podcasts never hides its controls).
    @ViewBuilder
    private var nowPlaying: some View {
        if let episode = store.selectedEpisode {
            HStack(spacing: 10) {
                ArtworkView(url: store.selectedFeed?.artworkURL ?? "")
                    .frame(width: 40, height: 40)
                VStack(alignment: .leading, spacing: 1) {
                    Text(episode.title)
                        .font(.caption.bold())
                        .lineLimit(1)
                    Text(store.npShow.isEmpty
                         ? (store.selectedFeed?.title ?? "")
                         : store.npShow)
                        .font(.caption2)
                        .foregroundStyle(.secondary)
                        .lineLimit(1)
                }
            }
            .frame(width: 230, alignment: .leading)
        } else {
            Text(L("nothingPlaying"))
                .font(.caption)
                .foregroundStyle(.secondary)
                .frame(width: 230, alignment: .leading)
        }
    }

    /// PodLens' edge: jump straight into the bilingual transcript reader.
    @ViewBuilder
    private var transcriptShortcut: some View {
        if store.transcriptAvailable {
            Button {
                store.detailTab = 0
            } label: {
                Image(systemName: "text.bubble")
            }
            .buttonStyle(.borderless)
            .help(L("transcriptTab"))
        }
    }

    @ViewBuilder
    private var chapterControls: some View {
        if !store.chapters.isEmpty {
            Button {
                store.skipChapter(-1)
            } label: {
                Image(systemName: "backward.end.fill")
            }
            .buttonStyle(.borderless)
            .help(store.currentChapter()?.title ?? L("chapterPrev"))

            Button {
                store.skipChapter(1)
            } label: {
                Image(systemName: "forward.end.fill")
            }
            .buttonStyle(.borderless)
            .help(L("chapterNext"))
        }
    }

    @ViewBuilder
    private var sleepMenu: some View {
        let remaining = store.sleepMinutesRemaining
        Menu {
            Button(L("sleepOff")) { store.cancelSleepTimer() }
            ForEach([5, 10, 15, 30, 45, 60, 90], id: \.self) { m in
                Button(String(format: L("sleepMinutes"), m)) {
                    store.startSleepTimer(minutes: m)
                }
            }
        } label: {
            Image(systemName: remaining == nil ? "moon.zzz" : "moon.zzz.fill")
        }
        .menuStyle(.borderlessButton)
        .fixedSize()
        .disabled(!hasEpisode)
        .help(remaining.map { String(format: L("sleepActive"), $0) } ?? L("sleep"))
    }

    private func formatTime(_ t: Double) -> String {
        guard t.isFinite else { return "00:00" }
        let m = Int(t) / 60, s = Int(t) % 60
        return String(format: "%02d:%02d", m, s)
    }
}

// MARK: settings

struct SettingsSheet: View {
    @EnvironmentObject private var store: PodLensStore
    @Environment(\.dismiss) private var dismiss
    @State private var rows: [(String, String, String)] = []
    @State private var lang = currentLanguage()
    @State private var saved = false

    var body: some View {
        VStack(spacing: 12) {
            Text(L("settings")).font(.headline)
            Form {
                ForEach(rows, id: \.0) { row in
                    HStack {
                        VStack(alignment: .leading) {
                            Text(row.0).font(.callout.monospaced())
                            Text(row.2).font(.caption2).foregroundStyle(.secondary)
                        }
                        TextField(row.1, text: Binding(
                            get: { row.1 },
                            set: { newValue in
                                if let idx = rows.firstIndex(where: { $0.0 == row.0 }) {
                                    rows[idx].1 = newValue
                                }
                            }))
                            .textFieldStyle(.roundedBorder)
                            .frame(width: 320)
                            .accessibilityLabel(row.0)
                    }
                }
            }
            Picker(L("language"), selection: Binding(get: { lang },
                                                     set: { lang = $0; setLanguage($0) })) {
                Text("中文").tag("zh")
                Text("English").tag("en")
            }
            .pickerStyle(.segmented)
            .frame(width: 220)

            Text(L("settingsDesc")).font(.caption2).foregroundStyle(.secondary)

            Divider()

            aboutSection

            HStack {
                if saved { Text(L("settingsSaved")).foregroundStyle(.green) }
                Button(L("cancel")) { dismiss() }
                Button(L("settingsSaved")) { save() }
                    .buttonStyle(.borderedProminent)
                    .accessibilityIdentifier("SettingsSaveButton")
            }
        }
        .padding(24)
        .task { load() }
    }

    /// About block: version, license and the support surface (taskly's
    /// AboutSheet facts) — a bug report starts one click away.
    private var aboutSection: some View {
        VStack(spacing: 6) {
            Text("PodLens v\(version)")
                .font(.callout.bold())
            Text(L("aboutLicense"))
                .font(.caption)
                .foregroundStyle(.secondary)
            HStack(spacing: 20) {
                Link(L("aboutWebsite"),
                     destination: URL(string: "https://podlens.jrtx.site")!)
                Link(L("aboutReportIssue"),
                     destination: URL(string: "https://github.com/turinglambdaai/podlens/issues")!)
            }
            .font(.caption)
        }
    }

    private var version: String {
        Bundle.main.object(forInfoDictionaryKey: "CFBundleShortVersionString") as? String
            ?? RivetGeneratedConfig.version
    }

    private func load() {
        Task {
            guard let api = store.apiForSettings else { return }
            let list = (try? await api.settings_list()) ?? []
            rows = list.map { ($0[0], $0[1], $0[2]) }
        }
    }

    private func save() {
        Task {
            guard let api = store.apiForSettings else { return }
            for row in rows {
                _ = try? await api.settings_set(key: row.0, value: row.1)
            }
            saved = true
        }
    }
}

// MARK: update sheet

/// Every terminal check outcome reaches this sheet (docs/UPDATE.md): an
/// available update offers install, an up-to-date answer confirms it, and a
/// failure shows the error — selectable, so it can travel into a bug report.
/// Silent launch-time checks only ever assign `.available`, so those never
/// nag.
struct UpdateSheet: View {
    @EnvironmentObject private var model: AppModel
    @EnvironmentObject private var store: PodLensStore
    @Environment(\.dismiss) private var dismiss

    var body: some View {
        VStack(spacing: 16) {
            Image(systemName: "arrow.down.circle.fill")
                .font(.system(size: 40))
                .foregroundStyle(Color.accentColor)

            switch store.updateOutcome {
            case .some(.success(.available(let manifest))):
                Text(L("updateAvailableTitle")).font(.headline)
                Text(String(format: L("updateAvailableBody"), manifest.version))
                HStack {
                    Button(L("updateLater")) { dismiss() }
                    Button(L("updateRestart")) {
                        dismiss()
                        Task { await model.installUpdate(manifest) }
                    }
                    .buttonStyle(.borderedProminent)
                }

            case .some(.success(.upToDate)):
                Text(L("updateUpToDate")).font(.headline)
                Button(L("dialogOk")) { dismiss() }
                    .keyboardShortcut(.defaultAction)
                    .buttonStyle(.borderedProminent)

            case .some(.failure(let error)):
                Text(String(format: L("updateCheckFailed"), Self.message(error)))
                    .font(.callout)
                    .foregroundStyle(.secondary)
                    .multilineTextAlignment(.center)
                    .textSelection(.enabled)
                    .frame(maxWidth: 320)
                    .fixedSize(horizontal: false, vertical: true)
                HStack {
                    Button(L("cancel")) { dismiss() }
                    Button(L("menuCheckUpdates")) {
                        model.checkForUpdates(manual: true)
                    }
                }

            case .none:
                EmptyView()
            }
        }
        .padding(28)
        .frame(width: 360)
    }

    private static func message(_ error: Error) -> String {
        (error as? LocalizedError)?.errorDescription ?? error.localizedDescription
    }
}
