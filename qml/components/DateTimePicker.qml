import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "Theme.js" as Theme

Dialog {
    id: picker
    parent: Overlay.overlay
    width: Math.min(420, parent.width - 32)
    x: (parent.width - width) / 2
    y: Math.max(16, (parent.height - height) / 2)
    padding: 20; modal: true; focus: true
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
    signal timestampSelected(string text)
    readonly property var styles: [
        {name: "Date and time", code: "f", preview: "MMM d, yyyy h:mm AP"},
        {name: "Full date and time", code: "F", preview: "dddd, MMMM d, yyyy h:mm AP"},
        {name: "Short date", code: "d", preview: "yyyy-MM-dd"},
        {name: "Long date", code: "D", preview: "MMMM d, yyyy"},
        {name: "Time", code: "t", preview: "h:mm AP"},
        {name: "Time with seconds", code: "T", preview: "h:mm:ss AP"},
        {name: "Relative time", code: "R", preview: "MMM d, yyyy h:mm AP"}
    ]
    readonly property var selectedDate: {
        const dateMatch = /^(\d{4})-(\d{2})-(\d{2})$/.exec(dateField.text)
        const timeMatch = /^(\d{2}):(\d{2})$/.exec(timeField.text)
        if (!dateMatch || !timeMatch) return null
        const year = Number(dateMatch[1]), month = Number(dateMatch[2]), day = Number(dateMatch[3])
        const hour = Number(timeMatch[1]), minute = Number(timeMatch[2])
        if (year < 1970 || month < 1 || month > 12 || day < 1 || day > 31 || hour > 23 || minute > 59) return null
        const date = new Date(year, month - 1, day, hour, minute)
        return date.getFullYear() === year && date.getMonth() === month - 1 && date.getDate() === day && date.getHours() === hour && date.getMinutes() === minute ? date : null
    }
    onOpened: {
        const now = new Date()
        dateField.text = Qt.formatDate(now, "yyyy-MM-dd")
        timeField.text = Qt.formatTime(now, "HH:mm")
        dateField.forceActiveFocus()
    }
    background: Rectangle { radius: 8; color: Theme.surface; border.color: Theme.border }
    contentItem: ColumnLayout {
        spacing: 16
        RowLayout {
            Layout.fillWidth: true
            Text { Layout.fillWidth: true; text: "Insert date & time"; color: Theme.text; font.pixelSize: Math.round(18 * preferences.fontScale); font.weight: Font.DemiBold }
            ActionButton { iconName: "close"; quiet: true; hint: "Close"; onClicked: picker.close() }
        }
        Text { Layout.fillWidth: true; text: "Choose your local time. Everyone sees it in their own time zone."; color: Theme.secondary; font.pixelSize: Math.round(12 * preferences.fontScale); wrapMode: Text.WordWrap }
        RowLayout {
            Layout.fillWidth: true; spacing: 12
            ColumnLayout {
                Layout.fillWidth: true; spacing: 6
                Text { text: "Date"; color: Theme.secondary; font.pixelSize: Math.round(12 * preferences.fontScale) }
                TextField { id: dateField; Layout.fillWidth: true; placeholderText: "YYYY-MM-DD"; maximumLength: 10; selectByMouse: true; color: Theme.text; Accessible.name: "Date, year-month-day" }
            }
            ColumnLayout {
                Layout.preferredWidth: 100; spacing: 6
                Text { text: "Time"; color: Theme.secondary; font.pixelSize: Math.round(12 * preferences.fontScale) }
                TextField { id: timeField; Layout.fillWidth: true; placeholderText: "HH:MM"; maximumLength: 5; selectByMouse: true; color: Theme.text; Accessible.name: "Time, 24-hour hours and minutes" }
            }
        }
        ComboBox { id: style; Layout.fillWidth: true; model: picker.styles; textRole: "name" }
        Rectangle {
            Layout.fillWidth: true
            implicitHeight: preview.implicitHeight + 24
            radius: 5; color: Theme.canvas
            Text { id: preview; anchors.left: parent.left; anchors.right: parent.right; anchors.margins: 12; y: 12; text: picker.selectedDate ? Qt.formatDateTime(picker.selectedDate, picker.styles[style.currentIndex].preview) + (picker.styles[style.currentIndex].code === "R" ? " · shown relatively" : "") : "Enter a valid date and time."; color: picker.selectedDate ? Theme.text : Theme.warning; font.pixelSize: Math.round(13 * preferences.fontScale); wrapMode: Text.WordWrap }
        }
        ActionButton {
            Layout.alignment: Qt.AlignRight
            text: "Insert"; primary: true; enabled: picker.selectedDate !== null
            onClicked: { picker.timestampSelected("<t:" + Math.floor(picker.selectedDate.getTime() / 1000) + ":" + picker.styles[style.currentIndex].code + "> "); picker.close() }
        }
    }
}
