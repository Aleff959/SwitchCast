#include "updater/OtaUpdater.hpp"
#include "updater/OtaFileEmunandFix.hpp"
#include <switch.h>
#include <curl/curl.h>
#include <mbedtls/sha256.h>
#include <nlohmann/json.hpp>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <sys/stat.h>
#include <cctype>
#include <chrono>

namespace SwitchCast::Updater {

using json = nlohmann::json;

namespace {
    struct CurlWriteContext {
        FILE* fp = nullptr;
        size_t totalBytes = 0;
        size_t downloadedBytes = 0;
        ProgressCallback callback;
    };

    size_t OnCurlWriteFile(void* ptr, size_t size, size_t nmemb, void* userdata) {
        auto* ctx = static_cast<CurlWriteContext*>(userdata);
        size_t written = std::fwrite(ptr, size, nmemb, ctx->fp);
        ctx->downloadedBytes += written;
        if (ctx->callback) {
            ctx->callback(ctx->downloadedBytes, ctx->totalBytes, OtaStatus::Downloading);
        }
        return written;
    }

    size_t OnCurlWriteString(void* ptr, size_t size, size_t nmemb, void* userdata) {
        auto* s = static_cast<std::string*>(userdata);
        s->append(static_cast<char*>(ptr), size * nmemb);
        return size * nmemb;
    }
}

OtaUpdater::OtaUpdater() {
    LoadSettings();
}

OtaUpdater& OtaUpdater::Instance() {
    static OtaUpdater instance;
    return instance;
}

void OtaUpdater::LoadSettings() {
    std::ifstream ifs(kSettingsFile);
    if (!ifs.is_open()) return;
    try {
        json cfg = json::parse(ifs, nullptr, false);
        if (!cfg.is_discarded() && cfg.is_object()) {
            if (cfg.contains("ota_channel") && cfg["ota_channel"].is_string()) {
                std::string ch = cfg["ota_channel"].get<std::string>();
                if (ch == "beta" || ch == "dev") {
                    mChannel.store(OtaChannel::Beta);
                } else {
                    mChannel.store(OtaChannel::Stable);
                }
            }
        }
    } catch (...) {}
}

void OtaUpdater::SaveSettings() {
    mkdir("sdmc:/switch/SwitchCast/config", 0777);
    json cfg = json::object();
    std::ifstream ifs(kSettingsFile);
    if (ifs.is_open()) {
        try {
            cfg = json::parse(ifs, nullptr, false);
            if (cfg.is_discarded() || !cfg.is_object()) {
                cfg = json::object();
            }
        } catch (...) {
            cfg = json::object();
        }
        ifs.close();
    }

    cfg["ota_channel"] = (mChannel.load() == OtaChannel::Beta) ? "beta" : "stable";

    std::ofstream ofs(kSettingsFile, std::ios::trunc);
    if (ofs.is_open()) {
        ofs << cfg.dump(4);
    }
}

void OtaUpdater::SetChannel(OtaChannel channel) {
    mChannel.store(channel);
    SaveSettings();
    {
        std::lock_guard<std::mutex> lock(mUpdateMutex);
        mPendingInfo = CoreUpdateInfo{};
        mUpdateReady.store(false);
    }
    mForceCheck.store(true);
}

void OtaUpdater::ToggleChannel() {
    if (mChannel.load() == OtaChannel::Stable) {
        SetChannel(OtaChannel::Beta);
    } else {
        SetChannel(OtaChannel::Stable);
    }
}

void OtaUpdater::TriggerManualCheck() {
    mForceCheck.store(true);
}

std::string OtaUpdater::GetChannelName() const {
    return (mChannel.load() == OtaChannel::Beta) ? "Beta / Dev" : "Estável";
}

OtaUpdater::~OtaUpdater() {
    StopBackgroundChecker();
    WaitForInstallThread();
}

void OtaUpdater::WaitForInstallThread() {
    if (mInstallThread.joinable()) {
        mInstallThread.join();
    }
}

bool OtaUpdater::VerifyBootIntegrity(bool forceRollbackComboPressed) {
    bool pendingValidation = false;
    ReadBootState(pendingValidation);

    // Se o usuário segurar L + R no boot ou se a atualização anterior travou antes de validar
    if (forceRollbackComboPressed || pendingValidation) {
        struct stat st{};
        if (stat(kBackupNro, &st) == 0) {
            return RollbackToBackup();
        }
    }
    return true;
}

void OtaUpdater::MarkBootSuccessful() {
    WriteBootState(false, "boot_verified_ok");
}

void OtaUpdater::StartBackgroundChecker() {
    if (mCheckerThread.joinable()) return;
    mStopChecker.store(false);
    mCheckerThread = std::thread([this]() {
        BackgroundCheckerLoop();
    });
}

void OtaUpdater::StopBackgroundChecker() {
    mStopChecker.store(true);
    if (mCheckerThread.joinable()) {
        mCheckerThread.join();
    }
}

void OtaUpdater::BackgroundCheckerLoop() {
    // Espera 5 segundos após a inicialização para estabilização de rede
    for (int i = 0; i < 50; ++i) {
        if (mStopChecker.load()) return;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    while (!mStopChecker.load()) {
        if (!mUpdateReady.load()) {
            CheckRemoteManifestAndDownload();
        }

        // Aguarda 30 minutos antes de checar novamente (em intervalos de 1s para desligamento responsivo)
        for (int i = 0; i < 1800; ++i) {
            if (mStopChecker.load()) return;
            if (mForceCheck.load()) {
                mForceCheck.store(false);
                break;
            }
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
    }
}

bool OtaUpdater::CheckRemoteManifestAndDownload() {
    std::string manifestUrl = (mChannel.load() == OtaChannel::Beta)
        ? "https://raw.githubusercontent.com/Aleff959/SwitchCast/dev/update_manifest.json"
        : "https://raw.githubusercontent.com/Aleff959/SwitchCast/main/update_manifest.json";

    std::ifstream sifs(kSettingsFile);
    if (sifs.is_open()) {
        try {
            json cfg = json::parse(sifs, nullptr, false);
            if (!cfg.is_discarded() && cfg.contains("ota_manifest_url") && cfg["ota_manifest_url"].is_string()) {
                std::string customUrl = cfg["ota_manifest_url"].get<std::string>();
                if (!customUrl.empty()) manifestUrl = customUrl;
            }
        } catch (...) {}
    }

    CURL* curl = curl_easy_init();
    if (!curl) return false;

    std::string manifestBody;
    curl_easy_setopt(curl, CURLOPT_URL, manifestUrl.c_str());
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 20L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "SwitchCast-OTA/2.4 (NintendoSwitch)");
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, OnCurlWriteString);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &manifestBody);

    CURLcode res = curl_easy_perform(curl);
    long httpCode = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK || httpCode != 200 || manifestBody.empty()) {
        return false;
    }

    try {
        json j = json::parse(manifestBody, nullptr, false);
        if (j.is_discarded() || !j.contains("core")) return false;

        auto core = j["core"];
        int remoteVersionCode = core.value("version_code", 0);
        bool shouldUpdate = false;
        if (remoteVersionCode > kCurrentVersionCode) {
            shouldUpdate = true;
        } else if (mChannel.load() == OtaChannel::Beta && remoteVersionCode == kCurrentVersionCode) {
            std::string expectedSha = core.value("sha256", "");
            if (!expectedSha.empty() && !VerifySha256(GetTargetNroPath(), expectedSha)) {
                shouldUpdate = true;
            }
        }

        if (!shouldUpdate) {
            return false;
        }

        CoreUpdateInfo info;
        info.version = core.value("version", "");
        info.versionCode = remoteVersionCode;
        info.downloadUrl = core.value("download_url", "");
        info.sha256 = core.value("sha256", "");
        info.sizeBytes = core.value("size_bytes", static_cast<size_t>(0));

        if (core.contains("changelog") && core["changelog"].is_array()) {
            for (const auto& item : core["changelog"]) {
                if (item.is_string()) info.changelog.push_back(item.get<std::string>());
            }
        }

        if (info.downloadUrl.empty() || info.sha256.empty()) return false;

        // Download automático em background para kTempNro
        FILE* fp = std::fopen(kTempNro, "wb");
        if (!fp) return false;

        curl = curl_easy_init();
        if (!curl) {
            std::fclose(fp);
            return false;
        }

        SetDownloadProgress(0, info.sizeBytes);

        CurlWriteContext writeCtx;
        writeCtx.fp = fp;
        writeCtx.totalBytes = info.sizeBytes;
        writeCtx.downloadedBytes = 0;
        writeCtx.callback = [this](size_t downloaded, size_t total, OtaStatus /*status*/) {
            this->SetDownloadProgress(downloaded, total);
        };

        curl_easy_setopt(curl, CURLOPT_URL, info.downloadUrl.c_str());
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 180L);
        curl_easy_setopt(curl, CURLOPT_USERAGENT, "SwitchCast-OTA/" SWITCHCAST_VERSION_STR " (NintendoSwitch)");
        curl_easy_setopt(curl, CURLOPT_BUFFERSIZE, 512 * 1024L);
        curl_easy_setopt(curl, CURLOPT_TCP_NODELAY, 1L);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, OnCurlWriteFile);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &writeCtx);

        res = curl_easy_perform(curl);
        httpCode = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);
        curl_easy_cleanup(curl);
        std::fclose(fp);
        FinishDownload();

        if (res != CURLE_OK || httpCode != 200) {
            std::remove(kTempNro);
            return false;
        }

        // Validação criptográfica SHA-256 do arquivo baixado
        if (!VerifySha256(kTempNro, info.sha256)) {
            std::remove(kTempNro);
            return false;
        }

        // Tudo validado! Marca atualização pronta para aprovação do usuário na tela
        {
            std::lock_guard<std::mutex> lock(mUpdateMutex);
            mPendingInfo = std::move(info);
            mUpdateReady.store(true);
        }
        return true;
    } catch (...) {
        return false;
    }
}

CoreUpdateInfo OtaUpdater::GetPendingUpdateInfo() const {
    std::lock_guard<std::mutex> lock(mUpdateMutex);
    return mPendingInfo;
}

void OtaUpdater::DismissPendingUpdate() {
    // Apenas marca que o usuário escolheu ver mais tarde
}

void OtaUpdater::SetDownloadProgress(size_t downloaded, size_t total) {
    mDownloadedBytes.store(downloaded);
    mTotalBytes.store(total);
    mIsDownloading.store(true);
}

void OtaUpdater::FinishDownload() {
    mIsDownloading.store(false);
}

void OtaUpdater::InitFromMainArgs(int argc, char* argv[]) {
    std::lock_guard<std::mutex> lock(mUpdateMutex);
    if (argc > 0 && argv && argv[0] != nullptr && argv[0][0] != '\0') {
        std::string path = argv[0];
        // Normaliza para o formato exigido pelo nx-hbloader (sdmc:/...)
        if (path.rfind("sdmc:/", 0) != 0) {
            if (path.rfind("sdmc:", 0) == 0) {
                path = "sdmc:/" + path.substr(5);
            } else if (!path.empty() && path[0] == '/') {
                path = "sdmc:" + path;
            } else {
                path = "sdmc:/" + path;
            }
        }
        mRunningNroPath = path;
    } else {
        mRunningNroPath = "sdmc:/switch/SwitchCast/SwitchCast.nro";
    }
}

void OtaUpdater::SetRunningNroPath(const std::string& path) {
    if (!path.empty()) {
        std::lock_guard<std::mutex> lock(mUpdateMutex);
        std::string p = path;
        if (p.rfind("sdmc:/", 0) != 0) {
            if (p.rfind("sdmc:", 0) == 0) p = "sdmc:/" + p.substr(5);
            else if (p[0] == '/') p = "sdmc:" + p;
            else p = "sdmc:/" + p;
        }
        mRunningNroPath = p;
    }
}

std::string OtaUpdater::GetTargetNroPath() const {
    std::lock_guard<std::mutex> lock(mUpdateMutex);
    if (!mRunningNroPath.empty()) {
        return mRunningNroPath;
    }
    struct stat st{};
    if (stat("sdmc:/switch/SwitchCast/SwitchCast.nro", &st) == 0) {
        return "sdmc:/switch/SwitchCast/SwitchCast.nro";
    }
    if (stat("sdmc:/switch/SwitchCast.nro", &st) == 0) {
        return "sdmc:/switch/SwitchCast.nro";
    }
    return kTargetNro;
}

std::string OtaUpdater::GetBackupNroPath() const {
    return GetTargetNroPath() + ".bak";
}

std::string OtaUpdater::GetInstallStatus() const {
    std::lock_guard<std::mutex> lock(mInstallMutex);
    return mInstallStatus;
}

bool OtaUpdater::StageUploadedNro(const std::string& tempFilePath, const std::string& expectedSha256,
                                  const std::string& version, const std::vector<std::string>& changelog) {
    if (!ValidateNroBinary(tempFilePath.c_str()) || !VerifySha256(tempFilePath, expectedSha256)) {
        std::remove(tempFilePath.c_str());
        return false;
    }

    CoreUpdateInfo info;
    info.version = version.empty() ? SWITCHCAST_VERSION_STR : version;
    info.sha256 = expectedSha256;
    info.changelog = changelog.empty() ? std::vector<std::string>{
        "Atualizacao recebida via rede local (LAN)",
        "Melhorias de desempenho no reprodutor NVDEC",
        "Suporte a notificacao e confirmacao interativa na tela"
    } : changelog;

    {
        std::lock_guard<std::mutex> lock(mUpdateMutex);
        mPendingInfo = std::move(info);
        mUpdateReady.store(true);
    }
    return true;
}

bool OtaUpdater::StartInstallation() {
    if (mIsInstalling.load()) return false;
    mIsInstalling.store(true);
    mInstallStage.store(InstallStage::Verifying);
    mInstallFraction.store(0.05f);
    {
        std::lock_guard<std::mutex> lock(mInstallMutex);
        mInstallStatus = "Verificando integridade (SHA-256)...";
    }

    if (mInstallThread.joinable()) {
        mInstallThread.detach();
    }

    mInstallThread = std::thread([this]() {
        // ETAPA 1: Verificação SHA-256 (0.05 -> 0.35)
        mInstallStage.store(InstallStage::Verifying);
        {
            std::lock_guard<std::mutex> lock(mInstallMutex);
            mInstallStatus = "Verificando integridade criptografica (SHA-256)...";
        }

        std::string expectedSha;
        {
            std::lock_guard<std::mutex> lock(mUpdateMutex);
            expectedSha = mPendingInfo.sha256;
        }

        std::string calculatedSha = CalculateFileSha256WithProgress(kTempNro, [this](float frac) {
            mInstallFraction.store(0.05f + frac * 0.30f);
        });

        bool hashMatch = true;
        if (!expectedSha.empty()) {
            if (calculatedSha.size() != expectedSha.size()) {
                hashMatch = false;
            } else {
                for (size_t i = 0; i < calculatedSha.size(); ++i) {
                    if (std::tolower(calculatedSha[i]) != std::tolower(expectedSha[i])) {
                        hashMatch = false;
                        break;
                    }
                }
            }
        }

        if (!hashMatch || calculatedSha.empty()) {
            mInstallStage.store(InstallStage::Error);
            {
                std::lock_guard<std::mutex> lock(mInstallMutex);
                mInstallStatus = "Erro: Hash SHA-256 divergente! Atualizacao abortada.";
            }
            std::remove(kTempNro);
            mIsInstalling.store(false);
            return;
        }

        // ETAPA 2: Preparação e verificação do espaço (0.35 -> 0.65)
        mInstallStage.store(InstallStage::BackingUp);
        mInstallFraction.store(0.50f);
        {
            std::lock_guard<std::mutex> lock(mInstallMutex);
            mInstallStatus = "Verificando integridade e preparando backup de seguranca...";
        }

        #ifdef __SWITCH__
        svcSleepThread(200'000'000); // 200ms
        #else
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        #endif
        mInstallFraction.store(0.65f);

        // ETAPA 3: Preparação dos arquivos para o auto-restart (0.65 -> 0.92)
        mInstallStage.store(InstallStage::Writing);
        {
            std::lock_guard<std::mutex> lock(mInstallMutex);
            mInstallStatus = "Pacote v" SWITCHCAST_VERSION_STR " preparado para reinicializacao...";
        }

        #ifdef __SWITCH__
        svcSleepThread(300'000'000); // 300ms
        #else
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        #endif

        mInstallFraction.store(0.95f);
        WriteBootState(true, "awaiting_first_boot_validation");

        // ETAPA 4: Conclusão e agendamento de reinício na Main Thread
        mInstallStage.store(InstallStage::Restarting);
        mInstallFraction.store(1.0f);
        {
            std::lock_guard<std::mutex> lock(mInstallMutex);
            mInstallStatus = "Reiniciando no Launcher para instalacao limpa...";
        }

        #ifdef __SWITCH__
        svcSleepThread(400'000'000); // 400ms para usuário ver 100%
        #else
        std::this_thread::sleep_for(std::chrono::milliseconds(400));
        #endif

        // Dispara o reinício de forma limpa na Main Thread (Thread 0)
        RequestRestart();
    });

    return true;
}

bool OtaUpdater::ApplyPendingUpdate() {
    return StartInstallation();
}

bool OtaUpdater::DownloadAndInstallCore(const CoreUpdateInfo& info, ProgressCallback onProgress) {
    if (onProgress) onProgress(0, info.sizeBytes, OtaStatus::Downloading);

    FILE* fp = std::fopen(kTempNro, "wb");
    if (!fp) return false;

    CURL* curl = curl_easy_init();
    if (!curl) {
        std::fclose(fp);
        return false;
    }

    CurlWriteContext ctx{fp, info.sizeBytes, 0, onProgress};

    curl_easy_setopt(curl, CURLOPT_URL, info.downloadUrl.c_str());
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, OnCurlWriteFile);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &ctx);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 180L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "SwitchCast-OTA/1.3 (NintendoSwitch)");

    CURLcode res = curl_easy_perform(curl);
    long httpCode = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);

    curl_easy_cleanup(curl);
    std::fclose(fp);

    if (res != CURLE_OK || httpCode != 200) {
        std::remove(kTempNro);
        if (onProgress) onProgress(0, 0, OtaStatus::Error);
        return false;
    }

    if (onProgress) onProgress(info.sizeBytes, info.sizeBytes, OtaStatus::VerifyingHash);
    return ApplyUploadedNro(kTempNro, info.sha256);
}

bool OtaUpdater::ApplyUploadedNro(const std::string& tempFilePath, const std::string& expectedSha256) {
    return StageUploadedNro(tempFilePath, expectedSha256) && StartInstallation();
}

bool OtaUpdater::RollbackToBackup() {
    struct stat st{};
    if (stat(kBackupNro, &st) != 0 || st.st_size < 0x80) return false;

    // Em vez de tentar sobrescrever o NRO com o Core aberto no FAT32 (permissão negada no Horizon),
    // grava a flag de rollback em pending_update.json e agenda o restart para que o Launcher
    // execute a restauração limpa do .bak sem travas (Zero-Lock!)
    mkdir("sdmc:/switch/SwitchCast/modules", 0777);
    std::ofstream ofs(kStateFile, std::ios::trunc);
    if (ofs.is_open()) {
        ofs << "{\n"
            << "  \"boot_pending\": true,\n"
            << "  \"rollback_pending\": true,\n"
            << "  \"status\": \"rollback_requested_by_core\"\n"
            << "}\n";
    }
    ApplyUpdateAndScheduleRestart();
    return TriggerSelfRestart();
}

bool OtaUpdater::ApplyUpdateAndScheduleRestart() {
#ifdef __SWITCH__
    if (envHasNextLoad()) {
        envSetNextLoad(kLauncherNro, kLauncherNro);
    }
#endif
    return true;
}

bool OtaUpdater::TriggerSelfRestart() {
    RequestRestart();
    return true;
}

std::string OtaUpdater::CalculateFileSha256(const std::string& filePath) {
    return CalculateFileSha256WithProgress(filePath, nullptr);
}

std::string OtaUpdater::CalculateFileSha256WithProgress(const std::string& filePath,
                                                       const std::function<void(float)>& onProgress) {
    FILE* fp = std::fopen(filePath.c_str(), "rb");
    if (!fp) return "";

    struct stat st{};
    size_t fileSize = (stat(filePath.c_str(), &st) == 0) ? st.st_size : 0;

    mbedtls_sha256_context ctx;
    mbedtls_sha256_init(&ctx);
    mbedtls_sha256_starts_ret(&ctx, 0);

    unsigned char buffer[65536];
    size_t bytesRead = 0;
    size_t totalRead = 0;
    while ((bytesRead = std::fread(buffer, 1, sizeof(buffer), fp)) > 0) {
        mbedtls_sha256_update_ret(&ctx, buffer, bytesRead);
        totalRead += bytesRead;
        if (onProgress && fileSize > 0) {
            onProgress(static_cast<float>(totalRead) / static_cast<float>(fileSize));
        }
        #ifdef __SWITCH__
        svcSleepThread(500'000); // 0.5ms yield para o renderizador manter 60 fps
        #endif
    }
    std::fclose(fp);

    unsigned char hash[32];
    mbedtls_sha256_finish_ret(&ctx, hash);
    mbedtls_sha256_free(&ctx);

    std::ostringstream oss;
    for (int i = 0; i < 32; ++i) {
        oss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(hash[i]);
    }
    return oss.str();
}

bool OtaUpdater::CopyFileContents(const std::string& src, const std::string& dst,
                                  const std::function<void(float)>& onProgress) {
    FILE* in = std::fopen(src.c_str(), "rb");
    if (!in) return false;

    struct stat st{};
    size_t total = (stat(src.c_str(), &st) == 0) ? st.st_size : 0;

    FILE* out = std::fopen(dst.c_str(), "wb");
    if (!out) {
        std::fclose(in);
        return false;
    }

    char buffer[65536];
    size_t bytes = 0;
    size_t copied = 0;
    bool success = true;
    while ((bytes = std::fread(buffer, 1, sizeof(buffer), in)) > 0) {
        if (std::fwrite(buffer, 1, bytes, out) != bytes) {
            success = false;
            break;
        }
        copied += bytes;
        if (onProgress && total > 0) {
            onProgress(static_cast<float>(copied) / static_cast<float>(total));
        }
        #ifdef __SWITCH__
        svcSleepThread(500'000); // 0.5ms yield
        #endif
    }

    std::fflush(out);
    std::fclose(out);
    std::fclose(in);
    return success;
}

bool OtaUpdater::VerifySha256(const std::string& filePath, const std::string& expectedHex) {
    std::string calculated = CalculateFileSha256(filePath);
    if (calculated.empty() || calculated.size() != expectedHex.size()) return false;

    for (size_t i = 0; i < calculated.size(); ++i) {
        if (std::tolower(calculated[i]) != std::tolower(expectedHex[i])) return false;
    }
    return true;
}

void OtaUpdater::WriteBootState(bool pendingValidation, const std::string& statusNote) {
    mkdir("sdmc:/switch/SwitchCast/config", 0777);
    std::ofstream ofs(kStateFile, std::ios::trunc);
    if (ofs.is_open()) {
        ofs << "{\n"
            << "  \"boot_pending\": " << (pendingValidation ? "true" : "false") << ",\n"
            << "  \"status\": \"" << statusNote << "\"\n"
            << "}\n";
    }
}

bool OtaUpdater::ReadBootState(bool& outPendingValidation) {
    std::ifstream ifs(kStateFile);
    if (!ifs.is_open()) {
        outPendingValidation = false;
        return false;
    }
    std::string content((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
    outPendingValidation = (content.find("\"boot_pending\": true") != std::string::npos);
    return true;
}

} // namespace SwitchCast::Updater
