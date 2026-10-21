#pragma once
#include <string>
#include <vector>
#include <functional>
#include <atomic>
#include <mutex>
#include <thread>
#include "Types.hpp"

namespace SwitchCast::Updater {

struct CoreUpdateInfo {
    std::string version;
    int versionCode = 0;
    std::string downloadUrl;
    std::string sha256;
    size_t sizeBytes = 0;
    std::vector<std::string> changelog;
};

enum class OtaStatus {
    Idle,
    CheckingManifest,
    Downloading,
    VerifyingHash,
    UpdateReady,
    ApplyingUpdate,
    RebootPending,
    RollbackTriggered,
    Error
};

enum class InstallStage {
    None,
    Verifying,
    BackingUp,
    Writing,
    Restarting,
    Error
};

using ProgressCallback = std::function<void(size_t bytesDownloaded, size_t totalBytes, OtaStatus status)>;

class OtaUpdater {
public:
    static constexpr const char* kCurrentVersion = SWITCHCAST_VERSION_STR;
    static constexpr int kCurrentVersionCode = SWITCHCAST_VERSION_CODE;

    static OtaUpdater& Instance();

    // 1. BOOT GUARD (Anti-Brick): Chamado no início do main()
    bool VerifyBootIntegrity(bool forceRollbackComboPressed);
    void MarkBootSuccessful();

    // 2. BACKGROUND CHECKER & DOWNLOAD AUTOMÁTICO
    void StartBackgroundChecker();
    void StopBackgroundChecker();

    bool HasUpdateReady() const { return mUpdateReady.load(); }
    CoreUpdateInfo GetPendingUpdateInfo() const;

    // Monitoramento de download em tempo real (para a Barra de Download na UI)
    void SetDownloadProgress(size_t downloaded, size_t total);
    void FinishDownload();
    bool IsDownloading() const { return mIsDownloading.load(); }
    size_t GetDownloadedBytes() const { return mDownloadedBytes.load(); }
    size_t GetTotalBytes() const { return mTotalBytes.load(); }
    float GetDownloadFraction() const {
        size_t total = mTotalBytes.load();
        if (total == 0) return 0.0f;
        return static_cast<float>(mDownloadedBytes.load()) / static_cast<float>(total);
    }

    // 3. INSTALAÇÃO ASSÍNCRONA COM BARRA DE PROGRESSO NA TELA
    bool StartInstallation();
    bool IsInstalling() const { return mIsInstalling.load(); }
    InstallStage GetInstallStage() const { return mInstallStage.load(); }
    float GetInstallFraction() const { return mInstallFraction.load(); }
    std::string GetInstallStatus() const;

    bool ApplyPendingUpdate();
    void DismissPendingUpdate();

    // Resolução dinâmica do local real do NRO (suporta tanto /switch/SwitchCast quanto /switch)
    void InitFromMainArgs(int argc, char* argv[]);
    void SetRunningNroPath(const std::string& path);
    std::string GetTargetNroPath() const;
    std::string GetBackupNroPath() const;

    // Executado exclusivamente na Main Thread antes do return 0
    bool ApplyUpdateAndScheduleRestart();
    bool IsRestartPending() const { return mRestartPending.load(); }
    void RequestRestart() { mRestartPending.store(true); mExitRequested.store(true); }
    void WaitForInstallThread();

    // 4. PULL OTA manual (se chamado diretamente)
    bool DownloadAndInstallCore(const CoreUpdateInfo& info, ProgressCallback onProgress = nullptr);

    // 5. PUSH OTA: Recebe binário via LAN e prepara o modal para o usuário no Switch
    bool StageUploadedNro(const std::string& tempFilePath, const std::string& expectedSha256,
                          const std::string& version = "", const std::vector<std::string>& changelog = {});
    bool ApplyUploadedNro(const std::string& tempFilePath, const std::string& expectedSha256);

    // 6. ROLLBACK: Restaura SwitchCast.nro.bak imediatamente e reinicia
    bool RollbackToBackup();

    // Utilitários de arquivo e hash com callback de progresso
    static bool CopyFileContents(const std::string& src, const std::string& dst,
                                 const std::function<void(float)>& onProgress = nullptr);
    static std::string CalculateFileSha256(const std::string& filePath);
    static std::string CalculateFileSha256WithProgress(const std::string& filePath,
                                                       const std::function<void(float)>& onProgress = nullptr);
    static bool VerifySha256(const std::string& filePath, const std::string& expectedHex);

    bool IsExitRequested() const { return mExitRequested.load(); }

private:
    OtaUpdater() = default;
    ~OtaUpdater();

    void BackgroundCheckerLoop();
    bool CheckRemoteManifestAndDownload();

    void WriteBootState(bool pendingValidation, const std::string& statusNote);
    bool ReadBootState(bool& outPendingValidation);
    bool TriggerSelfRestart();

    std::atomic<bool> mExitRequested{false};
    std::atomic<bool> mRestartPending{false};
    std::atomic<bool> mUpdateReady{false};
    std::atomic<bool> mStopChecker{false};

    // Download tracking
    std::atomic<bool> mIsDownloading{false};
    std::atomic<size_t> mDownloadedBytes{0};
    std::atomic<size_t> mTotalBytes{0};

    // Installation tracking
    std::atomic<bool> mIsInstalling{false};
    std::atomic<InstallStage> mInstallStage{InstallStage::None};
    std::atomic<float> mInstallFraction{0.0f};
    mutable std::mutex mInstallMutex;
    std::string mInstallStatus;
    std::thread mInstallThread;

    mutable std::mutex mUpdateMutex;
    CoreUpdateInfo mPendingInfo;
    std::thread mCheckerThread;
    std::string mRunningNroPath;

    static constexpr const char* kLauncherNro = "sdmc:/switch/SwitchCast/SwitchCast.nro";
    static constexpr const char* kTargetNro   = "sdmc:/switch/SwitchCast/modules/SwitchCastCore.nro";
    static constexpr const char* kTempNro     = "sdmc:/switch/SwitchCast/modules/update.nro";
    static constexpr const char* kBackupNro   = "sdmc:/switch/SwitchCast/modules/SwitchCastCore.nro.bak";
    static constexpr const char* kStateFile   = "sdmc:/switch/SwitchCast/modules/pending_update.json";
    static constexpr const char* kSettingsFile = "sdmc:/switch/SwitchCast/config/settings.json";
};

} // namespace SwitchCast::Updater
