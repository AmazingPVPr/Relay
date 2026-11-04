import QtQuick

// Keeps the message under the reader still while a bounded history window moves.
// Pagination is armed by input, never by the layout changes a page creates.
Item {
    id: history
    required property ListView view
    required property var records
    required property var backend
    property var wheel: null
    property bool followTail: true
    property bool suspended: false
    property bool scrollbarPressed: false
    property bool olderArmed: false
    property bool newerArmed: false
    property int lastDirection: 0
    property int fillPages: 0
    property bool anchorPending: false
    property bool restoring: false
    property string anchorId: ""
    property string stableAnchorId: ""
    property real stableAnchorOffset: 0
    property bool geometryQueued: false
    property int geometryFirstIndex: -1
    property real anchorOffset: 0
    property real remainingMotion: 0
    property real lastUserY: 0
    readonly property string channelId: String(backend.currentChannelId || "")
    signal tailRequested(bool follow)
    signal olderRequested()
    signal newerRequested()
    visible: false

    function reset() {
        restoring = true
        pager.stop()
        if (wheel) wheel.cancel()
        if (view) view.cancelFlick()
        anchorPending = false; anchorId = ""; stableAnchorId = ""; geometryQueued = false; geometryFirstIndex = -1; remainingMotion = 0
        olderArmed = false; newerArmed = false; lastDirection = 0; fillPages = 0
        lastUserY = view.contentY
        restoring = false
        Qt.callLater(history.rememberAnchor)
    }
    onChannelIdChanged: reset()
    onSuspendedChanged: if (suspended) reset()
    onFollowTailChanged: if (!followTail) Qt.callLater(history.rememberAnchor)

    function schedule() { if (!pager.running) pager.start() }
    function userScroll(delta) {
        if (geometryQueued) restoreGeometry()
        if (anchorPending) restoreAnchor()
        if (delta < 0) { lastDirection = -1; olderArmed = true; newerArmed = false; tailRequested(false) }
        else if (delta > 0) { lastDirection = 1; newerArmed = true; olderArmed = false }
        schedule()
    }
    function settleTail() {
        if (lastDirection > 0 && !anchorPending && !restoring && !backend.loadingMessages && !backend.hasNewerMessages && view.atYEnd)
            tailRequested(true)
    }
    function recordId(index) {
        return String(typeof records.recordIdAt === "function" ? records.recordIdAt(index) : records.get(index).recordId)
    }
    function locate(id) {
        if (typeof records.indexOf === "function") return records.indexOf(id)
        for (let i = 0; i < records.count; ++i) if (recordId(i) === id) return i
        return -1
    }
    function visibleAnchor(firstRemoved, lastRemoved) {
        let clipped = null, lastIndex = -1
        for (let y = 1; y < view.height; y += 8) {
            const index = view.indexAt(Math.min(5, view.width / 2), view.contentY + y)
            if (index < 0 || index === lastIndex || (index >= firstRemoved && index <= lastRemoved)) continue
            lastIndex = index
            const item = view.itemAtIndex(index)
            if (!item) continue
            const candidate = {id:recordId(index),offset:item.y-view.contentY}
            // A clipped message may still resolve a line/image above the
            // viewport. Prefer the next message whose top the reader can see.
            if (candidate.offset >= -0.5) return candidate
            if (!clipped) clipped = candidate
        }
        return clipped
    }
    function rememberAnchor() {
        if (anchorPending || restoring || followTail || suspended || !view.visible || records.count === 0) return
        const current = visibleAnchor(-1,-1)
        if (current) { stableAnchorId = current.id; stableAnchorOffset = current.offset }
    }
    function holdAnchor(id, offset) {
        anchorId = id; anchorOffset = offset
        anchorPending = true
        remainingMotion = wheel ? wheel.pauseForAnchor() : 0
        view.cancelFlick()
    }
    function captureAnchor(firstRemoved, lastRemoved) {
        if (anchorPending || restoring || followTail || suspended || !view.visible || records.count === 0) return
        const saved = locate(stableAnchorId)
        if (geometryQueued && saved >= 0 && (saved < firstRemoved || saved > lastRemoved)) {
            geometryQueued = false; geometryFirstIndex = -1
            holdAnchor(stableAnchorId,stableAnchorOffset)
            return
        }
        const current = visibleAnchor(firstRemoved,lastRemoved)
        if (current) holdAnchor(current.id,current.offset)
    }
    // Images and text can finish laying out after the page's insert signals.
    // Keep the last visible message fixed when a cached row above it changes.
    function delegateGeometryChanged(id) {
        if (anchorPending || restoring || followTail || suspended || !view.visible || !stableAnchorId.length) return
        const changed = locate(String(id)), anchor = locate(stableAnchorId)
        if (changed < 0 || anchor < 0 || changed >= anchor) return
        geometryFirstIndex = geometryFirstIndex < 0 ? changed : Math.min(geometryFirstIndex,changed)
        if (!geometryQueued) { geometryQueued = true; Qt.callLater(history.restoreGeometry) }
    }
    function restoreGeometry() {
        if (!geometryQueued) return
        geometryQueued = false
        const changed = geometryFirstIndex; geometryFirstIndex = -1
        if (anchorPending || restoring || followTail || suspended || !view.visible || !stableAnchorId.length) return
        const anchor = locate(stableAnchorId)
        if (anchor < 0 || changed >= anchor) return
        holdAnchor(stableAnchorId,stableAnchorOffset)
        queueRestore()
    }
    function queueRestore() { if (anchorPending) Qt.callLater(history.restoreAnchor) }
    function originShifted() {
        if (anchorPending || restoring || followTail || suspended || !stableAnchorId.length || !wheel || !wheel.animating || wheel.applying) return
        const index = locate(stableAnchorId), item = index >= 0 ? view.itemAtIndex(index) : null
        if (!item || Math.abs(item.y-view.contentY-stableAnchorOffset) <= 0.5) return
        holdAnchor(stableAnchorId,stableAnchorOffset)
        queueRestore()
    }
    function restoreAnchor() {
        if (!anchorPending || restoring) return
        if (suspended || followTail) { anchorPending = false; remainingMotion = 0; return }
        const index = locate(anchorId)
        if (index < 0) { anchorPending = false; remainingMotion = 0; return }
        restoring = true
        view.forceLayout()
        if (!view.itemAtIndex(index)) {
            view.positionViewAtIndex(index, ListView.Beginning)
            view.forceLayout()
        }
        for (let pass = 0; pass < 2; ++pass) {
            const item = view.itemAtIndex(index)
            if (!item) break
            const next = item.y - anchorOffset
            if (Math.abs(view.contentY - next) > 0.25) view.contentY = next
            view.forceLayout()
        }
        const motion = remainingMotion
        anchorPending = false; anchorId = ""; remainingMotion = 0
        lastUserY = view.contentY
        restoring = false
        rememberAnchor()
        if (wheel) wheel.resumeAfterAnchor(motion)
        settleTail()
        schedule()
    }
    function loadVisible() {
        if (suspended || anchorPending || restoring || !view.visible || !backend.connected || backend.demoMode || backend.loadingMessages || backend.error || records.count === 0) return
        const top = view.originY - view.topMargin
        const remaining = view.originY + view.contentHeight + view.bottomMargin - view.height - view.contentY
        const threshold = Math.max(240, view.height * 0.75)
        // A short initial page can fill the viewport, but cannot become an
        // unbounded fetch loop even if every returned page has tiny delegates.
        const fill = followTail && view.contentHeight <= view.height && fillPages < 4
        if (backend.hasMoreMessages && view.contentY - top < threshold && (olderArmed || fill)) {
            olderArmed = false
            if (fill) ++fillPages
            olderRequested()
        } else if (backend.hasNewerMessages && remaining < threshold && newerArmed) {
            newerArmed = false
            newerRequested()
        }
    }
    Timer { id: pager; interval: 60; onTriggered: history.loadVisible() }
    Connections {
        target: history.wheel
        function onScrollIntent(delta) { history.userScroll(delta) }
        function onScrolled() {
            if (!history.wheel.animating && !history.anchorPending && !history.restoring)
                history.tailRequested(history.view.atYEnd && !history.backend.hasNewerMessages)
        }
        function onAnimatingChanged() {
            if (!history.wheel.animating && !history.anchorPending && !history.restoring)
                history.tailRequested(history.view.atYEnd && !history.backend.hasNewerMessages)
        }
    }
    Connections {
        target: history.backend
        function onStateChanged() { history.settleTail(); history.schedule() }
    }
    Connections {
        target: history.records
        function onRowsAboutToBeInserted(parent, first, last) { history.captureAnchor(-1, -1) }
        function onRowsAboutToBeRemoved(parent, first, last) { history.captureAnchor(first, last) }
        function onRowsInserted() { history.queueRestore() }
        function onRowsRemoved() { history.queueRestore() }
        function onModelAboutToBeReset() { history.reset() }
    }
    Connections {
        target: history.view
        function onContentHeightChanged() { history.schedule() }
        function onOriginYChanged() { history.originShifted() }
        function onHeightChanged() { history.schedule() }
        function onContentYChanged() {
            if (!history.anchorPending && !history.restoring) {
                if (history.geometryQueued && history.wheel && history.wheel.applying)
                    history.stableAnchorOffset -= history.view.contentY-history.lastUserY
                else history.rememberAnchor()
            }
            if (!history.restoring && !history.anchorPending && (history.view.dragging || history.view.flicking || history.scrollbarPressed)) {
                history.userScroll(history.view.contentY - history.lastUserY)
            }
            history.lastUserY = history.view.contentY
            history.schedule()
        }
    }
}
