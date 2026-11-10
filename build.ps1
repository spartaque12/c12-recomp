# Regenerate recompiled C and build c12.exe (MSVC + Ninja).
param([switch]$NoRecomp, [string]$Config = "RelWithDebInfo")
$ErrorActionPreference = "Stop"
$root = $PSScriptRoot
$vs = "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools"
if (-not $env:VSCMD_VER) {
    cmd /c "`"$vs\VC\Auxiliary\Build\vcvars64.bat`" >nul 2>&1 && set" | ForEach-Object {
        if ($_ -match '^([^=]+)=(.*)$') { Set-Item -Path "env:$($matches[1])" -Value $matches[2] }
    }
}
if (-not $NoRecomp) { python "$root\tools\recomp\recomp.py" "$root\recomp.json"; if ($LASTEXITCODE) { exit 1 } }
$cmake = "$vs\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
if (-not (Test-Path "$root\build\build.ninja")) {
    & $cmake -S $root -B "$root\build" -G Ninja "-DCMAKE_BUILD_TYPE=$Config" -DCMAKE_C_COMPILER=cl -DCMAKE_CXX_COMPILER=cl | Out-Host
    if ($LASTEXITCODE) { exit 1 }
}
& $cmake --build "$root\build" 2>&1 | Select-String -NotMatch '^\[\d+/\d+\] Building' | Out-Host
exit $LASTEXITCODE
