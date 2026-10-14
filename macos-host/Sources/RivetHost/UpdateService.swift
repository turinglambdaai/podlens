import Foundation
import RivetUpdate

/// Online update service (docs/UPDATE.md) — a thin product adapter over
/// rivet's `RivetUpdate` module (the family's shared host-side update
/// client since rivet 0.8.1, adopted by taskly in ba3a54e): this file only
/// contributes the product facts (feed, signing key, key id, bundle name),
/// the localized error surface, and the seams future host tests may want.
/// Signed-manifest verification, the bounded delegate-driven download, the
/// atomic swap with rollback, and the quit/relaunch handoff all live in
/// the library — the previous ~320-line hand-rolled client (CryptoKit
/// verify, AsyncBytes download, bash swap script) is gone.
///
/// Only a copy under /Applications (or ~/Applications) can self-update —
/// debug builds and random folders get the "not installed" answer.
@MainActor
public final class UpdateService {
    /// Ed25519 public key (base64, raw 32 bytes) — pair of the
    /// UPDATE_ED25519_PRIVATE_KEY GitHub secret that signs release
    /// manifests (generate: scripts/update-keys.sh). Same literal the
    /// engine embeds (app/update.rkt).
    nonisolated public static let publicKeyBase64 = "CRJhkEfo79/a7lgRkLhWSvVGviGcLNL2obvE2k7bFZI="

    /// The manifest must carry this key id — rotation ships a build that
    /// trusts the next key before releases stop being signed with this one.
    nonisolated public static let expectedKeyID = "podlens-2026-10"

    public static let manifestURL = URL(string:
        "https://github.com/turinglambdaai/podlens/releases/latest/download/update-manifest.json")!
    public static let releasesAPI = "https://api.github.com/repos/turinglambdaai/podlens/releases/latest"
    public static let releasesPage = "https://github.com/turinglambdaai/podlens/releases/latest"

    /// Silent checks run at most once per 4 h (docs/UPDATE.md triggers).
    public static let throttleSeconds: TimeInterval = 4 * 60 * 60

    public enum UpdateError: LocalizedError {
        case notInstalled
        case badPublicKey
        case manifestMissing
        case signatureInvalid
        case checksumMismatch
        case versionMismatch(expected: String, got: String)
        case installFailed(String)

        public var errorDescription: String? {
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

    public typealias Manifest = RivetUpdateManifest

    // No Equatable: nothing compares outcomes, and synthesizing a
    // conformance over the cross-module Manifest payload trips a SILGen
    // segfault in the Intel release runner's older swiftc (6.1.2).
    public enum CheckResult {
        case upToDate
        case available(Manifest)
    }

    /// nil only when the (constant) product configuration failed the
    /// library's fail-closed validation — surfaced as badPublicKey.
    private let service: RivetUpdateService?

    /// Injectable parameters exist for future URLProtocol test stubs —
    /// production callers use the defaults.
    init(feedURL: URL = UpdateService.manifestURL,
                sessionConfiguration: URLSessionConfiguration? = nil,
                keyBase64: String? = UpdateService.publicKeyBase64,
                currentVersion: String? = nil,
                bundleURL: URL? = nil) {
        let configuration = RivetUpdateConfiguration(
            feedURL: feedURL,
            publicKeyBase64: keyBase64 ?? "",
            keyID: Self.expectedKeyID,
            bundleName: "PodLens.app")
        self.service = try? RivetUpdateService(
            configuration: configuration,
            bundlePath: bundleURL?.path,
            currentVersion: currentVersion,
            sessionConfiguration: sessionConfiguration)
    }

    /// Translates library errors into this product's (localized) surface.
    nonisolated private static func map(_ error: RivetUpdateError) -> UpdateError {
        switch error {
        case .notInstalled: return .notInstalled
        case .badPublicKey: return .badPublicKey
        case .invalidConfiguration: return .badPublicKey
        case .manifestMissing: return .manifestMissing
        case .signatureInvalid: return .signatureInvalid
        case .checksumMismatch: return .checksumMismatch
        case .sizeMismatch: return .checksumMismatch
        case .versionMismatch(let expected, let got):
            return .versionMismatch(expected: expected, got: got)
        case .network(let message): return .installFailed(message)
        }
    }

    /// Removes swap work dirs older than 30 minutes — the swap script
    /// cannot reliably delete its own directory while bash is still
    /// reading it, so a later launch sweeps the leftovers.
    nonisolated public static func sweepStaleWorkDirs() {
        RivetUpdateService.cleanupStaleWorkDirectories(olderThan: 30 * 60)
    }

    // MARK: - Environment

    /// The installed bundle this copy can update in place, or nil for
    /// development copies (not under /Applications or ~/Applications).
    public var installedBundleURL: URL? { service?.installedBundleURL }

    public var canUpdate: Bool { service?.canUpdate ?? false }

    public var canInstall: Bool { canUpdate }

    // MARK: - Check

    /// Fetches the latest release manifest, verifies its Ed25519 signature
    /// and compares versions. Silent checks are throttled to one per
    /// `throttleSeconds` (UserDefaults `last-update-check`); a throttled
    /// check answers "up to date" without touching the network. Failures
    /// come back as `.failure` (presentation is the caller's job).
    public func check(manual: Bool) async -> Result<CheckResult, Error> {
        if !manual {
            let last = UserDefaults.standard.double(forKey: "last-update-check")
            if Date().timeIntervalSince1970 - last < Self.throttleSeconds {
                return .success(.upToDate)
            }
        }
        UserDefaults.standard.set(Date().timeIntervalSince1970, forKey: "last-update-check")

        guard let service else { return .failure(UpdateError.badPublicKey) }
        guard service.canUpdate else { return .failure(UpdateError.notInstalled) }
        do {
            switch try await service.check() {
            case .upToDate: return .success(.upToDate)
            case .available(let manifest): return .success(.available(manifest))
            }
        } catch let error as RivetUpdateError {
            return .failure(Self.map(error))
        } catch {
            return .failure(error)
        }
    }

    // MARK: - Download + install

    /// Downloads, verifies (sha256, then unpacked bundle version), swaps
    /// the bundle in place and relaunches. Throws before touching the
    /// running install when anything is off.
    public func downloadAndInstall(_ manifest: Manifest) async throws {
        guard let service else { throw UpdateError.badPublicKey }
        do {
            try await service.downloadAndInstall(manifest)
        } catch let error as RivetUpdateError {
            throw Self.map(error)
        }
    }

    // MARK: - Version compare (numeric per segment; 1.2 > 1.1.9)

    nonisolated public static func isVersion(_ a: String, greaterThan b: String) -> Bool {
        RivetUpdateService.isVersion(a, greaterThan: b)
    }
}
