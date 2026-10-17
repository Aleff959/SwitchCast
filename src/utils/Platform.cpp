#include "Platform.hpp"

#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <switch.h>

#include <cstdio>
#include <cstring>

#include "Log.hpp"

namespace SwitchCast::Platform {

void PinCurrentThreadToCore(int core) {
    svcSetThreadCoreMask(CUR_THREAD_HANDLE, core, 1u << core);
}

std::string GetLocalIp() {
    u32 raw = 0;
    if (R_FAILED(nifmGetCurrentIpAddress(&raw)) || raw == 0) return "0.0.0.0";
    in_addr addr{};
    addr.s_addr = raw;  // já vem em ordem de rede
    char buf[INET_ADDRSTRLEN] = {};
    inet_ntop(AF_INET, &addr, buf, sizeof buf);
    return buf;
}

static void EnsureDataDir() {
    mkdir("sdmc:/switch", 0777);
    mkdir(kDataDir, 0777);
}

static std::string GenerateUuidV4() {
    unsigned char b[16];
    randomGet(b, sizeof b);
    b[6] = (b[6] & 0x0F) | 0x40;  // versão 4
    b[8] = (b[8] & 0x3F) | 0x80;  // variante RFC 4122
    char out[40];
    std::snprintf(out, sizeof out,
                  "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
                  b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7],
                  b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
    return out;
}

DeviceIdentity LoadDeviceIdentity(int httpPort, int wsPort) {
    DeviceIdentity id;
    id.httpPort = httpPort;
    id.wsPort = wsPort;
    EnsureDataDir();

    const std::string path = std::string(kDataDir) + "/uuid.txt";
    char buf[64] = {};
    if (FILE* f = std::fopen(path.c_str(), "r")) {
        if (std::fgets(buf, sizeof buf, f)) {
            buf[std::strcspn(buf, "\r\n")] = '\0';
            id.uuid = buf;
        }
        std::fclose(f);
    }
    if (id.uuid.size() != 36) {
        id.uuid = GenerateUuidV4();
        if (FILE* f = std::fopen(path.c_str(), "w")) {
            std::fputs(id.uuid.c_str(), f);
            std::fclose(f);
        }
    }

    // Nome opcional: sdmc:/switch/SwitchCast/name.txt (uma linha).
    const std::string namePath = std::string(kDataDir) + "/name.txt";
    if (FILE* f = std::fopen(namePath.c_str(), "r")) {
        char name[96] = {};
        if (std::fgets(name, sizeof name, f)) {
            name[std::strcspn(name, "\r\n")] = '\0';
            if (name[0] != '\0') id.friendlyName = name;
        }
        std::fclose(f);
    }
    return id;
}

std::string ExportSystemFonts() {
    // As fontes do Horizon são lidas diretamente da memória pelo FreeType (Renderer2D),
    // eliminando escritas lentas no cartão SD e acelerando a inicialização para < 0.1s.
    return {};
}

double NowSeconds() {
    return static_cast<double>(armTicksToNs(armGetSystemTick())) / 1e9;
}

}  // namespace SwitchCast::Platform
