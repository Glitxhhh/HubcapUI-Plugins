#Requires -Version 7.0
$ErrorActionPreference = 'Stop'
$exampleRoot = Split-Path -Parent $PSScriptRoot
$repo = [IO.Path]::GetFullPath((Join-Path $exampleRoot '../..'))
$source = Join-Path $exampleRoot 'src'
$sdk = Join-Path $repo 'sdk'
$json = Join-Path $repo 'renodx/third_party'
$objects = Join-Path $exampleRoot 'obj'
$dist = Join-Path $exampleRoot 'dist'
New-Item -ItemType Directory -Force -Path $objects,$dist | Out-Null
$output = Join-Path $dist 'hello-hubcap.dll'
$pluginObject = Join-Path $objects 'plugin.obj'
$jsonObject = Join-Path $objects 'yyjson.obj'
$resourceObject = Join-Path $objects 'plugin.res'

$vswhere = "${env:ProgramFiles(x86)}/Microsoft Visual Studio/Installer/vswhere.exe"
$vcvars = if (Test-Path -LiteralPath $vswhere) {
    & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -find 'VC/Auxiliary/Build/vcvars64.bat' | Select-Object -First 1
}
if ($vcvars) {
    $commands = @"
call "$vcvars" >NUL
if errorlevel 1 exit /b %errorlevel%
cl /nologo /c /O2 /MT /EHsc /std:c++17 /I"$sdk" /I"$json" "$source/plugin.cpp" /Fo:"$pluginObject"
if errorlevel 1 exit /b %errorlevel%
cl /nologo /c /O2 /MT /I"$json" "$json/yyjson.c" /Fo:"$jsonObject"
if errorlevel 1 exit /b %errorlevel%
rc /nologo /I "$source" /fo "$resourceObject" "$source/plugin.rc"
if errorlevel 1 exit /b %errorlevel%
link /NOLOGO /DLL /MACHINE:X64 /OUT:"$output" "$pluginObject" "$jsonObject" "$resourceObject" kernel32.lib
if errorlevel 1 exit /b %errorlevel%
"@
    $commandFile = Join-Path $objects 'build.cmd'
    Set-Content -LiteralPath $commandFile -Value $commands -Encoding ascii
    & cmd.exe /c "`"$commandFile`""
    if ($LASTEXITCODE -ne 0) { throw "Example MSVC build failed: $LASTEXITCODE" }
} else {
    $zig = $env:HUBCAP_ZIG
    if (-not $zig) { $zig = (Get-Command zig.exe -ErrorAction SilentlyContinue).Source }
    if (-not $zig) {
        $portableZig = Join-Path $env:TEMP 'hubcap-zig-0.16.0/zig.exe'
        if (Test-Path -LiteralPath $portableZig) { $zig = $portableZig }
    }
    if (-not $zig) { throw 'Install Visual Studio C++ Build Tools and the Windows SDK, or set HUBCAP_ZIG to zig.exe.' }
    & $zig c++ -target x86_64-windows-gnu -O2 -std=c++17 -I $sdk -I $json -c (Join-Path $source 'plugin.cpp') -o $pluginObject
    if ($LASTEXITCODE -ne 0) { throw 'Example C++ compilation failed.' }
    & $zig cc -target x86_64-windows-gnu -O2 -I $json -c (Join-Path $json 'yyjson.c') -o $jsonObject
    if ($LASTEXITCODE -ne 0) { throw 'Example JSON parser compilation failed.' }
    & $zig rc /nologo /:auto-includes gnu /I $source /fo $resourceObject (Join-Path $source 'plugin.rc')
    if ($LASTEXITCODE -ne 0) { throw 'Example manifest resource compilation failed.' }
    & $zig c++ -target x86_64-windows-gnu -shared $pluginObject $jsonObject $resourceObject -o $output
    if ($LASTEXITCODE -ne 0) { throw 'Example DLL link failed.' }
}
Write-Host "Example plugin written to $output"
