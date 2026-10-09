import AppKit
import CryptoKit
import Foundation

/// PodLens updater — the family signed-wrapper scheme (docs/UPDATE.md).
///
/// Trust model: HTTPS only protects transport. The update feed is a single
/// GitHub release asset, `update-manifest.json`: a self-contained wrapper
/// (schema + base64 payload + Ed25519 signature block, the same
/// rivet/distribution format the backend verifies). The payload is the
/// inner manifest; the signature is trusted only after verification against
/// the embedded public key, and the downloaded archive must then match the
/// signed SHA-256. The swapped-in bundle's Info.plist version must equal
/// the manifest version before the old bundle is replaced (with rollback
/// on failure).
///
/// Developer builds run from `.rivet/stage` or the build directory are
/// detected and update install is refused — they would replace nothing
/// useful. A missing embedded key keeps update checks honest ("developer
/// build") instead of pretending to check.
struct UpdateService {
    // Base64 of the raw 32-byte Ed25519 public key (scripts/update-keys.sh).
    static let publicKeyBase64 = "yBOlLqQHWs7P5CMVwHTh+uqR3b8fA9K6K5Iwkt4csD0="

    /// The manifest must carry this key id — rotation ships a build that
    /// trusts the next key before releases stop being signed with this one.
    static let expectedKeyID = "podlens-2026-10"

    static let manifestURL = URL(string:
        "https://github.com/turinglambdaai/podlens/releases/latest/download/update-manifest.json")!
    static let releasesAPI = "https://api.github.com/repos/turinglambdaai/podlens/releases/latest"
    static let releasesPage = "https://github.com/turinglambdaai/podlens/releases/latest"
    static let throttleSeconds: TimeInterval = 4 * 60 * 60

    private let sessionConfiguration: URLSessionConfiguration
    private let keyBase64: String?
    private let currentVersion: String

    /// Feed URL; injectable only for future URLProtocol test stubs —
    /// production callers use the default (the moving "latest" location).
    init(feedURL: URL = UpdateService.manifestURL,
         sessionConfiguration: URLSessionConfiguration = .ephemeral,
         keyBase64: String? = UpdateService.publicKeyBase64,
         currentVersion: String? = nil) {
        let cfg = sessionConfiguration
        cfg.timeoutIntervalForRequest = 30
        cfg.timeoutIntervalForResource = 600
        self.sessionConfiguration = cfg
        self.keyBase64 = keyBase64
        self.currentVersion = currentVersion
            ?? Bundle.main.object(forInfoDictionaryKey: "CFBundleShortVersionString") as? String
            ?? "0"
        self.feedURL = feedURL
    }

    private let feedURL: URL

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

    /// The signed wrapper: the inner manifest travels base64-encoded so the
    /// Ed25519 signature covers the exact bytes every client verifies
    /// (rivet/distribution's signed-wrapper format).
    struct SignedWrapper: Decodable {
        struct SignatureBlock: Decodable {
            let algorithm: String
            let keyId: String
            let value: String
        }
        let schema: Int
        let payload: String
        let signature: SignatureBlock
    }

    struct Manifest: Decodable, Equatable {
        struct Artifact: Decodable, Equatable {
            let platform: String
            let architecture: String
            let url: URL
            let sha256: String
            let size: Int
        }
        let version: String
        let artifacts: [Artifact]

        /// The release feed carries one artifact per platform × architecture;
        /// each built host targets exactly one architecture, so the match is
        /// compile-time ("x64" on Intel builds, "arm64" on Apple silicon).
        static let hostArchitecture: String = {
            #if arch(x86_64)
            return "x64"
            #elseif arch(arm64)
            return "arm64"
            #else
            #error("unsupported macOS architecture")
            #endif
        }()

        func artifact(forPlatform platform: String) -> Artifact? {
            artifacts.first { $0.platform == platform
                && $0.architecture == Self.hostArchitecture }
        }
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

            let (wrapperBytes, response) = try await session(from: sessionConfiguration)
                .data(from: feedURL)
            guard let http = response as? HTTPURLResponse, http.statusCode == 200 else {
                throw UpdateError.manifestMissing
            }
            let manifest = try Self.verifyManifest(wrapperBytes, keyBase64: keyBase64!)

            if Self.isVersion(manifest.version, greaterThan: currentVersion) {
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

    /// Verifies the signed wrapper (schema, key id, Ed25519 over the exact
    /// payload bytes) and decodes the inner manifest.
    static func verifyManifest(_ wrapperBytes: Data, keyBase64: String) throws -> Manifest {
        let decoder = JSONDecoder()
        decoder.keyDecodingStrategy = .convertFromSnakeCase
        let wrapper: SignedWrapper
        do {
            wrapper = try decoder.decode(SignedWrapper.self, from: wrapperBytes)
        } catch {
            throw UpdateError.manifestMissing
        }
        guard wrapper.schema == 1 else { throw UpdateError.manifestMissing }
        guard wrapper.signature.algorithm == "ed25519",
              wrapper.signature.keyId == expectedKeyID else {
            throw UpdateError.signatureInvalid
        }
        guard let payload = Data(base64Encoded: wrapper.payload),
              let signature = Data(base64Encoded: wrapper.signature.value) else {
            throw UpdateError.signatureInvalid
        }
        guard signature.count == 64 else { throw UpdateError.signatureInvalid }
        guard let keyRaw = Data(base64Encoded: keyBase64), keyRaw.count == 32 else {
            throw UpdateError.badPublicKey
        }
        guard let publicKey = try? Curve25519.Signing.PublicKey(rawRepresentation: keyRaw) else {
            throw UpdateError.badPublicKey
        }
        guard publicKey.isValidSignature(signature, for: payload) else {
            throw UpdateError.signatureInvalid
        }
        do {
            return try decoder.decode(Manifest.self, from: payload)
        } catch {
            throw UpdateError.manifestMissing
        }
    }

    private func isVersion(_ a: String, greaterThan b: String) -> Bool {
        Self.isVersion(a, greaterThan: b)
    }

    static func isVersion(_ a: String, greaterThan b: String) -> Bool {
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
        guard let artifact = manifest.artifact(forPlatform: "macos") else {
            throw UpdateError.manifestMissing
        }

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
