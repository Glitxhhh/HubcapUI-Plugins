#Requires -Version 7.0
[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
& (Join-Path $repo 'examples/hello-hubcap/scripts/build.ps1')

$objects = Join-Path $repo 'tests/obj'
New-Item -ItemType Directory -Force -Path $objects | Out-Null
$harnessObject = Join-Path $objects 'plugin-template-harness.obj'
$jsonObject = Join-Path $objects 'yyjson.obj'
$harnessExe = Join-Path $objects 'plugin-template-harness.exe'
$uiScript = Join-Path $objects 'hello-hubcap-ui.js'
$sdk = Join-Path $repo 'sdk'
$thirdParty = Join-Path $repo 'renodx/third_party'
$source = Join-Path $repo 'tests/plugin-template-harness.cpp'

$vcvars = $null
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
if (Test-Path -LiteralPath $vswhere) {
    $installation = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if ($installation) {
        $candidate = Join-Path $installation 'VC/Auxiliary/Build/vcvars64.bat'
        if (Test-Path -LiteralPath $candidate) { $vcvars = $candidate }
    }
}
if (-not $vcvars) {
    $found = Get-ChildItem -Path @("${env:ProgramFiles}/Microsoft Visual Studio", "${env:ProgramFiles(x86)}/Microsoft Visual Studio") -Recurse -Filter vcvars64.bat -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($found) { $vcvars = $found.FullName }
}

if ($vcvars) {
    $commands = @"
call "$vcvars" >NUL
if errorlevel 1 exit /b %errorlevel%
cl /nologo /c /O2 /MT /EHsc /std:c++17 /I"$sdk" /I"$thirdParty" "$source" /Fo:"$harnessObject"
if errorlevel 1 exit /b %errorlevel%
cl /nologo /c /O2 /MT /I"$thirdParty" "$thirdParty/yyjson.c" /Fo:"$jsonObject"
if errorlevel 1 exit /b %errorlevel%
link /NOLOGO /MACHINE:X64 /OUT:"$harnessExe" "$harnessObject" "$jsonObject" kernel32.lib
if errorlevel 1 exit /b %errorlevel%
"@
    $commandFile = Join-Path $objects 'build-plugin-template-harness.cmd'
    Set-Content -LiteralPath $commandFile -Value $commands -Encoding ASCII
    & cmd.exe /c "`"$commandFile`""
    if ($LASTEXITCODE -ne 0) { throw 'MSVC failed to build the plugin template harness.' }
} else {
    $zigPath = $env:HUBCAP_ZIG
    if (-not $zigPath) {
        $zigCommand = Get-Command zig.exe -ErrorAction SilentlyContinue
        if ($zigCommand) { $zigPath = $zigCommand.Source }
    }
    if (-not $zigPath) {
        $portableZig = Join-Path $env:TEMP 'hubcap-zig-0.16.0/zig.exe'
        if (Test-Path -LiteralPath $portableZig) { $zigPath = $portableZig }
    }
    if (-not $zigPath -or -not (Test-Path -LiteralPath $zigPath)) {
        throw 'Install Visual Studio C++ Build Tools or set HUBCAP_ZIG to zig.exe.'
    }
    & $zigPath cc -target x86_64-windows-gnu -O2 -I $thirdParty -c (Join-Path $thirdParty 'yyjson.c') -o $jsonObject
    if ($LASTEXITCODE -ne 0) { throw 'Zig failed to compile yyjson for the harness.' }
    & $zigPath c++ -target x86_64-windows-gnu -O2 -std=c++17 -I $sdk -I $thirdParty $source $jsonObject -o $harnessExe
    if ($LASTEXITCODE -ne 0) { throw 'Zig failed to build the plugin template harness.' }
}

& $harnessExe (Join-Path $repo 'examples/hello-hubcap/dist/hello-hubcap.dll') $uiScript (Join-Path $repo 'examples/hello-hubcap/src/plugin.json')
if ($LASTEXITCODE -ne 0) { throw 'Plugin template native checks failed.' }
& node (Join-Path $repo 'tests/plugin-template-ui.test.cjs') $uiScript
if ($LASTEXITCODE -ne 0) { throw 'Plugin template UI checks failed.' }
