import QtQuick
import QtQuick.Controls
import "Theme.js" as Theme

TextEdit {
    id: root
    property string rawText: ""
    property var metadata: ({})
    signal userRequested(string userId)
    readonly property var referenceService: typeof messageReferences === "undefined" ? null : messageReferences
    readonly property var referenceMetadata: {
        if (referenceService) {
            const revision = referenceService.revision
            return referenceService.resolve(rawText, String(metadata.guildId || ""))
        }
        return {mentionChannels: metadata.mentionChannels || [], guildId: metadata.guildId || ""}
    }
    text: chatText.render(rawText, {mentionChannels: referenceMetadata.mentionChannels, guildId: referenceMetadata.guildId, mentionUsers: metadata.mentionUsers || [], mentionRoles: metadata.mentionRoles || [], underlineLinks: preferences.underlineLinks, highContrast: preferences.highContrast, language: preferences.language, timeFormat: preferences.timeFormat})
    textFormat: TextEdit.RichText
    wrapMode: TextEdit.Wrap
    readOnly: true
    selectByMouse: true
    selectionColor: Theme.selected
    selectedTextColor: Theme.text
    color: preferences.highContrast ? "#ffffff" : Theme.text
    visible: rawText.length > 0
    onLinkActivated: link => {
        if (/^relay-user:[0-9]{1,20}$/.test(link)) root.userRequested(link.substring(11))
        else if (/^relay-channel:[0-9]{1,20}$/.test(link) || /^relay-guild:[0-9]{1,20}:(customize|browse)$/.test(link)) {
            if (referenceService) referenceService.activate(link)
        } else discord.openLink(link)
    }
    HoverHandler { cursorShape: root.hoveredLink.length ? Qt.PointingHandCursor : Qt.IBeamCursor }
    ToolTip.visible: hoveredLink.length > 0 && !hoveredLink.startsWith("relay-user:")
    ToolTip.text: hoveredLink.startsWith("relay-channel:") ? "Open channel" : hoveredLink.startsWith("relay-guild:") ? (hoveredLink.endsWith(":customize") ? "Channels & Roles" : "Browse Channels") : hoveredLink
    ToolTip.delay: 600
}
