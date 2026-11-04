import QtQuick
import QtQuick.Controls
import "Theme.js" as Theme

TextField {
    id: control
    property real value: 0
    property real minimum: 0
    property real maximum: 100
    property string unit: "%"
    property int decimals: 2
    property bool editing: false
    property string editText: ""
    signal valueEdited(real number)
    function formatted(number) {
        const fixed = Number(number).toFixed(decimals)
        return decimals > 0 ? fixed.replace(/\.?0+$/, "") : fixed
    }
    function beginEditing() {
        if (editing) return
        editText = String(value)
        editing = true
        selectAll()
    }
    function commit() {
        if (!editing) return
        const number = Number(editText.trim())
        if (editText.trim().length && Number.isFinite(number) && number >= minimum && number <= maximum) valueEdited(number)
        editing = false
    }
    implicitWidth: Math.max(64, displayMetrics.width + 16)
    implicitHeight: 30
    leftPadding: 8; rightPadding: 8; topPadding: 3; bottomPadding: 3
    horizontalAlignment: Text.AlignRight
    text: editing ? editText : formatted(value) + unit
    color: Theme.secondary
    font.pixelSize: Math.round(12 * preferences.fontScale)
    readOnly: !editing
    selectByMouse: editing
    selectionColor: Theme.selected
    selectedTextColor: Theme.text
    inputMethodHints: Qt.ImhFormattedNumbersOnly
    validator: DoubleValidator { bottom: control.minimum; top: control.maximum; decimals: 8; notation: DoubleValidator.StandardNotation; locale: "C" }
    onActiveFocusChanged: activeFocus ? beginEditing() : commit()
    onTextEdited: editText = text
    onAccepted: { commit(); focus = false }
    Keys.onEscapePressed: { editing = false; focus = false }
    background: Rectangle { radius: 4; color: control.editing ? Theme.rail : hover.hovered ? Theme.hover : "transparent"; border.width: control.editing ? 1 : 0; border.color: Theme.focus }
    HoverHandler { id: hover; cursorShape: Qt.IBeamCursor }
    TapHandler { onTapped: { control.forceActiveFocus(); control.beginEditing() } }
    TextMetrics { id: displayMetrics; font: control.font; text: control.formatted(control.value) + control.unit }
    ToolTip.visible: hover.hovered && !editing
    ToolTip.delay: 700
    ToolTip.text: "Click to enter an exact value"
}
