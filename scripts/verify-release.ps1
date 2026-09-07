$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$version = (Get-Content (Join-Path $root 'VERSION') -Raw).Trim()
if ($version -notmatch '^(0|[1-9][0-9]{0,5})\.(0|[1-9][0-9]{0,5})\.(0|[1-9][0-9]{0,5})$') { throw 'Invalid release version' }
if ($env:GITHUB_REF_TYPE -eq 'tag' -and $env:GITHUB_REF_NAME -ne "v$version") { throw 'Release tag does not match VERSION' }
foreach ($plugin in @('renodx','optiscaler')) {
    $manifest = Get-Content (Join-Path $root "$plugin/src/plugin.json") -Raw | ConvertFrom-Json
    $source = Get-Content (Join-Path $root "$plugin/src/plugin.cpp") -Raw
    if ($manifest.version -ne $version -or -not $source.Contains('plugin->version = "' + $version + '"')) { throw "Version mismatch: $plugin" }
    if ($manifest.update.repository -ne 'YaBoi69er/HubcapUI-Plugins' -or $manifest.update.asset -ne "$plugin.dll") { throw "Update source mismatch: $plugin" }
}
Write-Host "Release metadata verified: v$version"
