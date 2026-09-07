# Hubcap plugin update standard (v1)

Plugin binaries and releases are independent of the Hubcap host release. The host implements update discovery, verification, and replacement once; authors publish future versions to their own public GitHub repository.

## Manifest

Embed the normal JSON manifest as Windows RCDATA resource 1 and add:

```json
{
  "schemaVersion": 1,
  "id": "author.example",
  "name": "Example Plugin",
  "version": "1.2.3",
  "update": {
    "repository": "Author/Example-Plugin",
    "asset": "example.dll"
  }
}
```

`repository` is an owner/repository pair, without a URL. `asset` must exactly match the installed DLL filename. Omitting `update` leaves a plugin under manual update control. Updating a plugin must preserve its ID and update source; changing source requires a manual installation. Public GitHub releases are supported; private repository credentials are never requested or embedded by the host.

## Release contract

- Publish a stable GitHub release tagged `vX.Y.Z`. Drafts, prereleases, and nonnumeric version suffixes are excluded.
- Upload the raw Windows x64 DLL under the declared asset name. An archive or installer cannot substitute for it.
- The DLL's embedded version must equal `X.Y.Z`; keep its runtime API version string synchronized too.
- Export `HubcapPluginInitializeV1` and implement the current Hubcap v1 plugin ABI.
- The asset must have GitHub's `sha256:` digest. Missing digests postpone the update.
- Versions compare numerically, and the updater never downgrades or reinstalls the same version.

A repository can publish several plugin DLLs together with the same release version, as this repository does. Authors who version plugins independently should use separate release repositories under this v1 convention.

## Host behavior

With the plugin system and Auto-update plugins enabled, the host checks on startup and every six hours while Steam is running. It only checks installed plugins that declare the update metadata. GitHub is polled; authors do not need a webhook or to contact the Hubcap maintainers.

Downloads are staged under `%LOCALAPPDATA%/HubcapUI/plugin-updates`. The host verifies GitHub's SHA-256 digest, AMD64 DLL format, initializer export, embedded plugin ID, and version. A failed check or download leaves the installed plugin intact. Settings shows when an update is ready.

On the next Steam startup, before loading the plugin, the host verifies the staged DLL again and checks that the installed original has not changed since staging. Replacement uses a temporary file on the destination volume and preserves the previous DLL as `.previous`. Failed replacement is rolled back when possible. Steam is never restarted automatically by the plugin updater, and no updater executable is installed.

Turning automatic updates off prevents both new automatic checks and applying staged updates. Existing plugins continue to run. Settings can be reopened to see the latest update status.

This standard currently requires the accompanying plugin-manager changes; it is not a claim that an unmodified upstream v1.0.9 host already supports automatic plugin updates.
