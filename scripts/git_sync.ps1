# scripts/git_sync.ps1 - Sincroniza e envia alterações para o GitHub
param(
    [string]$Message = "chore: atualizacoes no SwitchCast"
)

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot

Set-Location $repoRoot

Write-Host "Adicionando arquivos..." -ForegroundColor Cyan
git add .

$status = git status --porcelain
if (-not $status) {
    Write-Host "Nenhuma alteracao para commitar." -ForegroundColor Yellow
    exit 0
}

Write-Host "Criando commit: $Message" -ForegroundColor Cyan
git commit -m $Message

Write-Host "Enviando para o GitHub (origin main)..." -ForegroundColor Green
git push origin main
