#include "ServerBridge.hpp"

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstring>

#include "Upnp.hpp"
#include "WebRemote.hpp"
#include "AppIcon.hpp"
#include "../utils/Log.hpp"
#include "../utils/Platform.hpp"
#include "../utils/Text.hpp"

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

void SendResponse(mg_connection* conn, int status, const char* contentType, const std::string& body,
                  const std::string& extraHeaders = {}) {
    const char* text = status == 200 ? "OK" : status == 204 ? "No Content" : status == 400 ? "Bad Request"
                       : status == 404 ? "Not Found" : status == 405 ? "Method Not Allowed"
                       : status == 503 ? "Service Unavailable" : "Internal Server Error";
    mg_printf(conn,
              "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\nConnection: close\r\n"
              "Access-Control-Allow-Origin: *\r\nAccess-Control-Allow-Headers: Content-Type\r\n"
              "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\nServer: SwitchCast/1.0\r\n%s\r\n",
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
        if (!j.contains("level") || !j["level"].is_number()) { err = "informe level (0-100)"; return false; }
        cmd.type = NetworkCommand::Type::SetVolume;
        cmd.volume = static_cast<int>(std::max<int64_t>(0, std::min<int64_t>(100, num("level", 100))));
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
                             "request_timeout_ms", "15000",
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

std::string ServerBridge::HandleJsonCommand(const std::string& name, const std::string& body, bool& ok) {
    json j = json::object();
    if (!Text::Trim(body).empty()) {
        j = json::parse(body, nullptr, /*allow_exceptions=*/false);
        if (j.is_discarded() || !j.is_object()) {
            ok = false;
            return json{{"ok", false}, {"error", "JSON inválido"}}.dump();
        }
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
        SendResponse(conn, 200, kJson, SnapshotToJson(mPlayer->Snapshot()).dump());
        return 200;
    }
    if (method == "GET" && route == "info") {
        json info{{"name", mIdentity.friendlyName}, {"author", "Aleff959"}, {"uuid", mIdentity.uuid}, {"ip", CurrentIp()},
                  {"httpPort", mIdentity.httpPort}, {"wsPort", mIdentity.wsPort}, {"version", "1.1.0"}};
        SendResponse(conn, 200, kJson, info.dump());
        return 200;
    }
    if (method != "POST") { SendResponse(conn, 405, kJson, R"({"ok":false,"error":"use POST"})"); return 405; }

    bool ok = false;
    const std::string reply = HandleJsonCommand(route, ReadBody(conn), ok);
    const int status = ok ? 200 : (reply.find("fila cheia") != std::string::npos ? 503 : 400);
    SendResponse(conn, status, kJson, reply);
    return status;
}

// ---------------------------------------------------------------------------
// HTTP: Web Remote
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
