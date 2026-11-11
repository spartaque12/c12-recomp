# Build every mods_src\*.c into mods\<name>.dll (MSVC x64).
$ErrorActionPreference = "Stop"
$root = Split-Path $PSScriptRoot -Parent
$vs = "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools"
if (-not $env:VSCMD_VER) {
    cmd /c "`"$vs\VC\Auxiliary\Build\vcvars64.bat`" >nul 2>&1 && set" | ForEach-Object {
        if ($_ -match '^([^=]+)=(.*)$') { Set-Item -Path "env:$($matches[1])" -Value $matches[2] }
    }
}
New-Item -ItemType Directory -Force "$root\mods" | Out-Null
New-Item -ItemType Directory -Force "$root\build\mods" | Out-Null
$only = $args
Get-ChildItem "$PSScriptRoot\*.c" | ForEach-Object {
    if ($only.Count -and ($only -notcontains $_.BaseName)) { return }
    $out = "$root\mods\$($_.BaseName).dll"
    & cl /nologo /LD /O2 /W3 "/I$root\runtime" $_.FullName "/Fo$root\build\mods\$($_.BaseName).obj" "/Fe$out" /link "/IMPLIB:$root\build\mods\$($_.BaseName).lib" | Out-Host
    if ($LASTEXITCODE) { exit 1 }
    Write-Host "built $out"
}
