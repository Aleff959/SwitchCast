param(
    [Parameter(Mandatory=$true)][string]$SwitchIp,
    [Parameter(Mandatory=$true)][ValidateSet("Core", "Module", "Rollback")][string]$Mode,
    [string]$FilePath = "",
    [string]$ModuleId = "",
    [string]$EntryFile = "protocols/custom.js",
    [string]$Version = "2.0.0",
    [string]$Changelog = "Notificacao interativa de atualizacao; Download em background; Auto-restart BootGuard",
    [switch]$InstantReboot,
    [int]$Port = 8080
)

$BaseUrl = "http://${SwitchIp}:${Port}/api/v1"

if ($Mode -eq "Rollback") {
    Invoke-RestMethod -Uri "$BaseUrl/system/rollback" -Method Post
    Write-Host "[OK] Switch revertido para a versao anterior (.bak)!" -ForegroundColor Green
    exit 0
}

$Sha256 = (Get-FileHash -Path $FilePath -Algorithm SHA256).Hash.ToLower()
$Headers = @{ "X-Expected-Sha256" = $Sha256 }

if ($Mode -eq "Core") {
    $Headers["X-Version"] = $Version
    $Headers["X-Changelog"] = $Changelog
    if ($InstantReboot) {
        $Headers["X-Instant-Reboot"] = "true"
    }
    Write-Host "Enviando novo SwitchCast.nro v$Version (SHA256: $Sha256)..." -ForegroundColor Cyan
    $res = Invoke-RestMethod -Uri "$BaseUrl/system/ota-update" `
        -Method Post -InFile $FilePath -ContentType "application/octet-stream" -Headers $Headers -TimeoutSec 180
    Write-Host "[OK] $($res.message)" -ForegroundColor Green
}
elseif ($Mode -eq "Module") {
    $Headers["X-Module-Id"] = $ModuleId
    $Headers["X-Module-Version"] = $Version
    $Headers["X-Entry-File"] = $EntryFile
    Invoke-RestMethod -Uri "$BaseUrl/modules/install" `
        -Method Post -InFile $FilePath -ContentType "application/octet-stream" -Headers $Headers
    Write-Host "[OK] Modulo '$ModuleId' recarregado em memoria (Hot-Reload)!" -ForegroundColor Green
}
