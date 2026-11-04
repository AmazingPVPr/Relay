import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

RowLayout {
    id: field
    readonly property var value: {
        const d = /^(\d{4})-(\d{2})-(\d{2})$/.exec(dateField.text)
        const t = /^(\d{2}):(\d{2})$/.exec(timeField.text)
        if (!d || !t) return null
        const year = Number(d[1]), month = Number(d[2]), day = Number(d[3]), hour = Number(t[1]), minute = Number(t[2])
        if (year < 1970 || month < 1 || month > 12 || day < 1 || day > 31 || hour > 23 || minute > 59) return null
        const date = new Date(year, month - 1, day, hour, minute)
        return date.getFullYear() === year && date.getMonth() === month - 1 && date.getDate() === day && date.getHours() === hour && date.getMinutes() === minute ? date : null
    }
    function setDate(value) { const date = new Date(value); dateField.text = Qt.formatDate(date,"yyyy-MM-dd"); timeField.text = Qt.formatTime(date,"HH:mm") }
    TextField { id: dateField; Layout.fillWidth: true; placeholderText: "YYYY-MM-DD"; maximumLength: 10; selectByMouse: true; Accessible.name: "Date, year-month-day" }
    TextField { id: timeField; Layout.preferredWidth: 105; placeholderText: "HH:MM"; maximumLength: 5; selectByMouse: true; Accessible.name: "Time, 24-hour hours and minutes" }
}
