$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
foreach ($plugin in @("renodx", "optiscaler", "hubcapticket", "dlsssuite")) {
    Write-Host "Building $plugin..."
    & (Join-Path $root "$plugin\scripts\build.ps1")
    if ($LASTEXITCODE -ne 0) { throw "$plugin build failed with exit code $LASTEXITCODE." }
}
Write-Host "All plugin builds completed."
