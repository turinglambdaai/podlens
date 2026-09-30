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
                        Text("\(feed.episodeCount)")
                            .font(.caption)
                            .foregroundStyle(.secondary)
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
                EpisodeRow(episode: episode).tag(episode)
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
            HStack(spacing: 8) {
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
                if episode.downloaded {
                    actionButton(L("removeAudio"), "trash") {
                        model.store.withEpisode(episode.id) { api in
                            _ = try? await api.episode_remove_audio(id: episode.id)
                        }
                    }
                }
                if let job = activeJob {
                    ProgressView(value: Double(job.pct), total: 100) {
                        Text("\(L("transcribing")) \(job.pct)%")
                            .font(.caption2)
                    }
                    .frame(width: 180)
                }
            }
        }
        .frame(maxWidth: .infinity, alignment: .leading)
        .padding(12)
    }

    private var activeJob: PodLensStore.Job? {
        model.store.jobs.values.first { $0.status == "running" && $0.pct > 0 }
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
            .onChange(of: model.store.currentTime) { _ in
                if let idx = model.store.currentSegmentIndex() {
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
            Button {
                model.store.togglePlay()
            } label: {
                Image(systemName: model.store.playing ? "pause.fill" : "play.fill")
                    .font(.title3)
            }
            .buttonStyle(.borderless)

            Text(formatTime(model.store.currentTime))
                .font(.caption.monospacedDigit())
            Slider(value: Binding(get: { model.store.currentTime },
                                  set: { model.store.seek(to: $0) }),
                   in: 0...(max(model.store.duration, 1)))
            Text(formatTime(model.store.duration))
                .font(.caption.monospacedDigit())

            Picker("", selection: Binding(get: { model.store.rate },
                                          set: { model.store.setRate($0) })) {
                ForEach([1.0, 1.25, 1.5, 1.75, 2.0], id: \.self) { r in
                    Text("\(r, specifier: "%.2g")×").tag(r)
                }
            }
            .frame(width: 80)
        }
        .padding(12)
        .background(.bar)
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
                    Task { try? await model.installUpdate(manifest) }
                }
                .buttonStyle(.borderedProminent)
            }
        }
        .padding(28)
    }
}
