#include "ServerBridge.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "Upnp.hpp"
#include "WebRemote.hpp"
#include "AppIcon.hpp"
#include "../utils/Log.hpp"
#include "../utils/Platform.hpp"
#include "../utils/Text.hpp"
#include "updater/OtaUpdater.hpp"
#include "updater/ModuleManager.hpp"
#include <sys/stat.h>
#ifdef __SWITCH__
#include <switch.h>
#endif

namespace SwitchCast {

using json = nlohmann::json;

namespace {

constexpr std::size_t kMaxBodyBytes = 256 * 1024;

std::string ReadBody(mg_connection* conn) {
    std::string body;
    char buf[2048];
    int n;
    while (body.size() < kMaxBodyBytes && (n = mg_read(conn, buf, sizeof buf)) > 0) body.append(buf, n);
    return body;
}

bool StreamBodyToFile(mg_connection* conn, const std::string& destinationPath,
                      const std::function<void(size_t, size_t)>& onProgress = nullptr) {
    FILE* fp = std::fopen(destinationPath.c_str(), "wb");
    if (!fp) return false;

    const char* clHeader = mg_get_header(conn, "Content-Length");
    size_t totalBytes = clHeader ? static_cast<size_t>(std::strtoull(clHeader, nullptr, 10)) : 0;

    char buf[65536];
    int n = 0;
    bool hadError = false;
    size_t downloaded = 0;

    while ((n = mg_read(conn, buf, sizeof(buf))) > 0) {
        if (std::fwrite(buf, 1, n, fp) != static_cast<size_t>(n)) {
            hadError = true;
            break;
        }
        downloaded += n;
        if (onProgress) {
            onProgress(downloaded, totalBytes);
        }
        // Micro-sleep de 1ms para liberar o barramento SDMMC do Tegra X1
        // e manter a tela 100% fluida a 60 fps durante o download
        #ifdef __SWITCH__
        svcSleepThread(1'000'000);
        #else
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        #endif
    }
    if (n < 0) hadError = true;
    std::fflush(fp);
    #ifdef __SWITCH__
    fsync(fileno(fp));
    #endif
    std::fclose(fp);
    #ifdef __SWITCH__
    fsdevCommitDevice("sdmc");
    #endif
    if (hadError) {
        std::remove(destinationPath.c_str());
        return false;
    }
    return true;
}

void SendResponse(mg_connection* conn, int status, const char* contentType, const std::string& body,
                  const std::string& extraHeaders = {}) {
    const char* text = status == 200 ? "OK" : status == 204 ? "No Content" : status == 400 ? "Bad Request"
                       : status == 404 ? "Not Found" : status == 405 ? "Method Not Allowed"
                       : status == 503 ? "Service Unavailable" : "Internal Server Error";
    mg_printf(conn,
              "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\nConnection: close\r\n"
              "Access-Control-Allow-Origin: *\r\nAccess-Control-Allow-Headers: Content-Type\r\n"
              "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\nServer: SwitchCast/" SWITCHCAST_VERSION_STR "\r\n%s\r\n",
              status, text, contentType, body.size(), extraHeaders.c_str());
    if (!body.empty()) mg_write(conn, body.data(), body.size());
}

json SnapshotToJson(const PlayerSnapshot& s) {
    return json{{"state", ToString(s.state)}, {"positionMs", s.positionMs}, {"durationMs", s.durationMs},
                {"volume", s.volume},         {"muted", s.muted},           {"buffering", s.buffering},
                {"title", s.title},           {"url", s.url},               {"error", s.lastError}};
}

// Converte um JSON de controle (REST ou WebSocket) em NetworkCommand.
bool ParseCommand(const std::string& name, const json& j, NetworkCommand& cmd, std::string& err) {
    auto num = [&](const char* k, int64_t def) { return j.contains(k) && j[k].is_number() ? j[k].get<int64_t>() : def; };

    if (name == "play" && j.contains("url")) {
        if (!j["url"].is_string() || j["url"].get<std::string>().empty()) { err = "url inválida"; return false; }
        cmd.type = NetworkCommand::Type::LoadMedia;
        cmd.media.url = j["url"].get<std::string>();
        if (j.contains("title") && j["title"].is_string()) cmd.media.title = j["title"].get<std::string>();
        cmd.media.startPositionMs = num("startPositionMs", 0);
        cmd.media.autoPlay = !j.contains("autoPlay") || j["autoPlay"].get<bool>();
        if (j.contains("headers") && j["headers"].is_object())
            for (auto it = j["headers"].begin(); it != j["headers"].end(); ++it)
                if (it.value().is_string()) cmd.media.headers[it.key()] = it.value().get<std::string>();
        if (j.contains("subtitles") && j["subtitles"].is_array())
            for (const auto& s : j["subtitles"])
                if (s.is_string()) cmd.media.subtitles.push_back(s.get<std::string>());
        return true;
    }
    if (name == "play" || name == "resume") { cmd.type = NetworkCommand::Type::Play;  return true; }
    if (name == "pause")                    { cmd.type = NetworkCommand::Type::Pause; return true; }
    if (name == "stop")                     { cmd.type = NetworkCommand::Type::Stop;  return true; }
    if (name == "seek") {
        if (j.contains("positionMs")) {
            cmd.type = NetworkCommand::Type::Seek;
            cmd.positionMs = num("positionMs", 0);
        } else if (j.contains("offsetMs")) {
            cmd.type = NetworkCommand::Type::SeekRelative;
            cmd.positionMs = num("offsetMs", 0);
        } else { err = "informe positionMs ou offsetMs"; return false; }
        return true;
    }
    if (name == "volume") {
        if (!j.contains("level") || !j["level"].is_number()) { err = "informe level (0-150)"; return false; }
        cmd.type = NetworkCommand::Type::SetVolume;
        cmd.volume = static_cast<int>(std::max<int64_t>(0, std::min<int64_t>(150, num("level", 100))));
        return true;
    }
    if (name == "mute") {
        cmd.type = NetworkCommand::Type::SetMute;
        cmd.flag = !j.contains("mute") || j["mute"].get<bool>();
        return true;
    }
    if (name == "subtitle") {
        if (!j.contains("url") || !j["url"].is_string()) { err = "informe url"; return false; }
        cmd.type = NetworkCommand::Type::AddSubtitle;
        cmd.text = j["url"].get<std::string>();
        return true;
    }
    if (name == "cycleAudio") {
        cmd.type = NetworkCommand::Type::CycleAudio;
        return true;
    }
    if (name == "cycleSubtitle" || name == "cycleSub") {
        cmd.type = NetworkCommand::Type::CycleSubtitle;
        return true;
    }
    if (name == "setAudioTrack") {
        cmd.type = NetworkCommand::Type::CycleAudio;
        return true;
    }
    if (name == "setSubtitleTrack") {
        cmd.type = NetworkCommand::Type::CycleSubtitle;
        return true;
    }
    err = "comando desconhecido: " + name;
    return false;
}

}  // namespace

ServerBridge& ServerBridge::Instance() {
    static ServerBridge instance;
    return instance;
}

std::string ServerBridge::CurrentIp() const { return Platform::GetLocalIp(); }

bool ServerBridge::QueueCommand(NetworkCommand cmd) { return mCommands.Push(std::move(cmd)); }
bool ServerBridge::PopCommand(NetworkCommand& out) { return mCommands.Pop(out); }

bool ServerBridge::Start(std::shared_ptr<MpvPlayer> player, int httpPort, int wsPort) {
    if (mRunning) return true;
    mPlayer = std::move(player);
    mIdentity = Platform::LoadDeviceIdentity(httpPort, wsPort);

    mUpnp = std::make_unique<UpnpService>([this](NetworkCommand c) { return QueueCommand(std::move(c)); },
                                          [this] { return mPlayer->Snapshot(); });

    mg_init_library(0);
    const std::string ports = httpPort == wsPort ? std::to_string(httpPort)
                                                 : std::to_string(httpPort) + "," + std::to_string(wsPort);
    const char* options[] = {"listening_ports", ports.c_str(),
                             "num_threads", "8",
                             "enable_keep_alive", "no",
                             "request_timeout_ms", "180000",
                             "websocket_timeout_ms", "3600000",
                             nullptr};
    mg_callbacks callbacks{};
    mHttpCtx = mg_start(&callbacks, this, options);
    if (!mHttpCtx) {
        SC_LOG("civetweb: falha ao abrir portas %s", ports.c_str());
        mg_exit_library();
        return false;
    }
    mg_set_request_handler(mHttpCtx, "/upnp/**", &ServerBridge::HttpUpnp, this);
    mg_set_request_handler(mHttpCtx, "/api/v1/**", &ServerBridge::HttpApi, this);
    mg_set_request_handler(mHttpCtx, "/", &ServerBridge::HttpWebRemote, this);
    mg_set_request_handler(mHttpCtx, "/index.html", &ServerBridge::HttpWebRemote, this);
    mg_set_request_handler(mHttpCtx, "/remote", &ServerBridge::HttpWebRemote, this);
    mg_set_request_handler(mHttpCtx, "/icon.jpg", &ServerBridge::HttpWebRemote, this);
    mg_set_request_handler(mHttpCtx, "/favicon.ico", &ServerBridge::HttpWebRemote, this);
    mg_set_websocket_handler(mHttpCtx, "/ws", &ServerBridge::WsConnect, &ServerBridge::WsReady,
                             &ServerBridge::WsData, &ServerBridge::WsClose, this);

    mRunning = true;
    mSsdp = std::make_unique<SsdpServer>(mIdentity, [] { return Platform::GetLocalIp(); });
    mSsdp->Start();
    mBroadcastThread = std::thread([this] { BroadcastLoop(); });
    return true;
}

void ServerBridge::Stop() {
    if (!mRunning.exchange(false)) return;
    if (mSsdp) mSsdp->Stop();
    if (mBroadcastThread.joinable()) mBroadcastThread.join();
    if (mHttpCtx) {
        mg_stop(mHttpCtx);
        mHttpCtx = nullptr;
    }
    mg_exit_library();
    {
        std::lock_guard<std::mutex> lock(mWsMutex);
        mWsClients.clear();
    }
    mCommands.Clear();
}

void ServerBridge::OnSleep() { mAsleep = true; }

void ServerBridge::OnResume() {
    mAsleep = false;
    // O Wi-Fi pode ter mudado de IP/socket durante o sleep: reanuncia via SSDP.
    if (mSsdp) mSsdp->RequestReannounce();
}

// ---------------------------------------------------------------------------
// HTTP: UPnP
// ---------------------------------------------------------------------------
int ServerBridge::HttpUpnp(mg_connection* conn, void* self) {
    return static_cast<ServerBridge*>(self)->HandleUpnp(conn);
}

int ServerBridge::HandleUpnp(mg_connection* conn) {
    const mg_request_info* ri = mg_get_request_info(conn);
    const std::string path = ri->local_uri ? ri->local_uri : "";
    const std::string method = ri->request_method ? ri->request_method : "";
    constexpr const char* kXml = "text/xml; charset=\"utf-8\"";

    if (method == "GET" && path == "/upnp/description.xml") {
        DeviceIdentity id = mIdentity;
        SendResponse(conn, 200, kXml, Upnp::BuildDeviceDescription(id), "EXT:\r\n");
        return 200;
    }

    Upnp::Service svc{};
    auto serviceFrom = [&](const std::string& prefix, const std::string& suffix) {
        if (path.compare(0, prefix.size(), prefix) != 0) return false;
        std::string name = path.substr(prefix.size());
        if (!suffix.empty()) {
            if (name.size() <= suffix.size() || name.compare(name.size() - suffix.size(), suffix.size(), suffix) != 0)
                return false;
            name.resize(name.size() - suffix.size());
        }
        return Upnp::ServiceFromName(name, svc);
    };

    if (method == "GET" && serviceFrom("/upnp/scpd/", ".xml")) {
        SendResponse(conn, 200, kXml, Upnp::BuildScpd(svc));
        return 200;
    }
    if (method == "POST" && serviceFrom("/upnp/control/", "")) {
        const char* action = mg_get_header(conn, "SOAPAction");
        const std::string body = ReadBody(conn);
        const UpnpService::Result r = mUpnp->HandleControl(svc, action ? action : "", body);
        SendResponse(conn, r.httpStatus, kXml, r.body, "EXT:\r\n");
        return r.httpStatus;
    }
    if (serviceFrom("/upnp/event/", "")) {
        // Eventing mínimo: aceita a assinatura, mas os controladores consultam via polling.
        if (method == "SUBSCRIBE") {
            SendResponse(conn, 200, "text/plain", "",
                         "SID: uuid:" + mIdentity.uuid + "-sub\r\nTIMEOUT: Second-1800\r\n");
        } else {
            SendResponse(conn, 200, "text/plain", "");
        }
        return 200;
    }
    SendResponse(conn, 404, "text/plain", "Not Found");
    return 404;
}

// ---------------------------------------------------------------------------
// HTTP: REST /api/v1
// ---------------------------------------------------------------------------
int ServerBridge::HttpApi(mg_connection* conn, void* self) {
    return static_cast<ServerBridge*>(self)->HandleApi(conn);
}

std::string ServerBridge::HandleCycleAudio(bool& ok) {
    std::string track = mPlayer ? mPlayer->CycleAudioTrack() : "Audio indisponivel";
    NetworkCommand cmd;
    cmd.type = NetworkCommand::Type::CycleAudio;
    cmd.text = track;
    QueueCommand(std::move(cmd));
    ok = true;
    return json{{"ok", true}, {"track", track}}.dump();
}

std::string ServerBridge::HandleCycleSubtitle(bool& ok) {
    std::string track = mPlayer ? mPlayer->CycleSubtitleTrack() : "Legendas indisponiveis";
    NetworkCommand cmd;
    cmd.type = NetworkCommand::Type::CycleSubtitle;
    cmd.text = track;
    QueueCommand(std::move(cmd));
    ok = true;
    return json{{"ok", true}, {"track", track}}.dump();
}

std::string ServerBridge::HandleStatus() {
    json s = SnapshotToJson(mPlayer ? mPlayer->Snapshot() : PlayerSnapshot{});
    std::string activeAudio = mPlayer ? mPlayer->GetCurrentAudioTrackName() : "";
    std::string activeSub = mPlayer ? mPlayer->GetCurrentSubtitleTrackName() : "";
    s["active_audio"] = activeAudio;
    s["active_subtitle"] = activeSub;
    s["audioTrack"] = activeAudio;
    s["subtitleTrack"] = activeSub;

    json audioTracks = json::array();
    json subTracks = json::array();
    if (mPlayer) {
        for (const auto& t : mPlayer->GetTrackList("audio")) {
            audioTracks.push_back({
                {"id", t.id}, {"type", t.type}, {"title", t.title},
                {"lang", t.lang}, {"codec", t.codec}, {"selected", t.selected}
            });
        }
        for (const auto& t : mPlayer->GetTrackList("sub")) {
            subTracks.push_back({
                {"id", t.id}, {"type", t.type}, {"title", t.title},
                {"lang", t.lang}, {"codec", t.codec}, {"selected", t.selected}
            });
        }
    }
    s["audio_tracks"] = audioTracks;
    s["subtitle_tracks"] = subTracks;
    return s.dump();
}

std::string ServerBridge::HandleJsonCommand(const std::string& name, const std::string& body, bool& ok) {
    json j = json::object();
    if (!Text::Trim(body).empty()) {
        j = json::parse(body, nullptr, /*allow_exceptions=*/false);
        if (j.is_discarded() || !j.is_object()) {
            ok = false;
            return json{{"ok", false}, {"error", "JSON inválido"}}.dump();
        }
    }
    if (name == "cycleAudio") {
        return HandleCycleAudio(ok);
    }
    if (name == "cycleSubtitle" || name == "cycleSub") {
        return HandleCycleSubtitle(ok);
    }
    if (name == "setAudioTrack") {
        int trackId = j.contains("id") && j["id"].is_number() ? j["id"].get<int>() : 1;
        if (mPlayer) mPlayer->SetAudioTrack(trackId);
        ok = true;
        return json{{"ok", true}}.dump();
    }
    if (name == "setSubtitleTrack") {
        int trackId = j.contains("id") && j["id"].is_number() ? j["id"].get<int>() : 0;
        if (mPlayer) mPlayer->SetSubtitleTrack(trackId);
        ok = true;
        return json{{"ok", true}}.dump();
    }
    NetworkCommand cmd;
    std::string err;
    if (!ParseCommand(name, j, cmd, err)) {
        ok = false;
        return json{{"ok", false}, {"error", err}}.dump();
    }
    ok = QueueCommand(std::move(cmd));
    return json{{"ok", ok}, {"error", ok ? "" : "fila cheia"}}.dump();
}

int ServerBridge::HandleApi(mg_connection* conn) {
    const mg_request_info* ri = mg_get_request_info(conn);
    const std::string path = ri->local_uri ? ri->local_uri : "";
    const std::string method = ri->request_method ? ri->request_method : "";
    const std::string route = path.size() > 8 ? path.substr(8) : "";  // após "/api/v1/"
    constexpr const char* kJson = "application/json; charset=utf-8";

    if (method == "OPTIONS") { SendResponse(conn, 204, kJson, ""); return 204; }

    if (method == "GET" && route == "status") {
        SendResponse(conn, 200, kJson, HandleStatus());
        return 200;
    }
    if (method == "GET" && route == "info") {
        auto& updater = Updater::OtaUpdater::Instance();
        json info{{"name", mIdentity.friendlyName}, {"author", "Aleff959"},
                  {"description", "Receptor de mídia e streaming de alto desempenho para Nintendo Switch OLED com aceleração NVDEC"},
                  {"uuid", mIdentity.uuid}, {"ip", CurrentIp()},
                  {"httpPort", mIdentity.httpPort}, {"wsPort", mIdentity.wsPort},
                  {"version", SWITCHCAST_VERSION_STR},
                  {"build", SWITCHCAST_BUILD_NUM},
                  {"versionFull", SWITCHCAST_VERSION_FULL_STR},
                  {"otaChannel", updater.GetChannelName()}};
        SendResponse(conn, 200, kJson, info.dump());
        return 200;
    }
    if (method == "GET" && (route == "ota/channel" || route == "system/ota-channel")) {
        auto& updater = Updater::OtaUpdater::Instance();
        bool isBeta = (updater.GetChannel() == Updater::OtaChannel::Beta);
        json res{
            {"ok", true},
            {"channel", isBeta ? "beta" : "stable"},
            {"name", updater.GetChannelName()},
            {"manifestUrl", isBeta
                ? "https://raw.githubusercontent.com/Aleff959/SwitchCast/dev/update_manifest.json"
                : "https://raw.githubusercontent.com/Aleff959/SwitchCast/main/update_manifest.json"}
        };
        SendResponse(conn, 200, kJson, res.dump());
        return 200;
    }
    if (method == "GET" && route == "modules") {
        auto modules = Updater::ModuleManager::Instance().GetInstalledModules();
        json jModules = json::array();
        for (const auto& m : modules) {
            jModules.push_back({
                {"id", m.id},
                {"name", m.name},
                {"version", m.version},
                {"entryFile", m.entryFile},
                {"sha256", m.sha256},
                {"hotReload", m.hotReload},
                {"enabled", m.enabled}
            });
        }
        SendResponse(conn, 200, kJson, json{{"ok", true}, {"modules", jModules}}.dump());
        return 200;
    }

    if (method != "POST") { SendResponse(conn, 405, kJson, R"({"ok":false,"error":"use POST"})"); return 405; }

    if (route == "ota/channel" || route == "system/ota-channel") {
        auto& updater = Updater::OtaUpdater::Instance();
        std::string body = ReadBody(conn);
        std::string target = "";
        if (!Text::Trim(body).empty()) {
            try {
                json j = json::parse(body, nullptr, false);
                if (j.contains("channel") && j["channel"].is_string()) {
                    target = j["channel"].get<std::string>();
                }
            } catch (...) {}
        }

        if (target == "beta" || target == "dev") {
            updater.SetChannel(Updater::OtaChannel::Beta);
        } else if (target == "stable" || target == "main") {
            updater.SetChannel(Updater::OtaChannel::Stable);
        } else {
            updater.ToggleChannel();
        }

        bool isBeta = (updater.GetChannel() == Updater::OtaChannel::Beta);
        json res{
            {"ok", true},
            {"channel", isBeta ? "beta" : "stable"},
            {"name", updater.GetChannelName()}
        };
        SendResponse(conn, 200, kJson, res.dump());
        return 200;
    }

    if (route == "system/rollback") {
        bool success = Updater::OtaUpdater::Instance().RollbackToBackup();
        if (success) {
            SendResponse(conn, 200, kJson, R"({"ok":true,"message":"Rollback concluido. Reiniciando Switch..."})");
            return 200;
        } else {
            SendResponse(conn, 500, kJson, R"({"ok":false,"error":"Falha ao executar rollback. Backup nao encontrado."})");
            return 500;
        }
    }

    if (route == "system/ota-update") {
        const char* expectedSha = mg_get_header(conn, "X-Expected-Sha256");
        const std::string sha = expectedSha ? Text::Trim(expectedSha) : "";
        if (sha.empty()) {
            SendResponse(conn, 400, kJson, R"({"ok":false,"error":"Cabecalho X-Expected-Sha256 ausente."})");
            return 400;
        }

        const std::string tempNro = "sdmc:/switch/SwitchCast/modules/update.nro";
        const char* clHeader = mg_get_header(conn, "Content-Length");
        size_t totalBytes = clHeader ? static_cast<size_t>(std::strtoull(clHeader, nullptr, 10)) : 0;
        Updater::OtaUpdater::Instance().SetDownloadProgress(0, totalBytes);

        bool streamOk = StreamBodyToFile(conn, tempNro, [](size_t down, size_t total) {
            Updater::OtaUpdater::Instance().SetDownloadProgress(down, total);
        });
        Updater::OtaUpdater::Instance().FinishDownload();

        if (!streamOk) {
            SendResponse(conn, 500, kJson, R"({"ok":false,"error":"Falha ao gravar SwitchCast.nro.tmp no SD."})");
            return 500;
        }

        const char* hVer = mg_get_header(conn, "X-Version");
        const char* hChange = mg_get_header(conn, "X-Changelog");
        const char* hInstant = mg_get_header(conn, "X-Instant-Reboot");

        std::string ver = hVer ? Text::Trim(hVer) : SWITCHCAST_VERSION_STR;
        std::vector<std::string> changelog;
        if (hChange) {
            std::string raw = hChange;
            std::stringstream ss(raw);
            std::string item;
            while (std::getline(ss, item, ';')) {
                std::string t = Text::Trim(item);
                if (!t.empty()) changelog.push_back(t);
            }
        }

        bool instant = hInstant && (Text::EqualsIgnoreCase(hInstant, "true") || Text::EqualsIgnoreCase(hInstant, "1"));
        bool ok = false;
        if (instant) {
            ok = Updater::OtaUpdater::Instance().ApplyUploadedNro(tempNro, sha);
        } else {
            ok = Updater::OtaUpdater::Instance().StageUploadedNro(tempNro, sha, ver, changelog);
        }

        if (ok) {
            if (instant) {
                SendResponse(conn, 200, kJson, R"({"ok":true,"message":"Atualizacao OTA instalada com sucesso! Reiniciando..."})");
            } else {
                SendResponse(conn, 200, kJson, R"({"ok":true,"message":"Atualizacao recebida e validada! Aviso exibido na tela do Switch para o usuario confirmar."})");
            }
            return 200;
        } else {
            SendResponse(conn, 400, kJson, R"({"ok":false,"error":"Verificacao SHA-256 falhou ou erro ao gravar atualizacao."})");
            return 400;
        }
    }

    if (route == "system/ota-apply") {
        bool ok = Updater::OtaUpdater::Instance().ApplyPendingUpdate();
        if (ok) {
            SendResponse(conn, 200, kJson, R"({"ok":true,"message":"Aplicando atualizacao e reiniciando o Switch..."})");
            return 200;
        } else {
            SendResponse(conn, 400, kJson, R"({"ok":false,"error":"Nenhuma atualizacao pendente para aplicar."})");
            return 400;
        }
    }

    if (route == "modules/install") {
        const char* hId = mg_get_header(conn, "X-Module-Id");
        const char* hVer = mg_get_header(conn, "X-Module-Version");
        const char* hEntry = mg_get_header(conn, "X-Entry-File");
        const char* hSha = mg_get_header(conn, "X-Expected-Sha256");

        std::string modId = hId ? Text::Trim(hId) : "";
        std::string modVer = hVer ? Text::Trim(hVer) : "1.0.0";
        std::string modEntry = hEntry ? Text::Trim(hEntry) : "";
        std::string modSha = hSha ? Text::Trim(hSha) : "";

        if (modId.empty() || modEntry.empty() || modSha.empty()) {
            SendResponse(conn, 400, kJson, R"({"ok":false,"error":"Headers X-Module-Id, X-Entry-File e X-Expected-Sha256 sao obrigatorios."})");
            return 400;
        }

        mkdir("sdmc:/switch/SwitchCast/modules", 0777);
        const std::string tempMod = "sdmc:/switch/SwitchCast/modules/temp_upload.tmp";
        if (!StreamBodyToFile(conn, tempMod)) {
            SendResponse(conn, 500, kJson, R"({"ok":false,"error":"Falha ao gravar arquivo temporario do modulo."})");
            return 500;
        }

        Updater::ModuleDescriptor desc;
        desc.id = modId;
        desc.name = modId;
        desc.version = modVer;
        desc.entryFile = modEntry;
        desc.sha256 = modSha;
        desc.hotReload = true;
        desc.enabled = true;

        bool ok = Updater::ModuleManager::Instance().InstallOrUpdateModule(desc, tempMod);
        if (ok) {
            SendResponse(conn, 200, kJson, R"({"ok":true,"message":"Modulo instalado e hot-reload executado com sucesso."})");
            return 200;
        } else {
            SendResponse(conn, 400, kJson, R"({"ok":false,"error":"Falha na validacao SHA-256 ou instalacao do modulo."})");
            return 400;
        }
    }

    bool ok = false;
    const std::string reply = HandleJsonCommand(route, ReadBody(conn), ok);
    const int status = ok ? 200 : (reply.find("fila cheia") != std::string::npos ? 503 : 400);
    SendResponse(conn, status, kJson, reply);
    return status;
}

// ---------------------------------------------------------------------------
// HTTP: Web Remote
namespace {

int ServeWallpaper(mg_connection* conn, const mg_request_info* ri) {
    static const char* kCandidatePaths[] = {
        "sdmc:/switch/SwitchCast/wallpaper.mp4",
        "romfs:/wallpaper.mp4",
        "switch/SwitchCast/wallpaper.mp4",
        "wallpaper.mp4",
        "./wallpaper.mp4"
    };

    FILE* f = nullptr;
    for (const char* p : kCandidatePaths) {
        f = std::fopen(p, "rb");
        if (f) break;
    }

    if (!f) {
        SendResponse(conn, 404, "text/plain", "Wallpaper not found");
        return 404;
    }

    std::fseek(f, 0, SEEK_END);
    const long fileSize = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);

    if (fileSize <= 0) {
        std::fclose(f);
        SendResponse(conn, 500, "text/plain", "Invalid wallpaper file");
        return 500;
    }

    const char* rangeHdr = nullptr;
    for (int i = 0; i < ri->num_headers; ++i) {
        if (strcasecmp(ri->http_headers[i].name, "Range") == 0) {
            rangeHdr = ri->http_headers[i].value;
            break;
        }
    }

    long startByte = 0;
    long endByte = fileSize - 1;
    bool isPartial = false;

    if (rangeHdr && std::strncmp(rangeHdr, "bytes=", 6) == 0) {
        const char* p = rangeHdr + 6;
        char* endPtr = nullptr;
        long s = std::strtol(p, &endPtr, 10);
        if (endPtr != p) {
            startByte = s;
            if (*endPtr == '-') {
                if (*(endPtr + 1) != '\0') {
                    long e = std::strtol(endPtr + 1, nullptr, 10);
                    if (e >= startByte && e < fileSize) endByte = e;
                }
            }
            isPartial = true;
        }
    }

    if (startByte < 0 || startByte >= fileSize) {
        std::fclose(f);
        mg_printf(conn,
                  "HTTP/1.1 416 Range Not Satisfiable\r\n"
                  "Content-Range: bytes */%ld\r\nConnection: close\r\n\r\n",
                  fileSize);
        return 416;
    }

    const long contentLength = endByte - startByte + 1;
    std::fseek(f, startByte, SEEK_SET);

    if (isPartial) {
        mg_printf(conn,
                  "HTTP/1.1 206 Partial Content\r\n"
                  "Content-Type: video/mp4\r\n"
                  "Content-Length: %ld\r\n"
                  "Content-Range: bytes %ld-%ld/%ld\r\n"
                  "Accept-Ranges: bytes\r\n"
                  "Cache-Control: public, max-age=86400\r\n"
                  "Connection: close\r\n\r\n",
                  contentLength, startByte, endByte, fileSize);
    } else {
        mg_printf(conn,
                  "HTTP/1.1 200 OK\r\n"
                  "Content-Type: video/mp4\r\n"
                  "Content-Length: %ld\r\n"
                  "Accept-Ranges: bytes\r\n"
                  "Cache-Control: public, max-age=86400\r\n"
                  "Connection: close\r\n\r\n",
                  fileSize);
    }

    char buf[32768];
    long remaining = contentLength;
    while (remaining > 0) {
        size_t toRead = static_cast<size_t>(std::min<long>(remaining, sizeof(buf)));
        size_t n = std::fread(buf, 1, toRead, f);
        if (n == 0) break;
        int sent = mg_write(conn, buf, n);
        if (sent <= 0) break;
        remaining -= sent;
    }

    std::fclose(f);
    return isPartial ? 206 : 200;
}

}  // namespace

// ---------------------------------------------------------------------------
int ServerBridge::HttpWebRemote(mg_connection* conn, void* self) {
    return static_cast<ServerBridge*>(self)->HandleWebRemote(conn);
}

int ServerBridge::HandleWebRemote(mg_connection* conn) {
    const mg_request_info* ri = mg_get_request_info(conn);
    const std::string path = ri->local_uri ? ri->local_uri : "/";
    const std::string method = ri->request_method ? ri->request_method : "GET";

    if (method == "OPTIONS") {
        SendResponse(conn, 204, "text/plain", "");
        return 204;
    }

    if (path == "/" || path == "/index.html" || path == "/remote") {
        SendResponse(conn, 200, "text/html; charset=utf-8", GetWebRemoteHtml());
        return 200;
    }

    if (path == "/icon.jpg" || path == "/favicon.ico") {
        mg_printf(conn,
                  "HTTP/1.1 200 OK\r\nContent-Type: image/jpeg\r\nContent-Length: %zu\r\n"
                  "Cache-Control: public, max-age=86400\r\nConnection: close\r\n\r\n",
                  kAppIconJpgSize);
        mg_write(conn, kAppIconJpg, kAppIconJpgSize);
        return 200;
    }

    if (path == "/wallpaper.mp4") {
        return ServeWallpaper(conn, ri);
    }

    SendResponse(conn, 404, "text/plain", "Not Found");
    return 404;
}

// ---------------------------------------------------------------------------
// WebSocket /ws
// ---------------------------------------------------------------------------
int ServerBridge::WsConnect(const mg_connection*, void*) { return 0; }  // 0 = aceita

void ServerBridge::WsReady(mg_connection* conn, void* self) {
    auto* bridge = static_cast<ServerBridge*>(self);
    {
        std::lock_guard<std::mutex> lock(bridge->mWsMutex);
        bridge->mWsClients[conn] = conn;
    }
    json hello = SnapshotToJson(bridge->mPlayer->Snapshot());
    hello["event"] = "status";
    bridge->WsSend(conn, hello.dump());
}

int ServerBridge::WsData(mg_connection* conn, int bits, char* data, size_t len, void* self) {
    auto* bridge = static_cast<ServerBridge*>(self);
    const int opcode = bits & 0x0F;
    if (opcode == MG_WEBSOCKET_OPCODE_CONNECTION_CLOSE) return 0;
    if (opcode != MG_WEBSOCKET_OPCODE_TEXT) return 1;

    const std::string text(data, len);
    json j = json::parse(text, nullptr, false);
    if (j.is_discarded() || !j.is_object() || !j.contains("cmd") || !j["cmd"].is_string()) {
        bridge->WsSend(conn, R"({"event":"ack","ok":false,"error":"JSON inválido"})");
        return 1;
    }
    const std::string name = j["cmd"].get<std::string>();
    if (name == "status") {
        json s = SnapshotToJson(bridge->mPlayer->Snapshot());
        s["event"] = "status";
        bridge->WsSend(conn, s.dump());
        return 1;
    }
    bool ok = false;
    json reply = json::parse(bridge->HandleJsonCommand(name, text, ok));
    reply["event"] = "ack";
    reply["cmd"] = name;
    if (j.contains("id")) reply["id"] = j["id"];
    bridge->WsSend(conn, reply.dump());
    return 1;
}

void ServerBridge::WsClose(const mg_connection* conn, void* self) {
    auto* bridge = static_cast<ServerBridge*>(self);
    std::lock_guard<std::mutex> lock(bridge->mWsMutex);
    bridge->mWsClients.erase(conn);
}

void ServerBridge::WsSend(mg_connection* conn, const std::string& text) {
    std::lock_guard<std::mutex> lock(mWsMutex);  // escritas de threads distintas não podem se intercalar
    mg_websocket_write(conn, MG_WEBSOCKET_OPCODE_TEXT, text.data(), text.size());
}

void ServerBridge::WsBroadcast(const std::string& text) {
    std::lock_guard<std::mutex> lock(mWsMutex);
    for (auto& [key, conn] : mWsClients)
        mg_websocket_write(conn, MG_WEBSOCKET_OPCODE_TEXT, text.data(), text.size());
}

void ServerBridge::BroadcastLoop() {
    Platform::PinCurrentThreadToCore(1);
    using namespace std::chrono_literals;
    while (mRunning) {
        for (int i = 0; i < 10 && mRunning; ++i) std::this_thread::sleep_for(100ms);
        if (!mRunning || mAsleep) continue;
        bool hasClients;
        { std::lock_guard<std::mutex> lock(mWsMutex); hasClients = !mWsClients.empty(); }
        if (!hasClients) continue;
        json s = SnapshotToJson(mPlayer->Snapshot());
        s["event"] = "status";
        WsBroadcast(s.dump());
    }
}

}  // namespace SwitchCast
