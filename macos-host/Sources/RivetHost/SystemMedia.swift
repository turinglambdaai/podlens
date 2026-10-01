import AppKit
import MediaPlayer

/// Bridge between the AVPlayer state in PodLensStore and the system media
/// surface: hardware media keys, the Control Center now-playing widget and
/// headphone controls all route through MPRemoteCommandCenter, and the
/// Control Center display is fed by MPNowPlayingInfoCenter. Pure host-side —
/// nothing here touches the RVT1 contract.
@MainActor
extension PodLensStore {

    func setupSystemMediaControls() {
        let center = MPRemoteCommandCenter.shared()

        center.playCommand.addTarget { [weak self] _ in
            Task { @MainActor in self?.remoteCommandPlay() }
            return .success
        }
        center.pauseCommand.addTarget { [weak self] _ in
            Task { @MainActor in self?.remoteCommandPause() }
            return .success
        }
        center.togglePlayPauseCommand.addTarget { [weak self] _ in
            Task { @MainActor in self?.togglePlay() }
            return .success
        }
        center.skipBackwardCommand.preferredIntervals = [15] as [NSNumber]
        center.skipBackwardCommand.addTarget { [weak self] event in
            guard let e = event as? MPSkipIntervalCommandEvent else { return .commandFailed }
            Task { @MainActor in self?.skip(by: -e.interval) }
            return .success
        }
        center.skipForwardCommand.preferredIntervals = [30] as [NSNumber]
        center.skipForwardCommand.addTarget { [weak self] event in
            guard let e = event as? MPSkipIntervalCommandEvent else { return .commandFailed }
            Task { @MainActor in self?.skip(by: e.interval) }
            return .success
        }
        center.changePlaybackPositionCommand.addTarget { [weak self] event in
            guard let e = event as? MPChangePlaybackPositionCommandEvent else { return .commandFailed }
            Task { @MainActor in self?.seek(to: e.positionTime) }
            return .success
        }
        center.changePlaybackRateCommand.supportedPlaybackRates =
            Self.playbackRates.map { NSNumber(value: $0) }
        center.changePlaybackRateCommand.addTarget { [weak self] event in
            guard let e = event as? MPChangePlaybackRateCommandEvent else { return .commandFailed }
            Task { @MainActor in self?.setRate(e.playbackRate) }
            return .success
        }
        // Unsupported on purpose: no queue concept yet, so track skipping
        // would be a lie. Commands left without targets stay disabled.
        center.nextTrackCommand.isEnabled = false
        center.previousTrackCommand.isEnabled = false
    }

    /// Push the current episode + transport state to Control Center. Called
    /// from the store on every state change worth reflecting.
    func updateNowPlayingInfo() {
        guard !npTitle.isEmpty else { return }
        var info: [String: Any] = [
            MPMediaItemPropertyTitle: npTitle,
            MPMediaItemPropertyArtist: npShow,
            MPNowPlayingInfoPropertyMediaType: MPNowPlayingInfoMediaType.audio.rawValue,
            MPNowPlayingInfoPropertyElapsedPlaybackTime: currentTime,
            MPNowPlayingInfoPropertyPlaybackRate: playing ? rate : 0
        ]
        if duration > 0 {
            info[MPMediaItemPropertyPlaybackDuration] = duration
        }
        if let npArtwork {
            info[MPMediaItemPropertyArtwork] = npArtwork
        }
        MPNowPlayingInfoCenter.default().nowPlayingInfo = info
    }

    /// Fetch the subscribed show's artwork once per episode so the Control
    /// Center widget carries the cover. Failure just means no artwork.
    func loadNowPlayingArtwork() {
        guard let urlString = selectedFeed?.artworkURL,
              let url = URL(string: urlString), url.scheme?.hasPrefix("http") == true
        else { return }
        Task {
            guard let (data, _) = try? await URLSession.shared.data(from: url),
                  let image = NSImage(data: data)
            else { return }
            let size = image.size
            guard size.width > 0, size.height > 0 else { return }
            let artwork = MPMediaItemArtwork(boundsSize: size) { _ in image }
            npArtwork = artwork
            updateNowPlayingInfo()
        }
    }
}
