import AppKit
import CryptoKit
import Foundation

/// PodLens updater — the Taskly scheme.
///
/// Trust model: HTTPS only protects transport. The update feed is a pair of
/// GitHub release assets, `update-manifest.json` and `manifest.sig` (a raw
/// 64-byte Ed25519 signature over the exact manifest bytes). The manifest is
/// trusted only after signature verification against the embedded public
/// key; the downloaded archive must then match the signed SHA-256. The
/// swapped-in bundle's Info.plist version must equal the manifest version
/// before the old bundle is replaced (with rollback on failure).
///
/// Developer builds run from `.rivet/stage` or the build directory are
/// detected and update install is refused — they would replace nothing
/// useful. A missing embedded key keeps update checks honest ("developer
/// build") instead of pretending to check.
struct UpdateService {
    // Base64 of the raw 32-byte Ed25519 public key (scripts/update-keys.sh).
    static let publicKeyBase64 = "eWk+MVBTRUkcf3O4HSKek5yZ+cEv1oyx4QEErjC4opA="
    static let releasesAPI = "https://api.github.com/repos/turinglambdaai/podlens/releases/latest"
    static let releasesPage = "https://github.com/turinglambdaai/podlens/releases/latest"
    static let throttleSeconds: TimeInterval = 4 * 60 * 60

    private let apiURL: URL
    private let sessionConfiguration: URLSessionConfiguration
    private let keyBase64: String?
    private let currentVersion: String

    init(apiURL: URL = URL(string: UpdateService.releasesAPI)!,
         sessionConfiguration: URLSessionConfiguration = .ephemeral,
         keyBase64: String? = UpdateService.publicKeyBase64,
         currentVersion: String? = nil) {
        self.apiURL = apiURL
        var cfg = sessionConfiguration
        cfg.timeoutIntervalForRequest = 30
        cfg.timeoutIntervalForResource = 600
        self.sessionConfiguration = cfg
        self.keyBase64 = keyBase64
        self.currentVersion = currentVersion
            ?? Bundle.main.object(forInfoDictionaryKey: "CFBundleShortVersionString") as? String
            ?? "0"
    }

    enum UpdateError: LocalizedError {
        case notInstalled
        case badPublicKey
        case manifestMissing
        case signatureInvalid
        case checksumMismatch
        case versionMismatch(expected: String, got: String)
        case installFailed(String)

        var errorDescription: String? {
            switch self {
            case .notInstalled: return L("updateNotInstalled")
            case .badPublicKey: return L("updateBadKey")
            case .manifestMissing: return L("updateManifestMissing")
            case .signatureInvalid: return L("updateSignatureInvalid")
            case .checksumMismatch: return L("updateChecksumMismatch")
            case .versionMismatch(let e, let g): return L("updateVersionMismatch") + " (\(e) / \(g))"
            case .installFailed(let m): return L("updateInstallFailed") + ": \(m)"
            }
        }
    }

    enum CheckResult: Equatable {
        case upToDate
        case available(Manifest)
    }

    struct Manifest: Decodable, Equatable {
        struct PlatformArtifact: Decodable, Equatable {
            let url: URL
            let sha256: String
            let size: Int
        }
        let version: String
        let notesUrl: String
        let platforms: [String: PlatformArtifact]
    }

    // MARK: check

    func check(manual: Bool) async -> Result<CheckResult, Error> {
        if !manual {
            let last = UserDefaults.standard.double(forKey: "last-update-check")
            if Date().timeIntervalSince1970 - last < Self.throttleSeconds {
                return .success(.upToDate)
            }
        }
        UserDefaults.standard.set(Date().timeIntervalSince1970, forKey: "last-update-check")

        do {
            if keyBase64 == nil || keyBase64!.isEmpty {
                return .failure(UpdateError.badPublicKey)
            }
            guard installedBundleURL != nil else { return .failure(UpdateError.notInstalled) }

            var req = URLRequest(url: apiURL)
            req.setValue("application/vnd.github+json", forHTTPHeaderField: "Accept")
            let (data, response) = try await session(from: sessionConfiguration).data(for: req)
            guard let http = response as? HTTPURLResponse, http.statusCode == 200 else {
                throw UpdateError.manifestMissing
            }
            struct Release: Decodable { struct Asset: Decodable { let name: String; let browser_download_url: URL }
                let assets: [Asset] }
            let release = try JSONDecoder().decode(Release.self, from: data)
            guard
                let manifestAsset = release.assets.first(where: { $0.name == "update-manifest.json" }),
                let sigAsset = release.assets.first(where: { $0.name == "manifest.sig" })
            else { throw UpdateError.manifestMissing }

            async let manifestBytes = downloadBytes(manifestAsset.browser_download_url)
            async let sigBytes = downloadBytes(sigAsset.browser_download_url)
            let (m, s) = try await (manifestBytes, sigBytes)

            try verifyManifest(m, signature: s, keyBase64: keyBase64!)
            let manifest = try JSONDecoder().decode(Manifest.self, from: m)

            if isVersion(manifest.version, greaterThan: currentVersion) {
                return .success(.available(manifest))
            }
            return .success(.upToDate)
        } catch {
            return .failure(error)
        }
    }

    private func session(from configuration: URLSessionConfiguration) -> URLSession {
        URLSession(configuration: configuration)
    }

    private func downloadBytes(_ url: URL) async throws -> Data {
        let (data, response) = try await session(from: sessionConfiguration).data(from: url)
        guard let http = response as? HTTPURLResponse, http.statusCode == 200 else {
            throw UpdateError.manifestMissing
        }
        return data
    }

    private func verifyManifest(_ manifestBytes: Data, signature: Data, keyBase64: String) throws {
        guard signature.count == 64 else { throw UpdateError.signatureInvalid }
        guard let keyRaw = Data(base64Encoded: keyBase64), keyRaw.count == 32 else {
            throw UpdateError.badPublicKey
        }
        guard let publicKey = try? Curve25519.Signing.PublicKey(rawRepresentation: keyRaw) else {
            throw UpdateError.badPublicKey
        }
        guard publicKey.isValidSignature(signature, for: manifestBytes) else {
            throw UpdateError.signatureInvalid
        }
    }

    private func isVersion(_ a: String, greaterThan b: String) -> Bool {
        func segments(_ v: String) -> [Int] {
            v.split(separator: ".").map { Int($0) ?? 0 }
        }
        var lhs = segments(a), rhs = segments(b)
        let n = max(lhs.count, rhs.count)
        lhs += Array(repeating: 0, count: n - lhs.count)
        rhs += Array(repeating: 0, count: n - rhs.count)
        for i in 0..<n where lhs[i] != rhs[i] {
            return lhs[i] > rhs[i]
        }
        return false
    }

    // MARK: install (macOS: zip → ditto → atomic swap with rollback)

    var installedBundleURL: URL? {
        let path = Bundle.main.bundlePath
        guard path.hasSuffix(".app") else { return nil }
        guard path.hasPrefix("/Applications/") || (path.hasPrefix("/Users/") && path.contains("/Applications/")) else {
            return nil
        }
        return URL(fileURLWithPath: path)
    }

    var canInstall: Bool {
        installedBundleURL != nil
    }

    @MainActor
    func downloadAndInstall(_ manifest: Manifest) async throws {
        guard let bundleURL = installedBundleURL else { throw UpdateError.notInstalled }
        guard let artifact = manifest.platforms["macos"] else { throw UpdateError.manifestMissing }

        let workDir = FileManager.default.temporaryDirectory
            .appendingPathComponent("podlens-update-\(UUID().uuidString)")
        try FileManager.default.createDirectory(at: workDir, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: workDir) }

        let zipURL = workDir.appendingPathComponent("podlens-update.zip")
        let (bytes, response) = try await session(from: sessionConfiguration).bytes(from: artifact.url)
        guard let http = response as? HTTPURLResponse, http.statusCode == 200 else {
            throw UpdateError.checksumMismatch
        }
        guard FileManager.default.createFile(atPath: zipURL.path, contents: nil) else {
            throw UpdateError.installFailed("cannot create temp file")
        }
        let handle = try FileHandle(forWritingTo: zipURL)
        defer { try? handle.close() }
        var hasher = SHA256()
        var buffer = Data()
        buffer.reserveCapacity(1 << 20)
        for try await byte in bytes {
            buffer.append(byte)
            if buffer.count >= 1 << 20 {
                hasher.update(data: buffer)
                try handle.write(contentsOf: buffer)
                buffer.removeAll(keepingCapacity: true)
            }
        }
        if !buffer.isEmpty {
            hasher.update(data: buffer)
            try handle.write(contentsOf: buffer)
        }
        let digest = hasher.finalize().map { String(format: "%02x", $0) }.joined()
        guard digest == artifact.sha256.lowercased() else { throw UpdateError.checksumMismatch }

        let unpacked = workDir.appendingPathComponent("unpacked", isDirectory: true)
        try FileManager.default.createDirectory(at: unpacked, withIntermediateDirectories: true)
        let ditto = Process()
        ditto.executableURL = URL(fileURLWithPath: "/usr/bin/ditto")
        ditto.arguments = ["-x", "-k", zipURL.path, unpacked.path]
        try ditto.run()
        ditto.waitUntilExit()
        guard ditto.terminationStatus == 0 else { throw UpdateError.checksumMismatch }

        let newBundle = unpacked.appendingPathComponent("PodLens.app")
        let newInfo = Bundle(url: newBundle)?.object(forInfoDictionaryKey: "CFBundleShortVersionString") as? String
        guard let newVersion = newInfo, newVersion == manifest.version else {
            throw UpdateError.versionMismatch(expected: manifest.version, got: newInfo ?? "missing")
        }

        // Atomic swap with rollback, executed after this process exits.
        let bundle = bundleURL.path
        let newBundlePath = newBundle.path
        let q: (String) -> String = { "'" + $0.replacingOccurrences(of: "'", with: "'\\''") + "'" }
        let script = """
        #!/bin/bash
        sleep 1
        rm -rf \(q(bundle)).old
        if mv \(q(bundle)) \(q(bundle)).old; then
          if mv \(q(newBundlePath)) \(q(bundle)); then
            open \(q(bundle))
            rm -rf \(q(bundle)).old
            exit 0
          fi
          mv \(q(bundle)).old \(q(bundle))
        fi
        exit 1
        """
        let swapURL = workDir.appendingPathComponent("swap.sh")
        try script.write(to: swapURL, atomically: true, encoding: .utf8)
        try FileManager.default.setAttributes([.posixPermissions: 0o755], ofItemAtPath: swapURL.path)

        let bash = Process()
        bash.executableURL = URL(fileURLWithPath: "/bin/bash")
        bash.arguments = [swapURL.path]
        try bash.run()
        NSApplication.shared.terminate(nil)
    }
}
