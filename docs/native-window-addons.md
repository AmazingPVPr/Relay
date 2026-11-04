# Separately distributed native windows

Relay discovers native Qt modules in its bundled add-on directories and its per-user
add-ons folder (Settings → Add-ons → Open add-ons folder). Local modules are disabled
by default. Copy a module into that folder, Refresh, enable it, then choose Open.
No catalog or publication is required. Share the compiled module privately, or
publish it independently. Removal takes effect on next launch; disable first.

A window add-on implements both RelayAddonInterface and RelayWindowAddonInterface.
Its existing API-1 metadata declares the "native-window" capability. openWindow()
receives the module's service and creates or raises a native Qt window. It may use
C++ Qt Quick windows, Qt Widgets when the host supports them, and resources compiled
entirely into its own library. settingsUrl() may be empty for window-only modules.
Normal settings and broadcasting add-ons keep their existing behavior.

The module owns every window, component, request and worker it creates. Its
stopService() must synchronously close all windows and stop all work; the host
then deletes its service. Disabling and application shutdown use that path. Reuse
an existing window when opened again. No service or module instance is created
during discovery. A missing module contributes no entry, resource, UI or feature
to Relay. Do not place private integration source in Relay's addons directory or
add it to Relay's CMake targets/install rules.

SDK headers are under src/addons. Build out of tree with the same Qt/compiler ABI
as the target Relay release. For an example lifecycle and metadata see
addons/testfixture/WindowAddon.cpp and window-metadata.json. The test fixture is
never installed. A typical independent CMake project uses AUTOMOC, add_library
(MODULE), target_include_directories pointing at the SDK, Qt dependencies, and
qt_add_resources for its own UI. Native add-ons are trusted executable code and
have the same OS permissions as Relay; metadata is not a sandbox.

The window interface does not expose Discord tokens or imply service permission.
Integrations should use the service's OAuth/device approval and server-side account
authorization. Possession of a private module must never substitute for access
checks. Private source, service endpoints, credentials and integration resources
belong in the independent module repository and distribution only.
