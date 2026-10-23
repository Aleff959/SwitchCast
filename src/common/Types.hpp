// Tipos compartilhados entre os subsistemas (rede, player, UI).
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#define SWITCHCAST_VERSION_STR       "2.3.0"
#define SWITCHCAST_VERSION_CODE      20300
#define SWITCHCAST_BUILD_NUM         29
#define SWITCHCAST_VERSION_FULL_STR  "2.3.0 | Comp. 29"

namespace SwitchCast {

enum class DisplayMode {
    OLED_720P,     // Modo portátil: painel OLED 1280x720
    DOCKED_1080P,  // Modo Dock: saída HDMI 1920x1080
};

// Mídia enviada por um controlador (UPnP/DLNA, REST ou WebSocket).
struct MediaPayload {
    std::string url;
    std::string title;
    std::map<std::string, std::string> headers;  // Referer, User-Agent, Cookie...
    std::vector<std::string> subtitles;          // URLs de legendas externas
    int64_t startPositionMs = 0;
    bool autoPlay = true;                        // UPnP carrega pausado e espera "Play"
};

enum class PlaybackState { Idle, Loading, Playing, Paused };

// Cópia imutável do estado do player; segura para ser lida de qualquer thread.
struct PlayerSnapshot {
    PlaybackState state = PlaybackState::Idle;
    bool buffering = false;
    bool muted = false;
    int64_t positionMs = 0;
    int64_t durationMs = 0;
    int volume = 100;  // 0..100
    std::string title;
    std::string url;
    std::string lastError;
};

inline const char* ToString(PlaybackState s) {
    switch (s) {
        case PlaybackState::Idle:    return "idle";
        case PlaybackState::Loading: return "loading";
        case PlaybackState::Playing: return "playing";
        case PlaybackState::Paused:  return "paused";
    }
    return "idle";
}

// Identidade de rede do dispositivo (descoberta SSDP + descrição UPnP).
struct DeviceIdentity {
    std::string uuid;
    std::string friendlyName = "SwitchCast (Nintendo Switch)";
    std::string ip = "0.0.0.0";
    int httpPort = 8080;
    int wsPort = 8081;
};

}  // namespace SwitchCast
