# Build every mods_src\*.c into mods\<name>.dll (MSVC x64).
# /GS-: save states copy stacks that can hold a mod's frames, and a /GS frame checks a per-run cookie.
$ErrorActionPreference = "Stop"
$root = Split-Path $PSScriptRoot -Parent
$vs = "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools"
if (-not $env:VSCMD_VER) {
    cmd /c "`"$vs\VC\Auxiliary\Build\vcvars64.bat`" >nul 2>&1 && set" | ForEach-Object {
        if ($_ -match '^([^=]+)=(.*)$') { Set-Item -Path "env:$($matches[1])" -Value $matches[2] }
    }
}
# C12_MODS_OUT: build somewhere else (a running game keeps its loaded mod DLLs locked)
$outdir = if ($env:C12_MODS_OUT) { $env:C12_MODS_OUT } else { "$root\mods" }
New-Item -ItemType Directory -Force $outdir | Out-Null
New-Item -ItemType Directory -Force "$root\build\mods" | Out-Null
$only = $args
Get-ChildItem "$PSScriptRoot\*.c" | ForEach-Object {
    if ($only.Count -and ($only -notcontains $_.BaseName)) { return }
    $out = "$outdir\$($_.BaseName).dll"
    # a fixed load address (no ASLR), different for every mod: save states hold return addresses into
    # mods, so they must sit at the same place in every run
    $h = 0; foreach ($ch in $_.BaseName.ToCharArray()) { $h = ($h * 31 + [int]$ch) % 4093 }
    $base = '0x{0:X}' -f ([int64]0x300000000 + [int64]$h * 0x1000000)
    & cl /nologo /LD /O2 /GS- /W3 "/I$root\runtime" $_.FullName "/Fo$root\build\mods\$($_.BaseName).obj" "/Fe$out" /link "/IMPLIB:$root\build\mods\$($_.BaseName).lib" /DYNAMICBASE:NO "/BASE:$base" | Out-Host
    if ($LASTEXITCODE) { exit 1 }
    Write-Host "built $out"
}
