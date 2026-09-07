$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$output = Join-Path $root 'obj'
$engine = Get-Content (Join-Path $root 'src/feature_engine.cpp') -Raw
$start = $engine.IndexOf('std::vector<RenoMod> reno_catalog(bool force')
$end = $engine.IndexOf('bool find_reno_mod', $start)
$function = $engine.Substring($start, $end - $start)
$prefix = @'
#include <windows.h>
#include <string>
#include <vector>
#include <cassert>
#include <cstdio>
struct RenoMod { std::string name; };
struct HttpResult { DWORD status; std::string body; std::string error; };
SRWLOCK g_reno_catalog_lock = SRWLOCK_INIT;
std::vector<RenoMod> g_reno_catalog;
ULONGLONG g_reno_catalog_at=0, g_reno_catalog_attempt_at=0, g_reno_catalog_updated_ms=0;
bool g_reno_catalog_failed=false;
ULONGLONG test_now=1000;
int requests=0;
HttpResult next_response{200,"First Game",""};
ULONGLONG test_clock() { return test_now; }
#define GetTickCount64 test_clock
HttpResult http_get_host(const wchar_t*, const wchar_t*, const char*, int, int) { ++requests; return next_response; }
std::vector<RenoMod> parse_reno_catalog(const std::string& body) { return body.empty() ? std::vector<RenoMod>{} : std::vector<RenoMod>{{body}}; }
'@
$suffix = @'
int main() {
    assert(reno_catalog()[0].name == "First Game");
    assert(requests==1 && g_reno_catalog_updated_ms>0);
    auto updated=g_reno_catalog_updated_ms;
    test_now+=899999; reno_catalog(); assert(requests==1);
    test_now++; next_response={503,"","offline"};
    assert(reno_catalog()[0].name=="First Game");
    assert(requests==2 && g_reno_catalog_failed && updated==g_reno_catalog_updated_ms);
    reno_catalog(); assert(requests==2);
    test_now+=60000; next_response={200,"",""};
    assert(reno_catalog()[0].name=="First Game" && requests==3);
    next_response={200,"New Game",""};
    assert(reno_catalog(true)[0].name=="New Game" && requests==4 && !g_reno_catalog_failed);
    next_response={500,"","offline"};
    assert(reno_catalog(true)[0].name=="New Game" && requests==5);
    test_now+=60000; next_response={200,"Recovered Game",""};
    assert(reno_catalog()[0].name=="Recovered Game" && requests==6);
    g_reno_catalog.clear();g_reno_catalog_failed=false;next_response={503,"","offline"};
    assert(reno_catalog().empty() && requests==7);
    assert(reno_catalog().empty() && requests==7);
    puts("RenoDX native catalog tests passed.");
}
'@
$cpp = Join-Path $output 'catalog-test.cpp'
Set-Content -LiteralPath $cpp -Value ($prefix + "`n" + $function + "`n" + $suffix)
$vcvars = Get-ChildItem -Path @("${env:ProgramFiles}\Microsoft Visual Studio", "${env:ProgramFiles(x86)}\Microsoft Visual Studio") -Recurse -Filter vcvars64.bat -ErrorAction SilentlyContinue | Select-Object -First 1
$exe = Join-Path $output 'catalog-test.exe'
$commands = @("call `"$($vcvars.FullName)`" >NUL", "cl /nologo /EHsc /std:c++17 `"$cpp`" /Fo:`"$output\catalog-test.obj`" /Fe:`"$exe`"", 'if errorlevel 1 exit /b %errorlevel%', "`"$exe`"")
$cmdFile = Join-Path $output 'catalog-test.cmd'
Set-Content -LiteralPath $cmdFile -Value ($commands -join "`r`n")
cmd /c "`"$cmdFile`""
if ($LASTEXITCODE -ne 0) { throw 'RenoDX catalog tests failed.' }
