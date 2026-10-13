#Requires -Version 7.0
# Builds the DLSS Enabler HubcapUI plugin. Follows examples/hello-hubcap/scripts/build.ps1's
# shape (see docs/plugin-development.md) with miniz added for ZIP extraction.
$ErrorActionPreference = 'Stop'
$pluginRoot = Split-Path -Parent $PSScriptRoot
$repo = [IO.Path]::GetFullPath((Join-Path $pluginRoot '..'))
$source = Join-Path $pluginRoot 'src'
$sdk = Join-Path $repo 'sdk'
$common = Join-Path $repo 'dlss-common'
$thirdParty = Join-Path $repo 'renodx/third_party'
$objects = Join-Path $pluginRoot 'obj'
$dist = Join-Path $pluginRoot 'dist'
New-Item -ItemType Directory -Force -Path $objects,$dist | Out-Null
$output = Join-Path $dist 'dlssenabler.dll'

$pluginObject = Join-Path $objects 'plugin.obj'
$jsonObject = Join-Path $objects 'yyjson.obj'
$resourceObject = Join-Path $objects 'plugin.res'
$minizNames = @('miniz','miniz_tdef','miniz_tinfl','miniz_zip')
$minizObjects = $minizNames | ForEach-Object { Join-Path $objects "$_.obj" }

$vswhere = "${env:ProgramFiles(x86)}/Microsoft Visual Studio/Installer/vswhere.exe"
$vcvars = if (Test-Path -LiteralPath $vswhere) {
    & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -find 'VC/Auxiliary/Build/vcvars64.bat' | Select-Object -First 1
}
if (-not $vcvars) {
    $found = Get-ChildItem -Path @("${env:ProgramFiles}/Microsoft Visual Studio", "${env:ProgramFiles(x86)}/Microsoft Visual Studio") -Recurse -Filter vcvars64.bat -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($found) { $vcvars = $found.FullName }
}
if (-not $vcvars) { throw 'Install Visual Studio C++ Build Tools and the Windows SDK, or set HUBCAP_ZIG to zig.exe.' }

$commands = @()
# Trim the inherited PATH before calling vcvars64.bat: on a machine with an already-large
# PATH, vcvars64.bat's own "set PATH=...;%PATH%" line can exceed cmd.exe's 8191-character
# limit ("The input line is too long.").
$commands += 'set "PATH=%SystemRoot%\System32;%SystemRoot%;%SystemRoot%\System32\Wbem;%SystemRoot%\System32\WindowsPowerShell\v1.0"'
$commands += "call `"$vcvars`" >NUL"
$commands += 'if errorlevel 1 exit /b %errorlevel%'
$commands += "cl /nologo /c /O2 /MT /EHsc /std:c++17 /I`"$sdk`" /I`"$common`" /I`"$thirdParty`" `"$source/plugin.cpp`" /Fo:`"$pluginObject`""
$commands += 'if errorlevel 1 exit /b %errorlevel%'
$commands += "cl /nologo /c /O2 /MT /I`"$thirdParty`" `"$thirdParty/yyjson.c`" /Fo:`"$jsonObject`""
$commands += 'if errorlevel 1 exit /b %errorlevel%'
for ($i = 0; $i -lt $minizNames.Count; $i++) {
    $commands += "cl /nologo /c /O2 /MT /I`"$thirdParty`" `"$thirdParty/$($minizNames[$i]).c`" /Fo:`"$($minizObjects[$i])`""
    $commands += 'if errorlevel 1 exit /b %errorlevel%'
}
$commands += "rc /nologo /I `"$source`" /fo `"$resourceObject`" `"$source/plugin.rc`""
$commands += 'if errorlevel 1 exit /b %errorlevel%'
$linkedObjects = (@($pluginObject,$jsonObject) + $minizObjects + @($resourceObject)) -join '" "'
$commands += "link /NOLOGO /DLL /MACHINE:X64 /OUT:`"$output`" `"$linkedObjects`" kernel32.lib winhttp.lib shlwapi.lib shell32.lib"
$commands += 'if errorlevel 1 exit /b %errorlevel%'

$commandFile = Join-Path $objects 'build.cmd'
Set-Content -LiteralPath $commandFile -Value ($commands -join "`r`n") -Encoding ASCII
& cmd.exe /c "`"$commandFile`""
if ($LASTEXITCODE -ne 0) { throw "DLSS Enabler plugin build failed: $LASTEXITCODE" }
Write-Host "DLSS Enabler plugin written to $output"
