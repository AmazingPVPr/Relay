.pragma library
// Shared only by windows in this QML engine; no background activity or service.
var shuttingDown = false
function closing() { return shuttingDown }
function beginShutdown() { shuttingDown = true }
