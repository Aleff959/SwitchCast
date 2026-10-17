// player/MpvPlayer.hpp
#pragma once

#include <mpv/client.h>

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include "Types.hpp"

namespace SwitchCast {

// Encapsula o libmpv. Todos os métodos de comando e PumpEvents() devem ser
// chamados da thread principal; Snapshot() é seguro para qualquer thread
// (lê apenas uma cópia protegida por mutex, nunca o handle do mpv).
class MpvPlayer {
public:
    MpvPlayer() = default;
    ~MpvPlayer();
    MpvPlayer(const MpvPlayer&) = delete;
    MpvPlayer& operator=(const MpvPlayer&) = delete;

    // `fontsDir` (opcional): diretório com TTFs para o OSD (libass).
    bool Initialize(const std::string& fontsDir = {});
    void Shutdown();

    mpv_handle* Handle() const { return mMpv; }

    void LoadMedia(const MediaPayload& media);
    void Play();
    void Pause();
    void TogglePause();
    void Stop();
    void SeekAbsoluteMs(int64_t ms);
    void SeekRelativeMs(int64_t deltaMs);
    void SetVolume(int percent);
    void AdjustVolume(int delta);
    void SetMute(bool mute);
    void AddExternalSubtitle(const std::string& subtitleUrl);
    void CycleAudioTrack();
    void CycleSubtitleTrack();

    // Overlay ASS (osd-overlay id 1) na resolução lógica `resX` x `resY`. String vazia remove.
    void SetOsdOverlay(const std::string& assEvents, int resX, int resY);

    // Drena a fila de eventos do mpv e atualiza o snapshot.
    void PumpEvents();

    PlayerSnapshot Snapshot() const;
    bool HasMedia() const;

private:
    enum ObserveId : uint64_t {
        kObsTimePos = 1, kObsDuration, kObsPause, kObsVolume, kObsMute, kObsBuffering, kObsTitle,
    };

    bool SetFirstSupportedOption(const char* name, const std::vector<const char*>& candidates);
    void ApplyHeaders(const std::map<std::string, std::string>& headers);
    void Command(const std::vector<std::string>& args, bool async = true);
    void HandleEvent(const mpv_event& ev);
    void HandlePropertyChange(const mpv_event_property& prop, uint64_t id);

    mpv_handle* mMpv = nullptr;

    mutable std::mutex mMutex;
    PlayerSnapshot mSnapshot;      // protegido por mMutex
    bool mLoadRequested = false;   // ignora END_FILE(stop) do arquivo substituído
    bool mPausedProperty = false;
    std::string mUserTitle;        // título vindo do controlador tem prioridade sobre media-title

    // Aplicados em MPV_EVENT_FILE_LOADED (loadfile é assíncrono).
    int64_t mPendingSeekMs = 0;
    std::vector<std::string> mPendingSubtitles;
};

}  // namespace SwitchCast
