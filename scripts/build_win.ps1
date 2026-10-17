# Compila o SwitchCast no Windows usando o devkitPro (MSYS2).
#
# Por que copiar para outra pasta? O toolchain MSYS2 do devkitPro falha ao criar os
# arquivos de dependência (.d) dentro de pastas sincronizadas pelo OneDrive. Este
# script espelha o projeto em C:\sc_build, compila lá e traz o .nro de volta.
#
# Uso:  powershell -ExecutionPolicy Bypass -File scripts\build_win.ps1 [-Clean] [-Debug]
param([switch]$Clean, [switch]$Debug)

$ErrorActionPreference = 'Stop'
$src = Split-Path -Parent $PSScriptRoot
$dst = 'C:\sc_build'
$bash = 'C:\devkitPro\msys2\usr\bin\bash.exe'

robocopy $src $dst /MIR /XD build .git /XF build.log *.nro *.elf *.nacp /NFL /NDL /NJH /NJS /NP | Out-Null
New-Item -ItemType Directory -Force "$dst\build" | Out-Null

$target = if ($Clean) { 'clean' } else { '' }
$flags  = if ($Debug) { 'DEBUG=1' } else { '' }
& $bash -c "bash /c/sc_build/scripts/build.sh $target $flags"
if ($LASTEXITCODE -ne 0) { throw "Falha na compilação (exit $LASTEXITCODE)" }

if (-not $Clean) {
    Copy-Item "$dst\SwitchCast.nro" $src -Force
    Write-Host "OK: $src\SwitchCast.nro"
}
