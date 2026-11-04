.pragma library

var nextId = 0
var current = null

function createId() { return "inline-media-" + (++nextId) }
function claim(player) {
    if (current && current !== player) current.suspend()
    current = player
}
function release(player) { if (current === player) current = null }

var bookmarks = ({})
var bookmarkOrder = []
function remember(key, milliseconds) {
    if (!key) return
    if (bookmarks[key] === undefined) bookmarkOrder.push(key)
    bookmarks[key] = Math.max(0, milliseconds)
    while (bookmarkOrder.length > 100) delete bookmarks[bookmarkOrder.shift()]
}
function position(key) { return bookmarks[key] || 0 }
function forget(key) { delete bookmarks[key]; bookmarkOrder = bookmarkOrder.filter(item => item !== key) }
