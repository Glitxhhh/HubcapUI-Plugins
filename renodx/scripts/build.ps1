param(
    [string]$HostVersion = "1.0.4",
    [switch]$Install
)

$ErrorActionPreference = "Stop"
$root = Resolve-Path (Join-Path $PSScriptRoot "..")
$source = Join-Path $root "src"
$sdk = Join-Path $root "sdk"
$thirdParty = Join-Path $root "third_party"
$objects = Join-Path $root "obj"
$dist = Join-Path $root "dist"
$output = Join-Path $dist "renodx.dll"
New-Item -ItemType Directory -Force -Path $objects,$dist | Out-Null

$vcvars = Get-ChildItem -Path @("${env:ProgramFiles}\Microsoft Visual Studio", "${env:ProgramFiles(x86)}\Microsoft Visual Studio") -Recurse -Filter vcvars64.bat -ErrorAction SilentlyContinue | Select-Object -First 1
if (-not $vcvars) { throw "Visual Studio C++ Build Tools were not found." }

$engineObject = Join-Path $objects "feature_engine.obj"
$pluginObject = Join-Path $objects "plugin.obj"
$yyjsonObject = Join-Path $objects "yyjson.obj"
$resourceObject = Join-Path $objects "plugin.res"
$minizNames = @("miniz","miniz_tdef","miniz_tinfl","miniz_zip")
$minizObjects = $minizNames | ForEach-Object { Join-Path $objects "$_.obj" }
$commands = @(
    "call `"$($vcvars.FullName)`" >NUL",
    "cl /nologo /c /O2 /EHsc /std:c++17 /D_WIN32 /D_WINDOWS /DHUBCAP_UI_VERSION=\`"$HostVersion\`" /I`"$sdk`" /I`"$source`" /I`"$thirdParty`" `"$(Join-Path $source 'feature_engine.cpp')`" /Fo:`"$engineObject`"",
    "if errorlevel 1 exit /b %errorlevel%",
    "cl /nologo /c /O2 /EHsc /std:c++17 /D_WIN32 /D_WINDOWS /I`"$sdk`" /I`"$source`" /I`"$thirdParty`" `"$(Join-Path $source 'plugin.cpp')`" /Fo:`"$pluginObject`"",
    "if errorlevel 1 exit /b %errorlevel%",
    "cl /nologo /c /O2 /D_WIN32 /D_WINDOWS /I`"$thirdParty`" `"$(Join-Path $thirdParty 'yyjson.c')`" /Fo:`"$yyjsonObject`"",
    "if errorlevel 1 exit /b %errorlevel%"
)
for ($index = 0; $index -lt $minizNames.Count; $index++) {
    $commands += "cl /nologo /c /O2 /D_WIN32 /D_WINDOWS /I`"$thirdParty`" `"$(Join-Path $thirdParty ($minizNames[$index] + '.c'))`" /Fo:`"$($minizObjects[$index])`""
    $commands += "if errorlevel 1 exit /b %errorlevel%"
}
$commands += "rc /nologo /I `"$sdk`" /fo `"$resourceObject`" `"$(Join-Path $source 'plugin.rc')`""
$commands += "if errorlevel 1 exit /b %errorlevel%"
$linkedObjects = @($pluginObject,$engineObject,$yyjsonObject) + $minizObjects + @($resourceObject)
$quotedObjects = ($linkedObjects | ForEach-Object { '"' + $_ + '"' }) -join ' '
$commands += "link /NOLOGO /DLL /OUT:`"$output`" $quotedObjects dbghelp.lib winhttp.lib bcrypt.lib gdi32.lib shell32.lib shlwapi.lib user32.lib ole32.lib"
$commands += "if errorlevel 1 exit /b %errorlevel%"
$commandFile = Join-Path $objects "build.cmd"
Set-Content -LiteralPath $commandFile -Value ($commands -join "`r`n") -Encoding ASCII
cmd.exe /c "`"$commandFile`""
if ($LASTEXITCODE -ne 0) { throw "RenoDX plugin build failed with exit code $LASTEXITCODE." }

if ($Install) {
    if (Get-Process steam -ErrorAction SilentlyContinue) { throw "Close Steam before installing a rebuilt plugin DLL." }
    $destination = "C:\Program Files (x86)\Steam\hubcapui-plugins"
    New-Item -ItemType Directory -Force -Path $destination | Out-Null
    Copy-Item -LiteralPath $output -Destination $destination -Force
    Write-Host "Installed RenoDX plugin to $destination"
}
Write-Host "RenoDX plugin written to $output"
