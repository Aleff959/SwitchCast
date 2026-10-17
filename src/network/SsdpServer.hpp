// network/SsdpServer.hpp - descoberta UPnP (M-SEARCH + NOTIFY alive/byebye) sobre UDP multicast.
#pragma once

#include <netinet/in.h>

#include <atomic>
#include <functional>
#include <string>
#include <thread>
#include <vector>

#include "Types.hpp"

namespace SwitchCast {

class SsdpServer {
public:
    // `ipProvider` é consultado a cada resposta: o IP muda ao reconectar o Wi-Fi.
    SsdpServer(DeviceIdentity identity, std::function<std::string()> ipProvider);
    ~SsdpServer();

    void Start();
    void Stop();                 // envia byebye e encerra a thread
    void RequestReannounce();    // após suspensão: recria o socket e anuncia de novo

private:
    struct Target { std::string nt; std::string usn; };

    void Run();
    int OpenSocket() const;
    void SendNotify(int fd, const char* nts);
    void SendSearchResponse(int fd, const struct sockaddr_in& to, const Target& t);
    std::string Location() const;
    std::vector<Target> BuildTargets() const;

    DeviceIdentity mIdentity;
    std::function<std::string()> mIpProvider;
    std::vector<Target> mTargets;
    std::thread mThread;
    std::atomic<bool> mRunning{false};
    std::atomic<bool> mReannounce{false};
};

}  // namespace SwitchCast
