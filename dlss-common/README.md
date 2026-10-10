# dlss-common

Shared, header-only helpers used by the `dlsstweaks`, `dlssgtofsr3`, and `dlssenabler`
plugins: WinHTTP requests, the Nexus Mods "site" domain file-listing/download-link API,
Steam library/game-folder resolution, ZIP extraction (miniz), and small per-game install
manifests so "remove" only deletes files a plugin actually placed.

Each plugin includes `dlss_common.h` directly into its own `src/plugin.cpp` translation
unit (see [docs/plugin-development.md](../docs/plugin-development.md): plugins are
self-contained, not a shared library), so every function in the header is `inline`.

All three plugins read the Nexus API key from `%LOCALAPPDATA%\HubcapUI\settings.json`
("NexusApiKey"), the same key RenoDX and OptiScaler already use (set via Hubcap Advanced
Settings).
