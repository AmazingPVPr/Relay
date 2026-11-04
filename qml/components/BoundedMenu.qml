import QtQuick
import QtQuick.Controls
import "Theme.js" as Theme

Menu {
    id: menu
    margins: 8
    padding: 6
    width: Math.round(250 * Math.min(preferences.fontScale, 1.25))
    font.pixelSize: Math.round(13 * preferences.fontScale)
    palette.windowText: Theme.text
    palette.light: Theme.hover
    palette.midlight: Theme.selected
    palette.mid: Theme.border
    background: Rectangle { color: Theme.surface; radius: 7; border.color: Theme.border }
    contentItem: ListView {
        id: items
        implicitHeight: contentHeight
        model: menu.contentModel
        interactive: contentHeight + menu.topPadding + menu.bottomPadding > menu.height
        clip: true
        currentIndex: menu.currentIndex
        boundsBehavior: Flickable.StopAtBounds
        boundsMovement: Flickable.StopAtBounds
        ScrollBoost { flickable: items }
        ScrollIndicator.vertical: ScrollIndicator { width: 4; active: items.contentHeight > items.height }
    }
}
