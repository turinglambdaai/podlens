import SwiftUI
import RivetEmbedding
import RivetRuntime
import RivetSystem

@main
struct PodLensApp: App {
    @StateObject private var model = AppModel()

    var body: some Scene {
        WindowGroup {
            ContentView()
                .environmentObject(model)
                .frame(minWidth: 980, minHeight: 620)
                .task { model.start() }
        }
        .commands {
            CommandGroup(after: .newItem) {
                Button(L("menuCheckUpdates")) { model.checkForUpdates(manual: true) }
                    .keyboardShortcut("u")
                Divider()
                Button(L("menuRefreshAll")) { model.store.refreshAll() }
                    .keyboardShortcut("r", modifiers: [.command])
                Button(L("menuSettings")) { model.showSettings = true }
                    .keyboardShortcut(",")
            }
        }
        .windowStyle(.automatic)
    }
}

@MainActor
final class AppModel: ObservableObject {
    @Published var ready = false
    @Published var bootStatus = L("booting") {
        didSet { store.bootStatus = bootStatus }
    }
    @Published var showSettings = false

    var pendingUpdateManifest: UpdateService.Manifest? {
        if case .success(.available(let m)) = store.updateOutcome {
            return m
        }
        return nil
    }

    var showSettingsBinding: Binding<Bool> {
        Binding(get: { [weak self] in self?.showSettings ?? false },
                set: { [weak self] in self?.showSettings = $0 })
    }

    let updateService = UpdateService()
    let store = PodLensStore()

    private var backend: EmbeddedRacketBackend?

    func start() {
        guard backend == nil else { return }
        do {
            let config = try Self.runtimeConfiguration()
            let backend = EmbeddedRacketBackend(configuration: config)
            self.backend = backend

            Task.detached { [backend, weak self] in
                do {
                    try backend.start(onEvent: { name, value in
                        guard let event = try? RivetEvent.decode(name: name, value: value) else { return }
                        Task { @MainActor in
                            self?.store.handleEvent(event)
                        }
                    })
                    let api = RivetAPI(client: backend.client)
                    await MainActor.run {
                        self?.store.bind(api: api)
                        self?.ready = true
                        self?.bootStatus = L("ready")
                        self?.store.loadFeeds()
                        self?.silentUpdateCheck()
                    }
                } catch {
                    await MainActor.run {
                        self?.ready = false
                        self?.bootStatus = L("backendError") + " \(error)"
                    }
                }
            }
        } catch {
            bootStatus = "config: \(error)"
        }
    }

    func checkForUpdates(manual: Bool) {
        Task {
            let outcome = await updateService.check(manual: manual)
            await MainActor.run { store.updateOutcome = outcome }
        }
    }

    func installUpdate(_ manifest: UpdateService.Manifest) async {
        do {
            try await updateService.downloadAndInstall(manifest)
        } catch {
            store.statusLine = L("updateInstallFailed") + " \(error)"
        }
    }

    private func silentUpdateCheck() {
        Task {
            let outcome = await updateService.check(manual: false)
            if case .success(.available) = outcome {
                await MainActor.run { store.updateOutcome = outcome }
            }
        }
    }

    private static func runtimeConfiguration() throws -> EmbeddedRacketConfiguration {
        let executable = Bundle.main.executableURL
            ?? URL(fileURLWithPath: CommandLine.arguments[0]).standardizedFileURL

        // Packaged apps keep Racket data in Contents/Resources. `raco rivet
        // dev` runs the staged executable directly, where runtime/res live
        // next to the executable.
        let roots = [
            Bundle.main.resourceURL,
            executable.deletingLastPathComponent()
        ].compactMap { $0 }

        for root in roots {
            let runtime = root.appendingPathComponent("runtime", isDirectory: true)
            let core = root.appendingPathComponent("res/core.zo")
            let required = [
                runtime.appendingPathComponent("petite.boot"),
                runtime.appendingPathComponent("scheme.boot"),
                runtime.appendingPathComponent("racket.boot"),
                core
            ]
            if required.allSatisfy({ FileManager.default.fileExists(atPath: $0.path) }) {
                return EmbeddedRacketConfiguration(
                    executable: executable,
                    petiteBoot: required[0],
                    schemeBoot: required[1],
                    racketBoot: required[2],
                    core: core,
                    moduleName: RivetGeneratedConfig.moduleName,
                    entryName: RivetGeneratedConfig.entryName
                )
            }
        }

        throw HostError.missingRuntimeLayout(
            roots.map(\.path).joined(separator: ", ")
        )
    }
}

enum HostError: Error, CustomStringConvertible {
    case missingRuntimeLayout(String)

    var description: String {
        switch self {
        case .missingRuntimeLayout(let roots):
            return "missing Rivet runtime/res layout under: \(roots)"
        }
    }
}
