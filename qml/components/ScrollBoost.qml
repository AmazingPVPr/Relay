import QtQuick

Item {
    id: boost
    required property Flickable flickable
    property real multiplier: preferences.scrollMultiplier
    readonly property real pixelsPerStep: 160 * multiplier
    property bool smoothing: !preferences.reducedMotion
    property real progress: 0
    property real startY: 0
    property real targetY: 0
    property bool anchorPaused: false
    property bool animating: false
    property bool applying: false
    signal scrollIntent(real delta)
    signal scrolled()
    anchors.fill: parent
    function cancel() {
        animating = false
        glide.stop()
        anchorPaused = false
    }
    function pauseForAnchor() {
        const remaining = animating && flickable ? targetY - flickable.contentY : 0
        // Keep the animation clock and velocity while history changes its
        // coordinate origin. Restarting its easing here creates a slow tail.
        anchorPaused = animating && glide.running
        if (anchorPaused) glide.pause()
        animating = false
        return remaining
    }
    function resumeAfterAnchor(distance) {
        const view = flickable
        if (!view || !enabled || !visible || Math.abs(distance) < 0.5) { cancel(); return }
        const top = view.originY - view.topMargin
        const bottom = Math.max(top, view.originY + view.contentHeight + view.bottomMargin - view.height)
        if (anchorPaused && smoothing) {
            const next = Math.max(top, Math.min(bottom, view.contentY + distance))
            targetY = next
            // Retain the displayed point at the existing eased progress, even
            // when a bounded page also shortens the destination.
            if (progress < 1) startY = view.contentY - (next - view.contentY) * progress / (1 - progress)
            anchorPaused = false
            animating = true
            glide.resume()
            return
        }
        anchorPaused = false
        startY = view.contentY
        targetY = Math.max(top, Math.min(bottom, startY + distance))
        if (!smoothing) { applyPosition(targetY, distance); return }
        progress = 0
        animating = true
        glide.restart()
    }
    function applyPosition(value, direction) {
        const view = flickable
        if (!view) return
        const top = view.originY - view.topMargin
        const bottom = Math.max(top, view.originY + view.contentHeight + view.bottomMargin - view.height)
        const current = view.contentY
        const next = Math.max(top, Math.min(bottom, value))
        // Changes in estimated list bounds must never reverse the glide.
        applying = true
        view.contentY = direction > 0 ? Math.max(current, next) : Math.min(current, next)
        applying = false
    }
    onProgressChanged: if (animating) applyPosition(startY + (targetY - startY) * progress, targetY - startY)
    onSmoothingChanged: if (!smoothing) cancel()
    onVisibleChanged: if (!visible) cancel()
    onEnabledChanged: if (!enabled) cancel()
    NumberAnimation {
        id: glide
        target: boost; property: "progress"
        from: 0; to: 1; duration: 110; easing.type: Easing.OutCubic
        onFinished: {
            boost.applyPosition(boost.targetY, boost.targetY - boost.startY)
            boost.animating = false
        }
    }
    Connections {
        target: boost.flickable
        // Scrollbars, history anchors and programmatic jumps own their new
        // position. A pending wheel glide must never pull them back afterward.
        function onContentYChanged() { if (boost.animating && !boost.applying) boost.cancel() }
        function onDraggingChanged() { if (boost.flickable.dragging) boost.cancel() }
        function onFlickingChanged() { if (boost.flickable.flicking) boost.cancel() }
    }
    WheelHandler {
        id: wheel
        // ListView reparents ordinary children into its moving content item.
        // Receive wheel events on the fixed viewport, including over delegates.
        parent: boost.flickable
        target: null
        acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad
        acceptedModifiers: Qt.NoModifier
        onWheel: event => {
            const view = boost.flickable
            if (!view || (event.pixelDelta.y === 0 && event.angleDelta.y === 0)) {
                event.accepted = false
                return
            }
            // Wayland can label a physical wheel as a touchpad and synthesize
            // 15 pixel units for each 120-unit detent. Recognize that discrete
            // signature without accelerating phased or precise touchpad input.
            const angle = event.angleDelta.y
            const pixels = event.pixelDelta.y
            const discreteWheel = event.phase === Qt.NoScrollPhase
                && angle !== 0 && Math.abs(angle) % 120 === 0
                && (pixels === 0 || pixels === angle / 8)
            const touchpad = event.device && event.device.type === PointerDevice.TouchPad && !discreteWheel
            const distance = touchpad
                ? (event.pixelDelta.y !== 0 ? event.pixelDelta.y : event.angleDelta.y / 120 * 60)
                : (event.angleDelta.y !== 0 ? event.angleDelta.y / 120 * boost.pixelsPerStep : event.pixelDelta.y / 15 * boost.pixelsPerStep)
            boost.scrollIntent(-distance)
            const top = view.originY - view.topMargin
            const bottom = Math.max(top, view.originY + view.contentHeight + view.bottomMargin - view.height)
            view.cancelFlick()
            // Variable-height lists revise their estimated bounds as delegates
            // appear. A revised estimate must never reverse a wheel step.
            const current = view.contentY
            // Accumulate the full wheel distance while moving in one direction.
            // Reversing starts from the displayed position, without old momentum.
            const sameDirection = !touchpad && boost.smoothing && boost.animating && (boost.targetY - current) * -distance > 0
            const base = sameDirection ? boost.targetY : current
            const next = Math.max(top, Math.min(bottom, base - distance))
            boost.cancel()
            if (touchpad || !boost.smoothing || next === current) {
                boost.applyPosition(next, -distance)
            } else {
                boost.startY = current
                boost.targetY = next
                boost.progress = 0
                boost.animating = true
                glide.restart()
            }
            event.accepted = true
            boost.scrolled()
        }
    }
}
