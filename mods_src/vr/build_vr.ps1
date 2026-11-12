# Build the VR pieces (MSVC x64, C++17, static CRT /MT like the C mods):
#   build\mods\vr\vr_xr.lib          the OpenXR layer (xr.cpp) that mods\vr.dll links
#   mods\vr.dll                      the VR mod (vr_mod.cpp; for now a placeholder)
#   mods\vr_xr_test_runtime.dll+json a simulated OpenXR runtime for tests without a headset (test\xr_test_runtime.cpp)
#   mods\openxr_loader.dll           the Khronos OpenXR loader 1.1.65 (Apache-2.0 OR MIT), from third_party\openxr\bin
#   build\mods\vr\vr_xr_smoke.exe    a headless test of the layer (test\xr_smoke.cpp)
# Usage: mods_src\vr\build_vr.ps1 [-Test] [-NoTestRuntime]
#   -Test: run vr_xr_smoke.exe against the test runtime afterwards (captures in build\mods\vr\smoke\).
# Like build_mods.ps1: /GS- (save states copy stacks that can hold a mod's frames, a /GS frame checks a per-run cookie)
# and a fixed load address per DLL (no ASLR; save states hold return addresses into mods). C12_MODS_OUT: another
# output folder (a running game keeps its mods locked).
param([switch]$Test, [switch]$NoTestRuntime)
$ErrorActionPreference = "Stop"
$here = $PSScriptRoot
$root = Split-Path (Split-Path $here -Parent) -Parent
$vs = "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools"
if (-not $env:VSCMD_VER) {
    cmd /c "`"$vs\VC\Auxiliary\Build\vcvars64.bat`" >nul 2>&1 && set" | ForEach-Object {
        if ($_ -match '^([^=]+)=(.*)$') { Set-Item -Path "env:$($matches[1])" -Value $matches[2] }
    }
}
$outdir = if ($env:C12_MODS_OUT) { $env:C12_MODS_OUT } else { "$root\mods" }
$obj = "$root\build\mods\vr"
New-Item -ItemType Directory -Force $outdir, $obj | Out-Null

$inc = @("/I$root\runtime", "/I$here", "/I$here\third_party\openxr\include")
$cxx = @("/nologo", "/O2", "/GS-", "/W3", "/EHsc", "/std:c++17", "/MT", "/Zc:__cplusplus",
         "/DWIN32_LEAN_AND_MEAN", "/DNOMINMAX", "/D_CRT_SECURE_NO_WARNINGS") + $inc
$syslibs = @("d3d11.lib", "dxgi.lib", "d3dcompiler.lib", "advapi32.lib", "user32.lib")

# the same fixed base as build_mods.ps1 gives a mod of that name
function Get-ModBase([string]$name) {
    $h = 0; foreach ($ch in $name.ToCharArray()) { $h = ($h * 31 + [int]$ch) % 4093 }
    return '0x{0:X}' -f ([int64]0x300000000 + [int64]$h * 0x1000000)
}
function Invoke-Checked([string]$what) {
    if ($LASTEXITCODE) { Write-Host "FAILED: $what"; exit 1 }
}

# 1. the OpenXR layer as a static library
& cl @cxx /c "$here\xr.cpp" "/Fo$obj\xr.obj" | Out-Host; Invoke-Checked "xr.cpp"
& lib /nologo "/OUT:$obj\vr_xr.lib" "$obj\xr.obj" | Out-Host; Invoke-Checked "vr_xr.lib"
Write-Host "built $obj\vr_xr.lib"

# 2. mods\vr.dll
$base = Get-ModBase "vr"
& cl @cxx /LD "$here\vr_mod.cpp" "/Fo$obj\vr_mod.obj" "/Fe$outdir\vr.dll" "$obj\vr_xr.lib" @syslibs `
    /link "/IMPLIB:$obj\vr.lib" /DYNAMICBASE:NO "/BASE:$base" | Out-Host; Invoke-Checked "vr.dll"
Write-Host "built $outdir\vr.dll (base $base)"

# 3. the test runtime (+ its manifest next to it)
if (-not $NoTestRuntime) {
    $base = Get-ModBase "vr_xr_test_runtime"
    & cl @cxx /LD "$here\test\xr_test_runtime.cpp" "/Fo$obj\xr_test_runtime.obj" "/Fe$outdir\vr_xr_test_runtime.dll" @syslibs `
        /link "/IMPLIB:$obj\vr_xr_test_runtime.lib" /DYNAMICBASE:NO "/BASE:$base" | Out-Host; Invoke-Checked "vr_xr_test_runtime.dll"
    Copy-Item "$here\test\xr_test_runtime.json" "$outdir\vr_xr_test_runtime.json" -Force
    Write-Host "built $outdir\vr_xr_test_runtime.dll + .json (base $base)"
}

# 4. the loader
Copy-Item "$here\third_party\openxr\bin\openxr_loader.dll" "$outdir\openxr_loader.dll" -Force
Write-Host "copied $outdir\openxr_loader.dll"

# 5. the smoke test
& cl @cxx "$here\test\xr_smoke.cpp" "/Fo$obj\xr_smoke.obj" "/Fe$obj\vr_xr_smoke.exe" "$obj\vr_xr.lib" @syslibs | Out-Host
Invoke-Checked "vr_xr_smoke.exe"
Copy-Item "$here\third_party\openxr\bin\openxr_loader.dll" "$obj\openxr_loader.dll" -Force
Write-Host "built $obj\vr_xr_smoke.exe"

if ($Test) {
    $smoke = "$obj\smoke"
    if (Test-Path $smoke) { Remove-Item -Recurse -Force $smoke }
    New-Item -ItemType Directory -Force $smoke | Out-Null
    $env:XR_RUNTIME_JSON = "$outdir\vr_xr_test_runtime.json"
    $env:XRTEST_CONTROL = "$smoke\control.txt"
    $env:XRTEST_LOG = "$smoke\xr_test_runtime.log"
    if (-not $env:XRTEST_CAPTURE_FORMAT) { $env:XRTEST_CAPTURE_FORMAT = "bmp" }
    # the loader ignores XR_RUNTIME_JSON in an elevated (high integrity) process - this machine has UAC off, so
    # everything is elevated: then xr.cpp loads the test runtime directly (C12_VR_RUNTIME)
    if ((whoami /groups) -match 'S-1-16-12288|S-1-16-16384') {
        Write-Host "elevated: the loader would ignore XR_RUNTIME_JSON; using C12_VR_RUNTIME (the runtime without the loader)"
        $env:C12_VR_RUNTIME = "$outdir\vr_xr_test_runtime.dll"
    }
    Push-Location $obj
    try { & "$obj\vr_xr_smoke.exe" 100 2>&1 | Out-Host; $code = $LASTEXITCODE } finally { Pop-Location }
    Write-Host "vr_xr_smoke.exe exit code $code; vr.log: $obj\vr.log; runtime log: $env:XRTEST_LOG; captures: $smoke"
    exit $code
}
