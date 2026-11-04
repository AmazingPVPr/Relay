import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Dialogs
import Relay
import "qrc:/Relay/components/Theme.js" as Theme
Pane {
    id: board
    property var service: null
    property bool editing: false
    function open() { visible=true }
    function close() { visible=false; if(service)service.cancelDraft() }
    padding: 14
    background: Rectangle {color:Theme.surface}
    FileDialog {
        id: choose; title:"Choose a sound"; nameFilters:["Audio files (*.wav *.mp3 *.ogg *.flac *.m4a *.opus)"]
        onAccepted: { board.editing=true; soundName.text=decodeURIComponent(String(selectedFile)).split("/").pop().replace(/\.[^.]+$/,""); startTime.value=0; endTime.value=0; board.service.prepareSound(selectedFile) }
    }
    Connections {target:board.service;function onChanged(){if(board.editing && endTime.value===0 && board.service.draftDuration>0)endTime.value=Math.min(30,board.service.draftDuration)}}
    contentItem:ColumnLayout {
        spacing:10
        RowLayout {
            Layout.fillWidth:true
            Text {Layout.fillWidth:true;text:board.editing?"Add a sound":"Custom Sounds";color:Theme.text;font.pixelSize:20;font.weight:Font.DemiBold}
            ActionButton {text:"Add sound";visible:!board.editing;onClicked:choose.open()}
            ActionButton {text:"Stop";quiet:true;onClicked:if(board.service)board.service.stop()}
        }
        Text {Layout.fillWidth:true;text:board.service?board.service.error:"";visible:text.length>0;color:Theme.danger;wrapMode:Text.WordWrap}
        ColumnLayout {
            visible:board.editing;Layout.fillWidth:true;spacing:10
            AppField {id:soundName;Layout.fillWidth:true;placeholderText:"Sound name";maximumLength:80;Accessible.name:"Sound name"}
            Text {text:board.service&&board.service.loading?"Preparing audio…":"Trim your sound, preview it, then save.";color:Theme.secondary}
            RowLayout {
                Text {text:"Start (seconds)";color:Theme.secondary}
                SpinBox {palette.base:Theme.raised;palette.text:Theme.text;palette.button:Theme.raised;palette.buttonText:Theme.secondary;palette.highlight:Theme.accent;id:startTime;from:0;to:board.service?Math.max(0,Math.ceil(board.service.draftDuration*100)-5):0;editable:true;validator:DoubleValidator {bottom:0;top:18000;decimals:2;locale:"C"} stepSize:10;property real seconds:value/100;textFromValue:function(v){return (v/100).toFixed(2)};valueFromText:function(t){return Math.round(Number(t)*100)}}
                Text {text:"End (seconds)";color:Theme.secondary}
                SpinBox {palette.base:Theme.raised;palette.text:Theme.text;palette.button:Theme.raised;palette.buttonText:Theme.secondary;palette.highlight:Theme.accent;id:endTime;from:0;to:board.service?Math.floor(board.service.draftDuration*100):0;editable:true;validator:DoubleValidator {bottom:0;top:18000;decimals:2;locale:"C"} stepSize:10;property real seconds:value/100;textFromValue:function(v){return (v/100).toFixed(2)};valueFromText:function(t){return Math.round(Number(t)*100)}}
            }
            RangeSlider {
                id:trimRange
                implicitHeight:28;leftPadding:8;rightPadding:8
                background:Rectangle {x:trimRange.leftPadding;y:trimRange.availableHeight/2;height:4;width:trimRange.availableWidth;radius:2;color:Theme.border
                    Rectangle {x:trimRange.first.visualPosition*parent.width;width:(trimRange.second.visualPosition-trimRange.first.visualPosition)*parent.width;height:4;radius:2;color:Theme.accent}}
                first.handle:Rectangle {x:trimRange.leftPadding+trimRange.first.visualPosition*(trimRange.availableWidth-width);y:(trimRange.availableHeight-height)/2;width:16;height:16;radius:8;color:Theme.text;border.color:Theme.accent;border.width:2}
                second.handle:Rectangle {x:trimRange.leftPadding+trimRange.second.visualPosition*(trimRange.availableWidth-width);y:(trimRange.availableHeight-height)/2;width:16;height:16;radius:8;color:Theme.text;border.color:Theme.accent;border.width:2}
                Layout.fillWidth:true;from:0;to:Math.max(.01,board.service?board.service.draftDuration:0);first.value:startTime.seconds;second.value:endTime.seconds;first.onMoved:startTime.value=Math.round(first.value*100);second.onMoved:endTime.value=Math.round(second.value*100)}
            Text {text:Math.max(0,endTime.seconds-startTime.seconds).toFixed(2)+" seconds selected · maximum 30 seconds";color:Theme.muted}
            RowLayout {
                ActionButton {text:"Preview trim";enabled:board.service&&!board.service.loading;onClicked:board.service.previewDraft(startTime.seconds,endTime.seconds)}
                Item {Layout.fillWidth:true}
                ActionButton {text:"Cancel";quiet:true;onClicked:{board.service.cancelDraft();board.editing=false}}
                ActionButton {text:"Save Sound";enabled:board.service&&!board.service.loading&&soundName.text.trim().length>0&&endTime.seconds-startTime.seconds>=.05&&endTime.seconds-startTime.seconds<=30;onClicked:if(board.service.saveDraft(soundName.text,startTime.seconds,endTime.seconds))board.editing=false}
            }
        }
        HardScrollView {
            visible:!board.editing;Layout.fillWidth:true;Layout.fillHeight:true
            ColumnLayout {
                width:parent.width;spacing:8
                Repeater {model:board.service?board.service.sounds:[]
                    delegate:RowLayout {
                        required property var modelData
                        Layout.fillWidth:true
                        ActionButton {text:modelData.name;Layout.fillWidth:true;enabled:board.service.voiceAvailable&&!board.service.loading;onClicked:board.service.play(modelData.id)}
                        ActionButton {iconName:"headphones";hint:"Preview only for you";quiet:true;onClicked:board.service.preview(modelData.id)}
                        ActionButton {iconName:"close";hint:"Remove sound";quiet:true;destructive:true;onClicked:board.service.removeSound(modelData.id)}
                    }
                }
                Text {Layout.fillWidth:true;visible:!board.service||board.service.sounds.length===0;text:"Your saved sounds will appear here. Add a sound to get started.";color:Theme.muted;wrapMode:Text.WordWrap}
            }
        }
        Item {visible:board.editing;Layout.fillHeight:true}
        Text {Layout.fillWidth:true;text:"Preview plays only for you. Join voice and unmute to play for everyone.";color:Theme.secondary;wrapMode:Text.WordWrap}
        RowLayout {Layout.fillWidth:true;Text {text:"Volume";color:Theme.secondary} AppSlider {Layout.fillWidth:true;from:0;to:1;value:board.service?board.service.volume:.5;onMoved:if(board.service)board.service.volume=value}}
    }
}
