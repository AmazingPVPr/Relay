import QtQuick

AnimatedImage {
    id: animation
    property bool playbackRequested: true
    asynchronous: true
    cache: true
    playing: playbackRequested
    // Older Qt versions stop at the GIF's finite loop count. Restart only
    // while playback is requested, retaining cached sequential-network frames.
    onPlaybackRequestedChanged: playing = playbackRequested
    onPlayingChanged: {
        if (!playing && playbackRequested && status === Image.Ready && frameCount > 1) {
            Qt.callLater(function() {
                if (animation && animation.playbackRequested && animation.status === Image.Ready) {
                    animation.currentFrame = 0
                    animation.playing = true
                }
            })
        }
    }
}
