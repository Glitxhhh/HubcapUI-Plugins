# dlss-common

Shared, header-only helpers used by the `dlsssuite` plugin (DLSSTweaks, dlssg-to-fsr3,
and DLSS Enabler — formerly three separate plugins, merged into one so they could share
a single Nexus API key UI entry instead of coordinating across plugin boundaries):
WinHTTP requests, the Nexus Mods "site" domain file-listing/download-link API, Steam
library/game-folder resolution, ZIP extraction (miniz), and small per-game install
manifests so "remove" only deletes files that part of the plugin actually placed.

`dlsssuite` includes `dlss_common.h` directly into its own `src/plugin.cpp` translation
unit (see [docs/plugin-development.md](../docs/plugin-development.md): plugins are
self-contained, not a shared library), so every function in the header is `inline`.

`dlsssuite` reads the Nexus API key from `%LOCALAPPDATA%\HubcapUI\settings.json`
("NexusApiKey"), the same key RenoDX and OptiScaler already use (set via Hubcap Advanced
Settings).
