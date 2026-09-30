import SwiftUI

/// PodLens design tokens — the macOS mirror of docs/DESIGN-ROADMAP.md §5.
/// The Windows side lives in windows/Themes/Tokens.xaml. Add new tokens to
/// the roadmap table first, then to both mirrors in the same change.
enum DesignTokens {
    /// Terracotta accent shared with the site and the app icon (#C15F3C).
    static let accent = Color(red: 0xC1 / 255.0, green: 0x5F / 255.0, blue: 0x3C / 255.0)

    /// Soft highlight background (current transcript segment, selected chips).
    /// Light: #F7E5DD · Dark: #3B2A22.
    static let accentSoft = Color(nsColor: NSColor { appearance in
        if appearance.bestMatch(from: [.aqua, .darkAqua]) == .darkAqua {
            return NSColor(red: 0x3B / 255.0, green: 0x2A / 255.0, blue: 0x22 / 255.0, alpha: 1)
        }
        return NSColor(red: 0xF7 / 255.0, green: 0xE5 / 255.0, blue: 0xDD / 255.0, alpha: 1)
    })
}
