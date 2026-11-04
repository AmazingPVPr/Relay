import QtQuick
import QtQuick.Controls

ScrollView {
    id: view
    rightPadding: 16
    clip: true
    contentWidth: availableWidth
    function clampScrolling() {
        if (contentItem && contentItem.boundsBehavior !== undefined) {
            contentItem.boundsBehavior = Flickable.StopAtBounds
            contentItem.boundsMovement = Flickable.StopAtBounds
        }
    }
    Component.onCompleted: clampScrolling()
    onContentItemChanged: Qt.callLater(clampScrolling)
    ScrollBar.vertical: ScrollBar { width: 6 }
    property ScrollBoost wheelBooster: ScrollBoost { parent: view; flickable: view.contentItem; visible: view.contentItem && view.contentItem.boundsBehavior !== undefined }
}
