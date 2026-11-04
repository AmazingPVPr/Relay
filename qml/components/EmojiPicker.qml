import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "Theme.js" as Theme

Popup {
    id: picker
    parent: Overlay.overlay
    width: Math.min(360, parent.width - 32)
    height: Math.min(425, parent.height - 32)
    padding: 14; focus: true
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
    signal emojiSelected(string text)
    readonly property var groups: ["All emoji", "Faces", "Gestures", "Hearts", "Animals", "Food", "Activities"]
    readonly property var entries: [
        ["😀","happy smile grin","Faces"], ["😃","happy smile","Faces"], ["😁","grin smile","Faces"], ["😂","laugh tears joy","Faces"], ["🤣","laugh rolling","Faces"], ["😊","blush smile happy","Faces"], ["🙂","smile","Faces"], ["😉","wink","Faces"], ["😍","love heart eyes","Faces"], ["🥰","love hearts","Faces"], ["😘","kiss love","Faces"], ["😎","cool sunglasses","Faces"], ["🤔","thinking","Faces"], ["😅","sweat nervous laugh","Faces"], ["😭","cry sad tears","Faces"], ["🥲","smile tear","Faces"], ["😢","sad cry","Faces"], ["😡","angry rage","Faces"], ["😮","surprise wow","Faces"], ["😴","sleep tired","Faces"], ["🥳","party celebrate","Faces"], ["🤯","mind blown","Faces"], ["🫡","salute","Faces"], ["🫠","melting","Faces"],
        ["👍","thumbs up yes good","Gestures"], ["👎","thumbs down no","Gestures"], ["👋","wave hello bye","Gestures"], ["👏","clap applause","Gestures"], ["🙌","hands celebration","Gestures"], ["🙏","thanks please pray","Gestures"], ["🤝","handshake agreement","Gestures"], ["💪","strong muscle","Gestures"], ["✌️","peace victory","Gestures"], ["🤞","fingers crossed luck","Gestures"], ["👌","okay perfect","Gestures"], ["🫶","heart hands love","Gestures"],
        ["❤️","red heart love","Hearts"], ["🧡","orange heart","Hearts"], ["💛","yellow heart","Hearts"], ["💚","green heart","Hearts"], ["💙","blue heart","Hearts"], ["💜","purple heart","Hearts"], ["🖤","black heart","Hearts"], ["🤍","white heart","Hearts"], ["💔","broken heart","Hearts"], ["💕","two hearts love","Hearts"], ["💖","sparkle heart","Hearts"], ["💯","hundred perfect","Hearts"],
        ["🐶","dog puppy","Animals"], ["🐱","cat kitten","Animals"], ["🐭","mouse","Animals"], ["🐰","rabbit bunny","Animals"], ["🦊","fox","Animals"], ["🐻","bear","Animals"], ["🐼","panda","Animals"], ["🐸","frog","Animals"], ["🐧","penguin","Animals"], ["🦋","butterfly","Animals"], ["🐢","turtle","Animals"], ["🌱","plant sprout","Animals"],
        ["🍕","pizza","Food"], ["🍔","burger","Food"], ["🍟","fries","Food"], ["🌮","taco","Food"], ["🍜","noodles ramen","Food"], ["🍣","sushi","Food"], ["🍎","apple fruit","Food"], ["🍓","strawberry","Food"], ["🍰","cake birthday","Food"], ["🍪","cookie","Food"], ["☕","coffee tea","Food"], ["🧋","bubble tea boba","Food"],
        ["🎉","party celebration","Activities"], ["🎊","confetti celebration","Activities"], ["🎂","birthday cake","Activities"], ["🎮","gaming controller","Activities"], ["🎵","music note","Activities"], ["🎧","headphones music","Activities"], ["⚽","soccer football","Activities"], ["🏀","basketball","Activities"], ["🏆","trophy winner","Activities"], ["🔥","fire lit","Activities"], ["✨","sparkles","Activities"], ["⭐","star","Activities"], ["🚀","rocket launch","Activities"], ["✅","check done yes","Activities"], ["❌","cross no","Activities"], ["👀","eyes looking","Activities"]
    ]
    function filteredEmoji() {
        const query = search.text.trim().toLowerCase()
        return entries.filter(entry => (!category.currentIndex || entry[2] === groups[category.currentIndex]) && (!query || entry[1].indexOf(query) !== -1 || entry[0] === query))
    }
    function showNear(item) {
        const position = item.mapToItem(parent, 0, 0)
        x = Math.max(16, Math.min(parent.width - width - 16, position.x + item.width - width))
        y = Math.max(16, Math.min(parent.height - height - 16, position.y - height - 8))
        open()
    }
    onOpened: { search.clear(); search.forceActiveFocus() }
    background: Rectangle { radius: 8; color: Theme.surface; border.color: Theme.border }
    contentItem: ColumnLayout {
        spacing: 10
        Text { text: "Emoji"; color: Theme.text; font.pixelSize: Math.round(15 * preferences.fontScale); font.weight: Font.DemiBold }
        TextField { id: search; Layout.fillWidth: true; placeholderText: "Search emoji"; selectByMouse: true; color: Theme.text; background: Rectangle { radius: 5; color: Theme.input } leftPadding: 10; rightPadding: 10; font.pixelSize: Math.round(13 * preferences.fontScale) }
        ComboBox { id: category; Layout.fillWidth: true; model: picker.groups }
        GridView {
            id: grid
            Layout.fillWidth: true; Layout.fillHeight: true
            model: picker.filteredEmoji()
            cellWidth: Math.floor(width / 6); cellHeight: 45
            clip: true
            boundsBehavior: Flickable.StopAtBounds; boundsMovement: Flickable.StopAtBounds
            delegate: Button {
                required property var modelData
                width: grid.cellWidth - 3; height: grid.cellHeight - 3
                hoverEnabled: true
                background: Rectangle { color: parent.hovered ? Theme.hover : "transparent"; radius: 5 }
                contentItem: Text { text: modelData[0]; font.family: "Noto Color Emoji"; font.pixelSize: 27; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter; textFormat: Text.PlainText }
                onClicked: { picker.emojiSelected(modelData[0]); picker.close() }
                ToolTip.visible: hovered; ToolTip.delay: 650; ToolTip.text: modelData[1]
                Accessible.name: modelData[1]
            }
            Text { anchors.centerIn: parent; visible: !grid.count; text: "No matching emoji"; color: Theme.muted; font.pixelSize: Math.round(12 * preferences.fontScale) }
            ScrollBoost { flickable: grid }
            ScrollBar.vertical: ScrollBar { width: 5 }
        }
    }
}
