#include "SsdpServer.hpp"

#include <arpa/inet.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstring>
#include <random>

#include "Upnp.hpp"
#include "../utils/Log.hpp"
#include "../utils/Platform.hpp"
#include "../utils/Text.hpp"

namespace SwitchCast {

namespace {
constexpr const char* kMulticastAddr = "239.255.255.250";
constexpr uint16_t kSsdpPort = 1900;
constexpr double kNotifyIntervalSec = 30.0;
constexpr const char* kServerString = "Horizon/1.0 UPnP/1.0 SwitchCast/1.2.0";

double Now() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

// Retorna o valor do cabeçalho `name` (case-insensitive) numa mensagem HTTP-over-UDP.
std::string HeaderValue(const std::string& msg, const std::string& name) {
    std::size_t pos = 0;
    while (pos < msg.size()) {
        std::size_t eol = msg.find("\r\n", pos);
        if (eol == std::string::npos) eol = msg.size();
        const std::size_t colon = msg.find(':', pos);
        if (colon != std::string::npos && colon < eol &&
            Text::EqualsIgnoreCase(Text::Trim(msg.substr(pos, colon - pos)), name)) {
            return Text::Trim(msg.substr(colon + 1, eol - colon - 1));
        }
        pos = eol + 2;
    }
    return {};
}
}  // namespace

SsdpServer::SsdpServer(DeviceIdentity identity, std::function<std::string()> ipProvider)
    : mIdentity(std::move(identity)), mIpProvider(std::move(ipProvider)) {
    mTargets = BuildTargets();
}

SsdpServer::~SsdpServer() { Stop(); }

std::vector<SsdpServer::Target> SsdpServer::BuildTargets() const {
    const std::string uuid = "uuid:" + mIdentity.uuid;
    std::vector<Target> t;
    t.push_back({"upnp:rootdevice", uuid + "::upnp:rootdevice"});
    t.push_back({uuid, uuid});
    t.push_back({Upnp::kDeviceType, uuid + "::" + Upnp::kDeviceType});
    for (auto s : {Upnp::Service::AVTransport, Upnp::Service::RenderingControl, Upnp::Service::ConnectionManager})
        t.push_back({Upnp::ServiceType(s), uuid + "::" + Upnp::ServiceType(s)});
    return t;
}

std::string SsdpServer::Location() const {
    return "http://" + mIpProvider() + ":" + std::to_string(mIdentity.httpPort) + "/upnp/description.xml";
}

int SsdpServer::OpenSocket() const {
    const int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return -1;

    int yes = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes);

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(kSsdpPort);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr) < 0) { close(fd); return -1; }

    ip_mreq mreq{};
    mreq.imr_multiaddr.s_addr = inet_addr(kMulticastAddr);
    mreq.imr_interface.s_addr = htonl(INADDR_ANY);
    if (setsockopt(fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof mreq) < 0) {
        SC_LOG("ssdp: IP_ADD_MEMBERSHIP falhou");
    }
    unsigned char ttl = 4;
    setsockopt(fd, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof ttl);
    return fd;
}

void SsdpServer::SendNotify(int fd, const char* nts) {
    sockaddr_in dest{};
    dest.sin_family = AF_INET;
    dest.sin_port = htons(kSsdpPort);
    dest.sin_addr.s_addr = inet_addr(kMulticastAddr);

    const bool alive = std::strcmp(nts, "ssdp:alive") == 0;
    const std::string location = alive ? Location() : std::string();
    for (const Target& t : mTargets) {
        std::string msg = "NOTIFY * HTTP/1.1\r\nHOST: 239.255.255.250:1900\r\n";
        if (alive) msg += "CACHE-CONTROL: max-age=1800\r\nLOCATION: " + location + "\r\n";
        msg += "NT: " + t.nt + "\r\nNTS: " + nts + "\r\nSERVER: " + kServerString + "\r\nUSN: " + t.usn + "\r\n\r\n";
        sendto(fd, msg.data(), msg.size(), 0, reinterpret_cast<sockaddr*>(&dest), sizeof dest);
    }
}

void SsdpServer::SendSearchResponse(int fd, const sockaddr_in& to, const Target& t) {
    const std::string msg = "HTTP/1.1 200 OK\r\nCACHE-CONTROL: max-age=1800\r\nEXT:\r\nLOCATION: " + Location() +
                            "\r\nSERVER: " + kServerString + "\r\nST: " + t.nt + "\r\nUSN: " + t.usn + "\r\n\r\n";
    sendto(fd, msg.data(), msg.size(), 0, reinterpret_cast<const sockaddr*>(&to), sizeof to);
}

void SsdpServer::Start() {
    if (mRunning.exchange(true)) return;
    mThread = std::thread([this] { Run(); });
}

void SsdpServer::Stop() {
    if (!mRunning.exchange(false)) return;
    if (mThread.joinable()) mThread.join();
}

void SsdpServer::RequestReannounce() { mReannounce = true; }

void SsdpServer::Run() {
    Platform::PinCurrentThreadToCore(1);  // rede no núcleo 1; UI/render no núcleo 0

    std::mt19937 rng{std::random_device{}()};
    int fd = -1;
    double lastNotify = 0;

    while (mRunning) {
        if (fd < 0 || mReannounce.exchange(false)) {
            if (fd >= 0) close(fd);
            fd = OpenSocket();
            if (fd < 0) {  // Wi-Fi ainda indisponível: tenta de novo em 2 s
                for (int i = 0; i < 20 && mRunning; ++i) usleep(100 * 1000);
                continue;
            }
            SendNotify(fd, "ssdp:alive");
            lastNotify = Now();
        }

        pollfd pfd{fd, POLLIN, 0};
        const int ready = poll(&pfd, 1, 500);
        if (ready > 0 && (pfd.revents & POLLIN)) {
            char buf[1500];
            sockaddr_in from{};
            socklen_t fromLen = sizeof from;
            const ssize_t n = recvfrom(fd, buf, sizeof buf - 1, 0, reinterpret_cast<sockaddr*>(&from), &fromLen);
            if (n > 0) {
                buf[n] = '\0';
                const std::string msg(buf, static_cast<std::size_t>(n));
                if (msg.compare(0, 8, "M-SEARCH") == 0) {
                    const std::string st = HeaderValue(msg, "ST");
                    for (const Target& t : mTargets) {
                        if (st == "ssdp:all" || st == t.nt) {
                            // MX pede uma resposta aleatória dentro da janela, evitando rajadas.
                            usleep(std::uniform_int_distribution<int>(0, 300)(rng) * 1000);
                            SendSearchResponse(fd, from, t);
                        }
                    }
                }
            }
        } else if (ready < 0) {
            close(fd);
            fd = -1;
            continue;
        }

        if (Now() - lastNotify >= kNotifyIntervalSec) {
            SendNotify(fd, "ssdp:alive");
            lastNotify = Now();
        }
    }

    if (fd >= 0) {
        SendNotify(fd, "ssdp:byebye");
        close(fd);
    }
}

}  // namespace SwitchCast
