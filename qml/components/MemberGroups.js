.pragma library

function snowflakeOrder(a, b) {
    a = String(a); b = String(b)
    return a.length - b.length || (a < b ? -1 : a > b ? 1 : 0)
}

function groupMembers(members) {
    const rows = [], counts = {}, labels = {}
    let online = 0, offline = 0, inVoice = 0
    for (const member of members) {
        const active = ["online", "idle", "dnd"].indexOf(member.status) !== -1
        const away = member.status === "offline" || member.status === "invisible"
        if (active) online++
        if (away) offline++
        if (member.channelId) inVoice++
        const hoisted = active && member.hoistedRoleId && member.hoistedRoleName
        const key = away ? "offline" : hoisted ? "role:" + member.hoistedRoleId : active ? "online" : "unknown"
        const label = away ? "Offline" : hoisted ? member.hoistedRoleName : active ? "Online" : "Members"
        counts[key] = (counts[key] || 0) + 1
        labels[key] = label
        rows.push(Object.assign({}, member, {
            groupKey: key,
            groupCategory: away ? 3 : hoisted ? 0 : active ? 1 : 2,
            groupPosition: hoisted ? Number(member.hoistedRolePosition) || 0 : 0,
            offline: away
        }))
    }
    rows.sort((a, b) => {
        if (a.groupKey !== b.groupKey)
            return a.groupCategory - b.groupCategory || b.groupPosition - a.groupPosition || snowflakeOrder(a.hoistedRoleId || "", b.hoistedRoleId || "")
        const first = String(a.name || "").toLocaleLowerCase()
        const second = String(b.name || "").toLocaleLowerCase()
        return first.localeCompare(second) || snowflakeOrder(a.recordId, b.recordId)
    })
    for (const key in labels) labels[key] += " — " + counts[key]
    return {members: rows, labels: labels, onlineCount: online, offlineCount: offline, voiceCount: inVoice}
}
