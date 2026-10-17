# scripts/git_sync.ps1 - Sincroniza e envia alterações para o GitHub
param(
    [string]$Message = "chore: atualizacoes no SwitchCast"
)

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot

Set-Location $repoRoot

$gitCmd = if (Test-Path "C:\Program Files\Git\cmd\git.exe") { "C:\Program Files\Git\cmd\git.exe" } else { "git" }

Write-Host "Adicionando arquivos..." -ForegroundColor Cyan
& $gitCmd add .

$status = & $gitCmd status --porcelain
if (-not $status) {
    Write-Host "Nenhuma alteracao para commitar." -ForegroundColor Yellow
} else {
    Write-Host "Criando commit: $Message" -ForegroundColor Cyan
    & $gitCmd commit -m $Message
}

Write-Host "Enviando para o GitHub (origin main)..." -ForegroundColor Green
& $gitCmd push -u origin main

