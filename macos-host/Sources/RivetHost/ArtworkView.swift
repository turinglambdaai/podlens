import AppKit
import SwiftUI

/// Memory cache for show/episode artwork; disk caching rides on the shared
/// URLCache (sized once at startup in PodLensApp). Keys are artwork URLs.
@MainActor
enum ArtworkStore {
    private static let cache = NSCache<NSString, NSImage>()

    static func image(for url: String) async -> NSImage? {
        if let hit = cache.object(forKey: url as NSString) { return hit }
        guard let remote = URL(string: url), !url.trimmingCharacters(in: .whitespaces).isEmpty else {
            return nil
        }
        do {
            let (data, _) = try await URLSession.shared.data(from: remote)
            guard let image = NSImage(data: data) else { return nil }
            cache.setObject(image, forKey: url as NSString)
            return image
        } catch {
            return nil
        }
    }
}

/// Rounded cover art. Feeds without a cover get a quiet accent placeholder
/// so list and grid layout never depends on the network.
struct ArtworkView: View {
    let url: String
    var cornerRadius: CGFloat = 6

    var body: some View {
        RoundedRectangle(cornerRadius: cornerRadius)
            .fill(Color.accentColor.opacity(url.isEmpty ? 0.25 : 0.12))
            .overlay {
                if url.isEmpty {
                    Image(systemName: "waveform")
                        .foregroundStyle(Color.accentColor.opacity(0.7))
                } else {
                    RemoteArtwork(url: url)
                }
            }
            .clipShape(RoundedRectangle(cornerRadius: cornerRadius))
    }
}

private struct RemoteArtwork: View {
    let url: String
    @State private var image: NSImage?

    var body: some View {
        ZStack {
            if let image {
                Image(nsImage: image)
                    .resizable()
                    .scaledToFill()
            } else {
                ProgressView().controlSize(.small)
            }
        }
        .task(id: url) {
            image = await ArtworkStore.image(for: url)
        }
    }
}
