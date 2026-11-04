.pragma library

function cdnImage(value) {
    const url = String(value || "")
    // Remote images are fetched only from Discord's media hosts.
    return /^https:\/\/(cdn\.discordapp\.com|media\.discordapp\.net|images-ext-[12]\.discordapp\.net)\//i.test(url) ? url : ""
}

function isGif(value) { return /\.gif(?:[?#]|$)|[?&]format=gif(?:&|$)/i.test(String(value || "")) }
function isVideo(value, contentType) {
    return /^video\//i.test(String(contentType || "")) || /\.(?:mp4|webm|mov|m4v|ogv)(?:[?#]|$)/i.test(String(value || ""))
}

function imageSize(width, height, maxWidth, maxHeight) {
    const w = Math.max(1, Number(width) || 400)
    const h = Math.max(1, Number(height) || 260)
    const scale = Math.min(1, maxWidth / w, maxHeight / h)
    return { width: Math.max(1, Math.round(w * scale)), height: Math.max(1, Math.round(h * scale)) }
}
