import QtQuick

// Report only a conversation whose latest messages are on screen after a short
// reading dwell. The backend validates the account, channel and watermark again.
Item {
    id: tracker
    required property ListView view
    required property var records
    required property var backend
    property bool active: false
    property bool following: false
    readonly property string accountId: String(backend.currentUserId || "")
    readonly property string channelId: String(backend.currentChannelId || "")
    readonly property bool eligible: active && following && view.visible && view.width > 0 && view.height > 0
                                    && view.atYEnd && !!backend.connected && !backend.demoMode
                                    && !backend.loadingMessages && !backend.hasNewerMessages
                                    && accountId.length > 0 && channelId.length > 0 && records.count > 0
    visible: false

    function schedule() {
        if (!eligible) { dwell.stop(); return }
        if (!dwell.running) dwell.start()
    }
    function reportVisible() {
        if (!eligible) return
        for (let i = records.count - 1; i >= 0; --i) {
            const id = String(typeof records.recordIdAt === "function" ? records.recordIdAt(i) : records.get(i).recordId)
            if (/^[1-9][0-9]*$/.test(id)) {
                backend.acknowledgeVisibleMessages(channelId, id)
                return
            }
        }
    }
    onEligibleChanged: schedule()
    onChannelIdChanged: { dwell.stop(); schedule() }
    onAccountIdChanged: { dwell.stop(); schedule() }
    onRecordsChanged: { dwell.stop(); schedule() }
    Component.onCompleted: schedule()
    Timer { id: dwell; interval: 150; onTriggered: tracker.reportVisible() }
    Connections {
        target: tracker.records
        function onRowsInserted() { tracker.schedule() }
        function onRowsRemoved() { tracker.schedule() }
        function onRowsMoved() { tracker.schedule() }
        function onModelReset() { dwell.stop(); tracker.schedule() }
        function onDataChanged() { tracker.schedule() }
    }
    Connections {
        target: tracker.view
        function onAtYEndChanged() { tracker.schedule() }
        function onContentYChanged() { tracker.schedule() }
        function onContentHeightChanged() { tracker.schedule() }
    }
}
