// player/MpvPlayer.cpp
#include "MpvPlayer.hpp"
#include <switch.h>

#include <cstdio>
#include <cstring>
#include <algorithm>

#include "../utils/Log.hpp"
#include "../utils/Text.hpp"

namespace SwitchCast {

namespace {
constexpr int64_t kNetworkCacheBytes = 128LL * 1024 * 1024;  // 128 MiB de RAM para o buffer
constexpr int64_t kBackBufferBytes   = 32LL * 1024 * 1024;   // histórico p/ seek curto para trás
}  // namespace

MpvPlayer::~MpvPlayer() { Shutdown(); }

bool MpvPlayer::SetFirstSupportedOption(const char* name, const std::vector<const char*>& candidates) {
    for (const char* value : candidates) {
        if (mpv_set_option_string(mMpv, name, value) >= 0) {
            SC_LOG("mpv: %s=%s", name, value);
            return true;
        }
    }
    SC_LOG("mpv: nenhum valor suportado para %s", name);
    return false;
}

bool MpvPlayer::Initialize(const std::string& fontsDir) {
    mMpv = mpv_create();
    if (!mMpv) return false;

    auto opt = [this](const char* k, const char* v) { mpv_set_option_string(mMpv, k, v); };

    // --- Isolamento: o player é controlado só pela API, nunca por arquivos/teclado ---
    opt("config", "no");
    opt("terminal", "no");
    opt("idle", "yes");
    opt("load-scripts", "no");
    opt("ytdl", "no");
    opt("input-default-bindings", "no");
    opt("input-vo-keyboard", "no");
    opt("osd-level", "0");  // o OSD é desenhado por nós via osd-overlay
    opt("sub-auto", "no");
    opt("audio-display", "no");
    opt("save-position-on-quit", "no");

    // --- Vídeo: renderizado pelo UIEngine através da render API (OpenGL/EGL) ---
    opt("vo", "libmpv");
    // O decodificador NVDEC do Tegra X1 é exposto pelo FFmpeg do devkitPro como
    // "nvtegra" (e NÃO como "nvdec", que é o nome do backend CUDA em PCs).
    // Se o backend não existir, cai para software em vez de falhar.
    SetFirstSupportedOption("hwdec", {"nvtegra", "auto-safe"});
    opt("hwdec-codecs", "all");
    opt("video-sync", "audio");  // display-resample exige informações de vsync que a render API não fornece

    // Cache adaptativo: 128MB em modo Application (Title Takeover) e 16MB se aberto via Álbum
    int64_t cacheBytes = kNetworkCacheBytes;
    int64_t backBytes = kBackBufferBytes;
    AppletType at = appletGetAppletType();
    if (at != AppletType_Application && at != AppletType_SystemApplication) {
        cacheBytes = 16LL * 1024 * 1024;
        backBytes = 4LL * 1024 * 1024;
    }

    opt("cache", "yes");
    opt("cache-pause", "yes");
    opt("cache-pause-wait", "3");
    opt("demuxer-max-bytes", std::to_string(cacheBytes).c_str());
    opt("demuxer-max-back-bytes", std::to_string(backBytes).c_str());
    opt("demuxer-readahead-secs", "30");
    opt("network-timeout", "15");

    // --- Áudio: backend nativo do Horizon (audren) quando disponível ---
    SetFirstSupportedOption("ao", {"hos", "sdl"});
    opt("audio-channels", "stereo");
    opt("audio-samplerate", "48000");

    // --- Cor: BT.709 com níveis completos (pretos reais no OLED) ---
    opt("target-prim", "bt.709");
    opt("video-output-levels", "full");

    if (mpv_initialize(mMpv) < 0) {
        mpv_terminate_destroy(mMpv);
        mMpv = nullptr;
        return false;
    }

    mpv_observe_property(mMpv, kObsTimePos,   "time-pos",         MPV_FORMAT_DOUBLE);
    mpv_observe_property(mMpv, kObsDuration,  "duration",         MPV_FORMAT_DOUBLE);
    mpv_observe_property(mMpv, kObsPause,     "pause",            MPV_FORMAT_FLAG);
    mpv_observe_property(mMpv, kObsVolume,    "volume",           MPV_FORMAT_DOUBLE);
    mpv_observe_property(mMpv, kObsMute,      "mute",             MPV_FORMAT_FLAG);
    mpv_observe_property(mMpv, kObsBuffering, "paused-for-cache", MPV_FORMAT_FLAG);
    mpv_observe_property(mMpv, kObsTitle,     "media-title",      MPV_FORMAT_STRING);

#ifdef SWITCHCAST_DEBUG
    mpv_request_log_messages(mMpv, "warn");
#endif
    return true;
}

void MpvPlayer::Shutdown() {
    if (!mMpv) return;
    // O UIEngine deve destruir o render context ANTES de chamar isto.
    mpv_terminate_destroy(mMpv);
    mMpv = nullptr;
}

void MpvPlayer::Command(const std::vector<std::string>& args, bool async) {
    if (!mMpv) return;
    std::vector<const char*> argv;
    argv.reserve(args.size() + 1);
    for (const auto& a : args) argv.push_back(a.c_str());
    argv.push_back(nullptr);
    if (async) mpv_command_async(mMpv, 0, argv.data());
    else       mpv_command(mMpv, argv.data());
}

void MpvPlayer::ApplyHeaders(const std::map<std::string, std::string>& headers) {
    // User-Agent e Referer têm opções próprias no mpv; o resto vai em http-header-fields
    // como lista (mpv_node), o que preserva vírgulas dentro de valores (ex.: Cookie).
    std::vector<std::string> lines;
    std::string userAgent, referrer;
    for (const auto& [key, value] : headers) {
        if (Text::EqualsIgnoreCase(key, "User-Agent")) userAgent = value;
        else if (Text::EqualsIgnoreCase(key, "Referer") || Text::EqualsIgnoreCase(key, "Referrer")) referrer = value;
        else lines.push_back(key + ": " + value);
    }

    mpv_set_property_string(mMpv, "user-agent", userAgent.empty() ? "SwitchCast/1.1.0" : userAgent.c_str());
    mpv_set_property_string(mMpv, "referrer", referrer.c_str());

    std::vector<mpv_node> items(lines.size());
    for (std::size_t i = 0; i < lines.size(); ++i) {
        items[i].format = MPV_FORMAT_STRING;
        items[i].u.string = lines[i].data();
    }
    mpv_node_list list{static_cast<int>(items.size()), items.empty() ? nullptr : items.data(), nullptr};
    mpv_node root{};
    root.format = MPV_FORMAT_NODE_ARRAY;
    root.u.list = &list;
    mpv_set_property(mMpv, "http-header-fields", MPV_FORMAT_NODE, &root);
}

void MpvPlayer::LoadMedia(const MediaPayload& media) {
    if (!mMpv || media.url.empty()) return;

    ApplyHeaders(media.headers);
    mpv_set_property_string(mMpv, "pause", media.autoPlay ? "no" : "yes");

    {
        std::lock_guard<std::mutex> lock(mMutex);
        mLoadRequested = true;
        mUserTitle = media.title;
        mPendingSeekMs = media.startPositionMs;
        mPendingSubtitles = media.subtitles;
        const int volume = mSnapshot.volume;
        const bool muted = mSnapshot.muted;
        mSnapshot = PlayerSnapshot{};
        mSnapshot.volume = volume;
        mSnapshot.muted = muted;
        mSnapshot.state = PlaybackState::Loading;
        mSnapshot.title = media.title;
        mSnapshot.url = media.url;
    }
    Command({"loadfile", media.url, "replace"});
}

void MpvPlayer::Play()  { if (mMpv) mpv_set_property_string(mMpv, "pause", "no"); }
void MpvPlayer::Pause() { if (mMpv) mpv_set_property_string(mMpv, "pause", "yes"); }

void MpvPlayer::TogglePause() {
    if (!mMpv) return;
    PlaybackState st;
    { std::lock_guard<std::mutex> lock(mMutex); st = mSnapshot.state; }
    if (st == PlaybackState::Paused) Play();
    else if (st == PlaybackState::Playing) Pause();
}

void MpvPlayer::Stop() {
    {
        std::lock_guard<std::mutex> lock(mMutex);
        mLoadRequested = false;
        mPendingSubtitles.clear();
        const int volume = mSnapshot.volume;
        const bool muted = mSnapshot.muted;
        mSnapshot = PlayerSnapshot{};
        mSnapshot.volume = volume;
        mSnapshot.muted = muted;
    }
    Command({"stop"});
}

void MpvPlayer::SeekAbsoluteMs(int64_t ms) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.3f", std::max<int64_t>(ms, 0) / 1000.0);
    Command({"seek", buf, "absolute"});
}

void MpvPlayer::SeekRelativeMs(int64_t deltaMs) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.3f", deltaMs / 1000.0);
    Command({"seek", buf, "relative"});
}

void MpvPlayer::SetVolume(int percent) {
    if (!mMpv) return;
    char buf[16];
    std::snprintf(buf, sizeof buf, "%d", std::clamp(percent, 0, 100));
    mpv_set_property_string(mMpv, "volume", buf);
}

void MpvPlayer::AdjustVolume(int delta) {
    int current;
    { std::lock_guard<std::mutex> lock(mMutex); current = mSnapshot.volume; }
    SetVolume(current + delta);
}

void MpvPlayer::SetMute(bool mute) {
    if (mMpv) mpv_set_property_string(mMpv, "mute", mute ? "yes" : "no");
}

void MpvPlayer::AddExternalSubtitle(const std::string& subtitleUrl) {
    if (!subtitleUrl.empty()) Command({"sub-add", subtitleUrl, "cached"});
}

void MpvPlayer::CycleAudioTrack()    { Command({"cycle", "audio"}); }
void MpvPlayer::CycleSubtitleTrack() { Command({"cycle", "sub"}); }

void MpvPlayer::SetOsdOverlay(const std::string& assEvents, int resX, int resY) {
    if (assEvents.empty()) {
        Command({"osd-overlay", "1", "none"});
    } else {
        Command({"osd-overlay", "1", "ass-events", assEvents, std::to_string(resX), std::to_string(resY)});
    }
}

PlayerSnapshot MpvPlayer::Snapshot() const {
    std::lock_guard<std::mutex> lock(mMutex);
    return mSnapshot;
}

bool MpvPlayer::HasMedia() const {
    std::lock_guard<std::mutex> lock(mMutex);
    return mSnapshot.state != PlaybackState::Idle;
}

void MpvPlayer::PumpEvents() {
    if (!mMpv) return;
    for (;;) {
        mpv_event* ev = mpv_wait_event(mMpv, 0);
        if (ev->event_id == MPV_EVENT_NONE) break;
        HandleEvent(*ev);
    }
}

void MpvPlayer::HandleEvent(const mpv_event& ev) {
    switch (ev.event_id) {
        case MPV_EVENT_FILE_LOADED: {
            int64_t seekMs = 0;
            std::vector<std::string> subs;
            {
                std::lock_guard<std::mutex> lock(mMutex);
                mLoadRequested = false;
                mSnapshot.state = mPausedProperty ? PlaybackState::Paused : PlaybackState::Playing;
                seekMs = mPendingSeekMs;
                subs.swap(mPendingSubtitles);
                mPendingSeekMs = 0;
            }
            if (seekMs > 0) SeekAbsoluteMs(seekMs);
            for (const auto& s : subs) AddExternalSubtitle(s);
            break;
        }
        case MPV_EVENT_END_FILE: {
            const auto* end = static_cast<const mpv_event_end_file*>(ev.data);
            std::lock_guard<std::mutex> lock(mMutex);
            // Um `loadfile replace` gera END_FILE(stop) do arquivo antigo: não é "ocioso".
            if (end->reason == MPV_END_FILE_REASON_STOP && mLoadRequested) break;

            const int volume = mSnapshot.volume;
            const bool muted = mSnapshot.muted;
            const std::string err =
                end->reason == MPV_END_FILE_REASON_ERROR ? mpv_error_string(end->error) : std::string();
            mSnapshot = PlayerSnapshot{};
            mSnapshot.volume = volume;
            mSnapshot.muted = muted;
            mSnapshot.lastError = err;
            mLoadRequested = false;
            break;
        }
        case MPV_EVENT_PROPERTY_CHANGE: {
            const auto* prop = static_cast<const mpv_event_property*>(ev.data);
            HandlePropertyChange(*prop, ev.reply_userdata);
            break;
        }
        case MPV_EVENT_LOG_MESSAGE: {
#ifdef SWITCHCAST_DEBUG
            const auto* msg = static_cast<const mpv_event_log_message*>(ev.data);
            std::printf("[mpv:%s] %s", msg->prefix, msg->text);
#endif
            break;
        }
        default:
            break;
    }
}

void MpvPlayer::HandlePropertyChange(const mpv_event_property& prop, uint64_t id) {
    std::lock_guard<std::mutex> lock(mMutex);
    switch (id) {
        case kObsTimePos:
            if (prop.format == MPV_FORMAT_DOUBLE)
                mSnapshot.positionMs = static_cast<int64_t>(*static_cast<double*>(prop.data) * 1000.0);
            break;
        case kObsDuration:
            mSnapshot.durationMs = prop.format == MPV_FORMAT_DOUBLE
                                       ? static_cast<int64_t>(*static_cast<double*>(prop.data) * 1000.0)
                                       : 0;
            break;
        case kObsPause:
            if (prop.format == MPV_FORMAT_FLAG) {
                mPausedProperty = *static_cast<int*>(prop.data) != 0;
                if (mSnapshot.state == PlaybackState::Playing || mSnapshot.state == PlaybackState::Paused)
                    mSnapshot.state = mPausedProperty ? PlaybackState::Paused : PlaybackState::Playing;
            }
            break;
        case kObsVolume:
            if (prop.format == MPV_FORMAT_DOUBLE)
                mSnapshot.volume = std::clamp(static_cast<int>(*static_cast<double*>(prop.data) + 0.5), 0, 100);
            break;
        case kObsMute:
            if (prop.format == MPV_FORMAT_FLAG) mSnapshot.muted = *static_cast<int*>(prop.data) != 0;
            break;
        case kObsBuffering:
            if (prop.format == MPV_FORMAT_FLAG) mSnapshot.buffering = *static_cast<int*>(prop.data) != 0;
            break;
        case kObsTitle:
            if (prop.format == MPV_FORMAT_STRING && mUserTitle.empty() && mSnapshot.state != PlaybackState::Idle)
                mSnapshot.title = *static_cast<char**>(prop.data);
            break;
        default:
            break;
    }
}

}  // namespace SwitchCast
