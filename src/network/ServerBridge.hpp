// network/ServerBridge.hpp
#pragma once

#include <civetweb.h>

#include <atomic>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>

#include "Command.hpp"
#include "SsdpServer.hpp"
#include "Types.hpp"
#include "UpnpService.hpp"
#include "../player/MpvPlayer.hpp"

namespace SwitchCast {

// Servidor embutido: HTTP (UPnP/SOAP + REST), WebSocket e descoberta SSDP.
// Roda em threads próprias; nunca toca o mpv diretamente — apenas enfileira
// NetworkCommands (consumidos pelo loop principal) e lê PlayerSnapshot.
class ServerBridge {
public:
    static ServerBridge& Instance();

    bool Start(std::shared_ptr<MpvPlayer> player, int httpPort, int wsPort);
    void Stop();
    void OnSleep();
    void OnResume();

    // Thread-safe: despacha comandos de rede para o loop do player.
    bool QueueCommand(NetworkCommand cmd);
    bool PopCommand(NetworkCommand& outCmd);

    bool IsRunning() const { return mRunning; }
    DeviceIdentity Identity() const { return mIdentity; }
    std::string CurrentIp() const;

private:
    ServerBridge() = default;

    // Callbacks C do civetweb.
    static int HttpUpnp(mg_connection* conn, void* self);
    static int HttpApi(mg_connection* conn, void* self);
    static int HttpWebRemote(mg_connection* conn, void* self);
    static int WsConnect(const mg_connection* conn, void* self);
    static void WsReady(mg_connection* conn, void* self);
    static int WsData(mg_connection* conn, int bits, char* data, size_t len, void* self);
    static void WsClose(const mg_connection* conn, void* self);

    int HandleUpnp(mg_connection* conn);
    int HandleApi(mg_connection* conn);
    int HandleWebRemote(mg_connection* conn);
    std::string HandleCycleAudio(bool& ok);
    std::string HandleCycleSubtitle(bool& ok);
    std::string HandleStatus();
    std::string HandleJsonCommand(const std::string& name, const std::string& body, bool& ok);
    void WsSend(mg_connection* conn, const std::string& text);
    void WsBroadcast(const std::string& text);
    void BroadcastLoop();

    std::shared_ptr<MpvPlayer> mPlayer;
    DeviceIdentity mIdentity;
    mg_context* mHttpCtx = nullptr;
    std::unique_ptr<SsdpServer> mSsdp;
    std::unique_ptr<UpnpService> mUpnp;
    std::thread mBroadcastThread;
    std::atomic<bool> mRunning{false};
    std::atomic<bool> mAsleep{false};

    CommandQueue mCommands;

    std::mutex mWsMutex;
    std::map<const mg_connection*, mg_connection*> mWsClients;  // protegido por mWsMutex
};

}  // namespace SwitchCast
