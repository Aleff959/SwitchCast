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

robocopy $src $dst /MIR /XD build build_launcher .git /XF build.log *.nro *.elf *.nacp /NFL /NDL /NJH /NJS /NP | Out-Null
New-Item -ItemType Directory -Force "$dst\build" | Out-Null
New-Item -ItemType Directory -Force "$dst\launcher\build_launcher" | Out-Null

$target = if ($Clean) { 'clean' } else { '' }
$flags  = if ($Debug) { 'DEBUG=1' } else { '' }
& $bash -c "bash /c/sc_build/scripts/build.sh $target $flags"
if ($LASTEXITCODE -ne 0) { throw "Falha na compilação (exit $LASTEXITCODE)" }

if (-not $Clean) {
    if (Test-Path "$dst\SwitchCastCore.nro") {
        Copy-Item "$dst\SwitchCastCore.nro" $src -Force
        Write-Host "OK: $src\SwitchCastCore.nro"

        # Atualização automática de hash SHA-256 e size_bytes no update_manifest.json
        $manifestPath = "$src\update_manifest.json"
        if (Test-Path $manifestPath) {
            $coreFile = "$src\SwitchCastCore.nro"
            $hash = (Get-FileHash $coreFile -Algorithm SHA256).Hash.ToLower()
            $size = (Get-Item $coreFile).Length

            $json = Get-Content $manifestPath -Raw | ConvertFrom-Json
            $json.core.sha256 = $hash
            $json.core.size_bytes = $size
            $json.core.version = "2.1.0"
            $json.core.version_code = 20100
            $json.core.download_url = "https://github.com/Aleff959/SwitchCast/releases/download/v2.1.0/SwitchCastCore.nro"
            [System.IO.File]::WriteAllText($manifestPath, ($json | ConvertTo-Json -Depth 10), [System.Text.UTF8Encoding]::new($false))
            Write-Host "OK: Manifest atualizado com SHA256 $hash ($size bytes)"
        }
    }

    if (Test-Path "$dst\launcher\SwitchCast.nro") {
        Copy-Item "$dst\launcher\SwitchCast.nro" $src -Force
        Write-Host "OK: $src\SwitchCast.nro (Launcher)"
    } elseif (Test-Path "$dst\SwitchCast.nro") {
        Copy-Item "$dst\SwitchCast.nro" $src -Force
        Write-Host "OK: $src\SwitchCast.nro"
    }
}
