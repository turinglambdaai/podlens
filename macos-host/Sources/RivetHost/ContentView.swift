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
        .sheet(isPresented: $showAddFeed) {
            AddFeedSheet()
        }
        .sheet(isPresented: Binding(get: { model.store.showDiscover },
                                    set: { model.store.showDiscover = $0 })) {
            DiscoverSheet()
        }
            .sheet(isPresented: model.showSettingsBinding) {
                SettingsSheet()
            }
            .sheet(item: updateDialog) { manifest in
                UpdateSheet(manifest: manifest)
            }
    }

    private var main: some View {
        NavigationSplitView {
            SidebarView(showAddFeed: $showAddFeed)
        } content: {
            EpisodeListView()
        } detail: {
            DetailView()
        }
    }

    private var updateDialog: Binding<UpdateService.Manifest?> {
        let store = model.store
        return Binding(
            get: {
                var found: UpdateService.Manifest?
                if case .success(.available(let m)) = store.updateOutcome {
                    found = m
                }
                return found
            },
            set: { _ in store.updateOutcome = nil })
    }
}

extension UpdateService.Manifest: Identifiable {
    var id: String { version }
}

// MARK: discover sheet

struct DiscoverSheet: View {
    @EnvironmentObject private var model: AppModel
    @Environment(\.dismiss) private var dismiss
    @State private var query = ""

    var body: some View {
        VStack(spacing: 12) {
            Text(L("discoverTitle")).font(.headline)
            Text(L("discoverNote"))
                .font(.caption)
                .foregroundStyle(.secondary)
                .multilineTextAlignment(.center)

            TextField(L("searchPlaceholder"), text: $query)
                .textFieldStyle(.roundedBorder)
                .submitLabel(.search)
                .onSubmit { model.store.searchDirectory(query) }
                .padding(.horizontal, 4)

            if model.store.isSearching {
                ProgressView(L("searching"))
                    .frame(maxWidth: .infinity, alignment: .center)
            } else if model.store.searchError != nil {
                Text(L("searchFailed") + " \(model.store.searchError!)")
                    .font(.caption)
                    .foregroundStyle(.red)
            } else if !model.store.searchResults.isEmpty {
                ScrollView {
                    LazyVStack(alignment: .leading, spacing: 8) {
                        ForEach(model.store.searchResults) { entry in
                            CatalogRow(entry: entry)
                        }
                    }
                    .padding(.horizontal, 4)
                }
            } else {
                ScrollView {
                    LazyVStack(alignment: .leading, spacing: 18) {
                        ForEach(categories, id: \.self) { category in
                            let items = model.store.catalog.filter { $0.category == category }
                            if !items.isEmpty {
                                VStack(alignment: .leading, spacing: 8) {
                                    Text(categoryLabel(category))
                                        .font(.subheadline.bold())
                                        .foregroundStyle(Color.accentColor)
                                    ForEach(items) { entry in
                                        CatalogRow(entry: entry)
                                    }
                                }
                            }
                        }
                    }
                    .padding(.horizontal, 4)
                }
            }

            Button(L("cancel")) { dismiss() }
                .keyboardShortcut(.cancelAction)
        }
        .padding(20)
        .frame(minWidth: 560, minHeight: 520)
        .task { model.store.loadCatalog() }
    }

    private var categories: [String] {
        var seen: [String] = []
        for entry in model.store.catalog where !seen.contains(entry.category) {
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

struct CatalogRow: View {
    @EnvironmentObject private var model: AppModel
    let entry: PodLensStore.CatalogEntry

    var body: some View {
        HStack(alignment: .top, spacing: 10) {
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
                    model.store.addFromCatalog(entry)
                }
                .buttonStyle(.borderedProminent)
                .controlSize(.small)
            }
        }
        .padding(10)
        .background(Color.primary.opacity(0.03))
        .clipShape(RoundedRectangle(cornerRadius: 8))
    }
}

// MARK: sidebar

struct SidebarView: View {
    @EnvironmentObject private var model: AppModel
    @Binding var showAddFeed: Bool

    var body: some View {
        List(selection: Binding(get: { model.store.selectedFeed },
                                set: { if let f = $0 { model.store.selectFeed(f) } })) {
            if model.store.feeds.isEmpty {
                Text(L("emptyFeeds"))
                    .font(.callout)
                    .foregroundStyle(.secondary)
                    .multilineTextAlignment(.center)
                    .frame(maxWidth: .infinity, maxHeight: .infinity)
            }
            ForEach(model.store.feeds) { feed in
                HStack(spacing: 10) {
                    RoundedRectangle(cornerRadius: 6)
                        .fill(Color.accentColor.opacity(0.25))
                        .frame(width: 34, height: 34)
                        .overlay(Text(String(feed.title.prefix(1))).bold())
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
                .tag(feed)
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
                    model.store.showDiscover = true
                } label: {
                    Image(systemName: "sparkles.rectangle.stack")
                }
                .buttonStyle(.borderless)
                .help(L("discover"))

                Button {
                    model.store.refreshAll()
                } label: {
                    Image(systemName: "arrow.clockwise")
                }
                .buttonStyle(.borderless)
                .help(L("menuRefreshAll"))

                if let feed = model.store.selectedFeed {
                    Button {
                        model.store.removeFeed(feed)
                    } label: {
                        Image(systemName: "minus.circle")
                    }
                    .buttonStyle(.borderless)
                    .help(L("removeFeed"))
                }

                Spacer()
                Text(model.store.statusLine)
                    .font(.caption2)
                    .foregroundStyle(.secondary)
                    .lineLimit(1)
            }
            .padding(8)
        }
    }
}

// MARK: add feed

struct AddFeedSheet: View {
    @EnvironmentObject private var model: AppModel
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
                    model.store.addFeed(url: url)
                    dismiss()
                }
                .keyboardShortcut(.defaultAction)
                .buttonStyle(.borderedProminent)
            }
        }
        .padding(24)
    }
}

// MARK: episodes

struct EpisodeListView: View {
    @EnvironmentObject private var model: AppModel

    var body: some View {
        List(selection: Binding(get: { model.store.selectedEpisode },
                                set: { if let e = $0 { model.store.selectEpisode(e) } })) {
            ForEach(model.store.episodes) { episode in
                EpisodeRow(episode: episode)
                    .tag(episode)
                    .contextMenu {
                        Button(episode.done ? L("markUnplayed") : L("markPlayed")) {
                            model.store.markEpisode(episode, done: !episode.done)
                        }
                    }
            }
        }
        .overlay {
            if model.store.episodes.isEmpty {
                Text(L("selectEpisode")).foregroundStyle(.secondary)
            }
        }
        .navigationTitle(model.store.selectedFeed?.title ?? L("episodes"))
    }
}

struct EpisodeRow: View {
    let episode: Episode

    var body: some View {
        VStack(alignment: .leading, spacing: 4) {
            Text(episode.title).lineLimit(2)
            HStack(spacing: 6) {
                Text(episode.pubDisplay).font(.caption).foregroundStyle(.secondary)
                if episode.done {
                    Label(L("done"), systemImage: "checkmark.circle.fill")
                        .font(.caption2).foregroundStyle(.green)
                }
                if episode.transcriptStatus == "done" {
                    badge("text.bubble", L("transcriptTab"))
                }
                if episode.transcriptStatus == "running" {
                    badge("doc.text.magnifyingglass", L("transcribing"))
                }
                if episode.hasTranslation {
                    badge("globe", "译")
                }
                if episode.summaryStatus == "done" {
                    badge("sparkles", L("summaryTab"))
                }
            }
        }
        .padding(.vertical, 2)
    }

    private func badge(_ icon: String, _ text: String) -> some View {
        Label(text, systemImage: icon)
            .font(.caption2)
            .foregroundStyle(Color.accentColor)
    }
}

// MARK: detail (player + transcript + summary)

struct DetailView: View {
    @EnvironmentObject private var model: AppModel
    @State private var tab = 0

    var body: some View {
        VStack(spacing: 0) {
            if let episode = model.store.selectedEpisode {
                EpisodeHeader(episode: episode)
                Picker("", selection: $tab) {
                    Text(L("transcriptTab")).tag(0)
                    Text(L("summaryTab")).tag(1)
                }
                .pickerStyle(.segmented)
                .padding([.horizontal, .top], 12)

                if tab == 0 {
                    TranscriptView()
                } else {
                    SummaryView()
                }
                PlayerBar()
            } else {
                Text(L("selectEpisode"))
                    .foregroundStyle(.secondary)
                    .frame(maxWidth: .infinity, maxHeight: .infinity)
            }
        }
    }
}

struct EpisodeHeader: View {
    @EnvironmentObject private var model: AppModel
    let episode: Episode

    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            Text(episode.title).font(.title3.bold())
            if !episode.description.isEmpty {
                Text(episode.description)
                    .font(.callout)
                    .foregroundStyle(.secondary)
                    .lineLimit(3)
            }
            HStack(spacing: 8) {
                Button {
                    model.store.startJob("pipeline", episode: episode)
                } label: {
                    Label(L("pipeline"), systemImage: "wand.and.stars")
                }
                .buttonStyle(.borderedProminent)
                .help(estimateHint)

                actionButton(L("download"), "arrow.down.circle") {
                    model.store.startJob("download", episode: episode)
                }
                actionButton(L("transcribe"), "doc.text.below.ecg") {
                    model.store.startJob("transcribe", episode: episode)
                }
                actionButton(L("translate"), "globe") {
                    model.store.startJob("translate", episode: episode)
                }
                actionButton(L("summarize"), "sparkles") {
                    model.store.startJob("summarize", episode: episode)
                }
                Button(episode.done ? L("markUnplayed") : L("markPlayed")) {
                    model.store.markEpisode(episode, done: !episode.done)
                }
                if episode.downloaded {
                    actionButton(L("removeAudio"), "trash") {
                        model.store.withEpisode(episode.id) { api in
                            _ = try? await api.episode_remove_audio(id: episode.id)
                        }
                    }
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
        .frame(maxWidth: .infinity, alignment: .leading)
        .padding(12)
    }

    private var activeJob: PodLensStore.Job? {
        model.store.jobs.values.first { $0.status == "running" && $0.pct > 0 }
    }

    private var estimateLine: String? {
        guard let est = model.store.estimate else { return nil }
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
        guard let est = model.store.estimate else { return L("pipeline") }
        if est.summarized { return L("pipelineNothingTodo") }
        if est.transcriptKnown, let chars = est.chars {
            return String(format: L("pipelineCostKnown"), chars)
        }
        return L("pipelineCostUnknown")
    }

    private func actionButton(_ title: String, _ icon: String, _ action: @escaping () -> Void) -> some View {
        Button(action: action) {
            Label(title, systemImage: icon)
        }
    }
}

struct TranscriptView: View {
    @EnvironmentObject private var model: AppModel

    var body: some View {
        ScrollViewReader { proxy in
            List {
                if model.store.segments.isEmpty {
                    Text(L("noTranscript"))
                        .foregroundStyle(.secondary)
                }
                ForEach(model.store.segments) { seg in
                    let active = model.store.currentSegmentIndex() == seg.id
                    VStack(alignment: .leading, spacing: 3) {
                        Text(formatTime(seg.start))
                            .font(.caption2.monospacedDigit())
                            .foregroundStyle(.tertiary)
                        Text(display(seg))
                            .font(.system(size: active ? 15 : 14))
                            .foregroundStyle(active ? Color.primary : Color.primary.opacity(0.75))
                            .background(active ? Color.accentColor.opacity(0.12) : .clear)
                        if model.store.translationMode != 2 && !seg.translation.isEmpty {
                            Text(seg.translation)
                                .font(.system(size: 14))
                                .foregroundStyle(active ? Color.accentColor : .secondary)
                        }
                        if model.store.translationMode == 2 {
                            Text(seg.text)
                                .font(.system(size: 14))
                                .foregroundStyle(active ? Color.accentColor : .secondary)
                                .opacity(model.store.translationMode == 2 ? 0 : 1)
                        }
                    }
                    .id(seg.id)
                    .contentShape(Rectangle())
                    .onTapGesture {
                        model.store.seek(to: seg.start)
                    }
                }
            }
            .onChange(of: model.store.currentTime) { _, _ in
                // follow only when the active sentence changes, so the
                // half-second ticks don't drag the list under the reader
                if let idx = model.store.segmentToFollow() {
                    proxy.scrollTo(idx, anchor: .center)
                }
            }
            .safeAreaInset(edge: .top) {
                HStack {
                    Picker("", selection: Binding(get: { model.store.translationMode },
                                                  set: { model.store.translationMode = $0 })) {
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
        model.store.translationMode == 1 && !seg.translation.isEmpty ? seg.translation : seg.text
    }

    private func formatTime(_ t: Double) -> String {
        let m = Int(t) / 60, s = Int(t) % 60
        return String(format: "%02d:%02d", m, s)
    }
}

struct SummaryView: View {
    @EnvironmentObject private var model: AppModel

    var body: some View {
        ScrollView {
            if let s = model.store.summary {
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

struct PlayerBar: View {
    @EnvironmentObject private var model: AppModel

    var body: some View {
        HStack(spacing: 12) {
            sleepMenu

            if !model.store.chapters.isEmpty {
                Button {
                    model.store.skipChapter(-1)
                } label: {
                    Image(systemName: "backward.end.fill")
                }
                .buttonStyle(.borderless)
                .help(model.store.currentChapter()?.title ?? L("chapterPrev"))

                Button {
                    model.store.skipChapter(1)
                } label: {
                    Image(systemName: "forward.end.fill")
                }
                .buttonStyle(.borderless)
                .help(L("chapterNext"))
            }

            Button {
                model.store.skip(by: -15)
            } label: {
                Image(systemName: "gobackward.15")
            }
            .buttonStyle(.borderless)
            .keyboardShortcut(.leftArrow, modifiers: [.command])
            .help(L("skipBack"))

            Button {
                model.store.togglePlay()
            } label: {
                Image(systemName: model.store.playing ? "pause.fill" : "play.fill")
                    .font(.title3)
            }
            .buttonStyle(.borderless)
            .keyboardShortcut(.space, modifiers: [])
            .help(model.store.playing ? L("pause") : L("play"))

            Button {
                model.store.skip(by: 30)
            } label: {
                Image(systemName: "goforward.30")
            }
            .buttonStyle(.borderless)
            .keyboardShortcut(.rightArrow, modifiers: [.command])
            .help(L("skipForward"))

            Text(formatTime(model.store.currentTime))
                .font(.caption.monospacedDigit())
            Slider(value: Binding(get: { model.store.currentTime },
                                  set: { model.store.seek(to: $0) }),
                   in: 0...(max(model.store.duration, 1)))
            Text(formatTime(model.store.duration))
                .font(.caption.monospacedDigit())

            Picker("", selection: Binding(get: { model.store.rate },
                                          set: { model.store.setRate($0) })) {
                ForEach(PodLensStore.playbackRates, id: \.self) { r in
                    Text("\(r, specifier: "%g")×").tag(r)
                }
            }
            .frame(width: 72)
        }
        .padding(12)
        .background(.bar)
    }

    @ViewBuilder
    private var sleepMenu: some View {
        let remaining = model.store.sleepMinutesRemaining
        Menu {
            Button(L("sleepOff")) { model.store.cancelSleepTimer() }
            ForEach([5, 10, 15, 30, 45, 60, 90], id: \.self) { m in
                Button(String(format: L("sleepMinutes"), m)) {
                    model.store.startSleepTimer(minutes: m)
                }
            }
        } label: {
            Image(systemName: remaining == nil ? "moon.zzz" : "moon.zzz.fill")
        }
        .menuStyle(.borderlessButton)
        .fixedSize()
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
    @EnvironmentObject private var model: AppModel
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

            HStack {
                if saved { Text(L("settingsSaved")).foregroundStyle(.green) }
                Button(L("cancel")) { dismiss() }
                Button(L("settingsSaved")) { save() }
                    .buttonStyle(.borderedProminent)
            }
        }
        .padding(24)
        .task { load() }
    }

    private func load() {
        Task {
            guard let api = model.store.apiForSettings else { return }
            let list = (try? await api.settings_list()) ?? []
            rows = list.map { ($0[0], $0[1], $0[2]) }
        }
    }

    private func save() {
        Task {
            guard let api = model.store.apiForSettings else { return }
            for row in rows {
                _ = try? await api.settings_set(key: row.0, value: row.1)
            }
            saved = true
        }
    }
}

// MARK: update sheet

struct UpdateSheet: View {
    @EnvironmentObject private var model: AppModel
    @Environment(\.dismiss) private var dismiss
    let manifest: UpdateService.Manifest

    var body: some View {
        VStack(spacing: 16) {
            Image(systemName: "arrow.down.circle.fill")
                .font(.system(size: 40))
                .foregroundStyle(Color.accentColor)
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
        }
        .padding(28)
    }
}
