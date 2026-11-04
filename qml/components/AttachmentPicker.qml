import QtQuick
import QtQuick.Dialogs

FileDialog {
    id: picker
    property var service: typeof uploads !== "undefined" ? uploads : null
    title: "Choose files to attach"
    fileMode: FileDialog.OpenFiles
    nameFilters: ["All files (*)"]
    onAccepted: {
        if (picker.service && !picker.service.busy)
            picker.service.addFiles(selectedFiles)
    }
}
