#!/bin/bash
# Compila o SwitchCast usando o devkitPro instalado no Windows (MSYS2).
# Uso (PowerShell): C:\devkitPro\msys2\usr\bin\bash.exe scripts/build.sh
export DEVKITPRO=/opt/devkitpro
export DEVKITA64=/opt/devkitpro/devkitA64
export PATH=/opt/devkitpro/devkitA64/bin:/opt/devkitpro/tools/bin:/usr/bin:$PATH
cd "$(dirname "$0")/.." || exit 1
make -j6 "$@"
