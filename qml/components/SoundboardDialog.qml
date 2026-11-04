import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "Theme.js" as Theme

Dialog {
    id: dialog
    parent: Overlay.overlay
    width: Math.min(620, parent.width - 32)
    height: Math.min(590, parent.height - 48)
    x: (parent.width-width)/2; y: (parent.height-height)/2
    padding: 0
    modal: false
    property var customService: null
    property string category: "all"
    property var collapsed: ({})
    readonly property string selectedGuildName: { for(let i=0;i<discord.guilds.count;i++){const guild=discord.guilds.get(i);if(guild.recordId===soundboard.guildId)return guild.name} return "Server Sounds" }
    readonly property var sectionRows: {
        const filter = sounds => sounds.filter(sound => !query.text.trim().length || String(sound.name).toLocaleLowerCase().indexOf(query.text.trim().toLocaleLowerCase()) !== -1)
        const sections = [
            {key:"favorites",title:"Favorites",icon:"star",sounds:filter(soundboard.favorites)},
            {key:"frequent",title:"Frequently Used",icon:"clock",sounds:filter(soundboard.frequent)},
            {key:"server",title:selectedGuildName,icon:"voice",sounds:filter(soundboard.sounds.filter(sound => !!sound.guildId))},
            {key:"default",title:"Discord Sounds",icon:"members",sounds:filter(soundboard.sounds.filter(sound => !sound.guildId))}
        ]
        const rows=[]
        for(const section of sections){
            if(category!=="all"&&category!==section.key)continue
            if(section.key==="server"&&!soundboard.guildId)continue
            if(query.text.length&&!section.sounds.length)continue
            rows.push({kind:"header",key:section.key,title:section.title,icon:section.icon})
            if(collapsed[section.key])continue
            if(!section.sounds.length)rows.push({kind:"empty",key:section.key,title:section.key==="favorites"?"Right-click a sound to add it to Favorites.":section.key==="frequent"?"Sounds you play in voice will appear here.":soundboard.busy?"Loading sounds…":"No sounds available"})
            for(let i=0;i<section.sounds.length;i+=3)rows.push({kind:"sounds",key:section.key,items:section.sounds.slice(i,i+3)})
        }
        return rows
    }
    property var selectedSound: ({})
    signal customRequested()
    function openFor(guildId) { category = "all"; query.clear(); soundboard.load(guildId); open() }
    function toggleSection(key) { const state=Object.assign({},collapsed);state[key]=!state[key];collapsed=state }
    function selectCustom() { addons.setEnabled("org.relay.soundboard",true); customService=addons.service("org.relay.soundboard"); category="custom" }
    function openSoundMenu(sound) { selectedSound=sound;soundOptions.popup() }
    onClosed: { soundboard.stopPreview(); if(customService)customService.stop() }
    background: Rectangle { radius: 8; color: Theme.surface; border.color: Theme.border }
    contentItem: ColumnLayout {
        spacing: 0
        RowLayout {
            Layout.fillWidth: true; Layout.margins: 12; spacing: 8
            AppField { id: query; Layout.fillWidth: true; placeholderText: "Find the perfect sound"; Accessible.name: "Search soundboard"; leftPadding: 32; AppIcon { x: 9; anchors.verticalCenter: parent.verticalCenter; width: 16; height: 16; name: "search" } }
            ActionButton { id: volumeButton; iconName: preferences.soundboardVolume > 0 ? "voice" : "volumeOff"; hint: "Soundboard listening volume"; quiet: true; implicitWidth: 28; padding: 4; onClicked: volumePopup.open() }
            ActionButton { iconName: "close"; hint: "Close soundboard"; quiet: true; implicitWidth: 26; padding: 4; onClicked: dialog.close() }
        }
        RowLayout {
            Layout.fillWidth: true; Layout.leftMargin: 12; Layout.rightMargin: 12
            ActionButton { text: "Server Sounds"; quiet: dialog.category === "custom"; onClicked: dialog.category="all" }
            ActionButton { text: "Custom Sounds"; quiet: dialog.category !== "custom"; onClicked: dialog.selectCustom() }
        }
        Loader {
            Layout.fillWidth: true; Layout.fillHeight: true; visible: dialog.category === "custom"
            active: visible && !!dialog.customService
            source: active ? "qrc:/relay/addons/org.relay.soundboard/Board.qml" : ""
            onLoaded: item.service=dialog.customService
        }
        Rectangle { Layout.fillWidth: true; implicitHeight: 1; color: Theme.border }
        Text { Layout.fillWidth: true; Layout.leftMargin: 12; Layout.rightMargin: 12; Layout.topMargin: visible ? 8 : 0; text: soundboard.error; visible: text.length > 0; wrapMode: Text.WordWrap; color: Theme.danger; font.pixelSize: Math.round(12*preferences.fontScale) }
        Text { Layout.fillWidth: true; Layout.leftMargin: 12; Layout.rightMargin: 12; Layout.topMargin: visible ? 8 : 0; visible: !soundboard.canPlay; text: "Join voice to play sounds for everyone. Use Preview to listen only for yourself."; wrapMode: Text.WordWrap; color: Theme.muted; font.pixelSize: Math.round(11*preferences.fontScale) }
        RowLayout {
            Layout.fillWidth: true; Layout.fillHeight: true; spacing: 0; visible: dialog.category !== "custom"
            Rectangle {
                Layout.fillHeight: true; implicitWidth: 48; color: Theme.sidebar
                ColumnLayout {
                    anchors.fill: parent; anchors.topMargin: 8; anchors.bottomMargin: 8; spacing: 5
                    Button {
                        Layout.alignment: Qt.AlignHCenter; implicitWidth: 34; implicitHeight: 34; padding: 6; hoverEnabled: true
                        Accessible.name: "Favorite sounds"
                        background: Rectangle { radius: 6; color: dialog.category === "favorites" || parent.hovered ? Theme.selected : "transparent" }
                        contentItem: Text { text: "★"; font.pixelSize: 23; color: Theme.secondary; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter }
                        onClicked: dialog.category = dialog.category === "favorites" ? "all" : "favorites"
                        ToolTip.visible: hovered; ToolTip.text: "Favorites"; ToolTip.delay: 450
                    }
                    Button {
                        Layout.alignment: Qt.AlignHCenter; implicitWidth: 34; implicitHeight: 34; padding: 6; hoverEnabled: true
                        Accessible.name: "Frequently used sounds"
                        background: Rectangle { radius: 6; color: dialog.category === "frequent" || parent.hovered ? Theme.selected : "transparent" }
                        contentItem: Text { text: "◷"; font.pixelSize: 25; color: Theme.secondary; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter }
                        onClicked: dialog.category = dialog.category === "frequent" ? "all" : "frequent"
                        ToolTip.visible: hovered; ToolTip.text: "Frequently Used"; ToolTip.delay: 450
                    }
                    ListView {
                        id: guilds; Layout.fillWidth: true; Layout.fillHeight: true
                        model: discord.guilds; clip: true; reuseItems: true; spacing: 7
                        boundsBehavior: Flickable.StopAtBounds; boundsMovement: Flickable.StopAtBounds
                        delegate: Button {
                            id: guildButton; required property var model
                            width: guilds.width; height: 36; padding: 6; hoverEnabled: true
                            Accessible.name: "Soundboard: " + model.name
                            background: Rectangle { anchors.horizontalCenter: parent.horizontalCenter; width: 34; radius: 8; color: model.recordId === soundboard.guildId && dialog.category === "all" || parent.hovered ? Theme.selected : "transparent" }
                            contentItem: Item {
                                RoundedImage { id: icon; anchors.centerIn: parent; width: 28; height: 28; radius: 14; source: guildButton.model.iconUrl || "" }
                                Text { visible: !icon.ready; anchors.centerIn: parent; text: String(guildButton.model.name||"?").substring(0,2).toUpperCase(); textFormat: Text.PlainText; color: Theme.text; font.pixelSize: Math.round(10*preferences.fontScale); font.weight: Font.Bold }
                            }
                            onClicked: { dialog.category = "all"; soundboard.load(model.recordId) }
                            ToolTip.visible: hovered; ToolTip.text: model.name; ToolTip.delay: 450
                        }
                        ScrollBoost { flickable: guilds }
                    }
                    ActionButton { Layout.alignment: Qt.AlignHCenter; iconName: "members"; hint: "Discord Sounds"; quiet: dialog.category !== "default"; implicitWidth: 34; implicitHeight: 34; padding: 6; onClicked: dialog.category = "default" }
                }
            }
            Item {
                Layout.fillWidth: true; Layout.fillHeight: true
                ListView {
                    id: sections; objectName: "soundboardSections"
                    anchors.fill: parent; anchors.margins: 10
                    model: dialog.sectionRows; clip: true; reuseItems: true; spacing: 5
                    boundsBehavior: Flickable.StopAtBounds; boundsMovement: Flickable.StopAtBounds
                    delegate: Item {
                        required property var modelData
                        width: sections.width
                        height: modelData.kind === "header" ? 30 : modelData.kind === "empty" ? emptyLabel.implicitHeight + 12 : Math.max(48,40*preferences.fontScale)
                        Button {
                            visible: modelData.kind === "header"; anchors.fill: parent; padding: 2; hoverEnabled: true; background: Item {}
                            contentItem: RowLayout { spacing: 6; Text { text: modelData.icon === "star" ? "★" : modelData.icon === "clock" ? "◷" : ""; color: Theme.muted; font.pixelSize: Math.round(14*preferences.fontScale) } Text { text: modelData.title || ""; textFormat: Text.PlainText; color: Theme.secondary; font.pixelSize: Math.round(12*preferences.fontScale); font.weight: Font.DemiBold } AppIcon { name: "chevron"; rotation: dialog.collapsed[modelData.key] ? -90 : 0; Layout.preferredWidth: 11; Layout.preferredHeight: 11 } Item { Layout.fillWidth: true } }
                            onClicked: dialog.toggleSection(modelData.key)
                        }
                        Text { id: emptyLabel; visible: modelData.kind === "empty"; anchors.left: parent.left; anchors.right: parent.right; anchors.verticalCenter: parent.verticalCenter; text: modelData.kind === "empty" ? modelData.title : ""; color: Theme.muted; font.pixelSize: Math.round(11*preferences.fontScale); wrapMode: Text.WordWrap }
                        RowLayout {
                            visible: modelData.kind === "sounds"; anchors.fill: parent; spacing: 7
                            Repeater {
                                model: modelData.items || []
                                delegate: Rectangle {
                                    id: tile; required property var modelData
                                    Layout.fillWidth: true; Layout.fillHeight: true; Layout.preferredWidth: sections.width/3
                                    radius: 5; color: tileHover.hovered ? Theme.selected : Theme.raised
                                    opacity: modelData.available || modelData.needsReload ? 1 : 0.45
                                    HoverHandler { id: tileHover }
                                    Button {
                                        objectName: "playSoundButton_" + tile.modelData.id
                                        anchors.fill: parent; anchors.bottomMargin: 20; padding: 4; hoverEnabled: true
                                        enabled: soundboard.canPlay && !soundboard.sending && (!!tile.modelData.available || !!tile.modelData.needsReload)
                                        Accessible.name: "Play " + tile.modelData.name + " in voice"
                                        background: Item {}
                                        contentItem: RowLayout { spacing: 5; Text { text: tile.modelData.emoji || "♪"; textFormat: Text.PlainText; color: Theme.text; font.pixelSize: Math.round(19*preferences.fontScale) } Text { Layout.fillWidth: true; text: tile.modelData.name; textFormat: Text.PlainText; elide: Text.ElideRight; color: Theme.text; font.pixelSize: Math.round(11*preferences.fontScale) } AppIcon { visible: !tile.modelData.available && !tile.modelData.needsReload; name: "lock"; Layout.preferredWidth: 14; Layout.preferredHeight: 14 } }
                                        onClicked: soundboard.activate(tile.modelData.id,false)
                                    }
                                    Item {
                                        anchors.right: parent.right; anchors.rightMargin: 3; anchors.bottom: parent.bottom; width: 52; height: 20
                                        Button { objectName: "favoriteSoundButton_" + tile.modelData.id; x: 0; y: 0; width: 24; height: 20; padding: 0; hoverEnabled: true; Accessible.name: soundboard.isFavorite(tile.modelData.id) ? "Remove favorite" : "Add favorite"; background: Item {} contentItem: Text { text: soundboard.isFavorite(tile.modelData.id) ? "★" : "☆"; color: soundboard.isFavorite(tile.modelData.id) ? Theme.warning : Theme.muted; font.pixelSize: 16; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter } onClicked: soundboard.setFavorite(tile.modelData.id,!soundboard.isFavorite(tile.modelData.id)) }
                                        ActionButton { objectName: "previewSoundButton_" + tile.modelData.id; x: 27; y: 0; width: 24; height: 20; iconName: "voice"; hint: "Preview " + tile.modelData.name + " only for you"; quiet: true; padding: 3; enabled: !!tile.modelData.available || !!tile.modelData.needsReload; onClicked: soundboard.activate(tile.modelData.id,true) }
                                    }
                                    TapHandler { acceptedButtons: Qt.RightButton; onTapped: dialog.openSoundMenu(tile.modelData) }
                                }
                            }
                            Repeater { model: Math.max(0,3-(modelData.items || []).length); delegate: Item { Layout.fillWidth: true; Layout.fillHeight: true; Layout.preferredWidth: sections.width/3 } }
                        }
                    }
                    ScrollBoost { flickable: sections }
                    ScrollBar.vertical: ScrollBar { width: 6 }
                }
                Text { anchors.centerIn: parent; width: parent.width - 30; visible: !sections.count; text: soundboard.busy ? "Loading sounds…" : "No sounds found"; color: Theme.muted; horizontalAlignment: Text.AlignHCenter; wrapMode: Text.WordWrap }
            }
        }
        Rectangle { Layout.fillWidth: true; implicitHeight: 1; color: Theme.border }
        RowLayout {
            Layout.fillWidth: true; Layout.margins: 9; spacing: 5
            ActionButton { iconName: "refresh"; hint: "Reload sounds"; quiet: true; implicitWidth: 27; padding: 4; enabled: !soundboard.busy; onClicked: soundboard.load(soundboard.guildId) }
            ActionButton { text: soundboard.previewLoading ? "Loading…" : "Stop Preview"; quiet: true; onClicked: soundboard.stopPreview() }
            Item { Layout.fillWidth: true }
            ActionButton { text: "Custom Sounds"; quiet: true; onClicked: dialog.selectCustom() }
        }
    }
    property BoundedMenu soundOptions: BoundedMenu {
        MenuItem { text: soundboard.isFavorite(dialog.selectedSound.id || "") ? "Remove from Favorites" : "Add to Favorites"; onTriggered: soundboard.setFavorite(dialog.selectedSound.id,!soundboard.isFavorite(dialog.selectedSound.id)) }
        MenuItem { text: "Preview Sound"; enabled: !!dialog.selectedSound.available || !!dialog.selectedSound.needsReload; onTriggered: soundboard.activate(dialog.selectedSound.id,true) }
        MenuItem { text: "Play in Voice"; enabled: soundboard.canPlay && !soundboard.sending && (!!dialog.selectedSound.available || !!dialog.selectedSound.needsReload); onTriggered: soundboard.activate(dialog.selectedSound.id,false) }
    }
    property Popup volumePopup: Popup {
        parent: Overlay.overlay; width: Math.min(300, parent.width - 16); padding: 15
        onAboutToShow: {
            const anchor = volumeButton.mapToItem(parent, 0, volumeButton.height)
            x = Math.max(8, Math.min(parent.width - width - 8, anchor.x - width + volumeButton.width))
            y = Math.max(8, Math.min(parent.height - height - 8, anchor.y + 6))
        }
        background: Rectangle { radius: 7; color: Theme.surface; border.color: Theme.border }
        contentItem: ColumnLayout {
            spacing: 8
            Text { Layout.fillWidth: true; text: "Soundboard listening volume"; wrapMode: Text.WordWrap; color: Theme.text; font.pixelSize: Math.round(13*preferences.fontScale); font.weight: Font.DemiBold }
            Text { Layout.fillWidth: true; text: "Server sounds from other people and your previews, only for you."; wrapMode: Text.WordWrap; color: Theme.muted; font.pixelSize: Math.round(11*preferences.fontScale) }
            RowLayout { Layout.fillWidth: true; AppSlider { objectName: "soundboardPopupListeningVolume"; Layout.fillWidth: true; from:0;to:1;stepSize:.01;value:preferences.soundboardVolume;onMoved:preferences.soundboardVolume=value;Accessible.name:"Soundboard listening volume" } NumericValue {objectName:"soundboardPopupListeningPercent";value:preferences.soundboardVolume*100;minimum:0;maximum:100;onValueEdited:value=>preferences.soundboardVolume=value/100;Accessible.name:"Soundboard listening volume percent"} }
        }
    }
}
