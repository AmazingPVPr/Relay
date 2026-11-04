import QtQml
QtObject {
    required property var service
    property bool serviceRunning: service ? service.running : false
}
