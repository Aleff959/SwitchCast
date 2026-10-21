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
    opt("osd-bar", "no");
    opt("osd-on-seek", "no");
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

    // --- Áudio Pro v2.1.0: Downmix ITU-R BS.775 + Volume Boost 150% ---
    SetFirstSupportedOption("ao", {"hos", "sdl"});
    opt("audio-channels", "stereo");
    opt("audio-samplerate", "48000");
    opt("ad-lavc-downmix", "yes");
    opt("audio-normalize-downmix", "yes");
    opt("volume-max", "150");
    // Soft limiter para impedir distorção (clipping) acima de 100% nos alto-falantes do Switch
    opt("af", "lavfi=[alimiter=limit=0.95:attack=5:release=50]");

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

    mpv_set_property_string(mMpv, "user-agent", userAgent.empty() ? "SwitchCast/" SWITCHCAST_VERSION_STR : userAgent.c_str());
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

    {
        std::lock_guard<std::mutex> lock(mMutex);
        mIsWallpaper = false;
    }
    mpv_set_property_string(mMpv, "loop-file", "no");
    mpv_set_property_string(mMpv, "aid", "auto");
    mpv_set_property_string(mMpv, "sid", "auto");

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
    StartWallpaperLoop();
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
    std::snprintf(buf, sizeof buf, "%d", std::clamp(percent, 0, 150));
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

// ---------------------------------------------------------------------------
// Faixas de Áudio e Legenda com nomes amigáveis e sem perder áudio
// ---------------------------------------------------------------------------
std::vector<MpvPlayer::TrackItem> MpvPlayer::GetTracks(const std::string& typeFilter) {
    std::vector<TrackItem> result;
    if (!mMpv) return result;

    mpv_node node{};
    if (mpv_get_property(mMpv, "track-list", MPV_FORMAT_NODE, &node) < 0) {
        return result;
    }

    if (node.format == MPV_FORMAT_NODE_ARRAY && node.u.list) {
        for (int i = 0; i < node.u.list->num; ++i) {
            const mpv_node& item = node.u.list->values[i];
            if (item.format != MPV_FORMAT_NODE_MAP || !item.u.list) continue;

            TrackItem t;
            for (int k = 0; k < item.u.list->num; ++k) {
                const char* key = item.u.list->keys[k];
                const mpv_node& val = item.u.list->values[k];
                if (!key) continue;

                if (std::strcmp(key, "type") == 0 && val.format == MPV_FORMAT_STRING) {
                    t.type = val.u.string ? val.u.string : "";
                } else if (std::strcmp(key, "id") == 0 && val.format == MPV_FORMAT_INT64) {
                    t.id = static_cast<int>(val.u.int64);
                } else if (std::strcmp(key, "selected") == 0 && val.format == MPV_FORMAT_FLAG) {
                    t.selected = (val.u.flag != 0);
                } else if (std::strcmp(key, "title") == 0 && val.format == MPV_FORMAT_STRING) {
                    t.title = val.u.string ? val.u.string : "";
                } else if (std::strcmp(key, "lang") == 0 && val.format == MPV_FORMAT_STRING) {
                    t.lang = val.u.string ? val.u.string : "";
                }
            }

            if (typeFilter.empty() || t.type == typeFilter) {
                result.push_back(std::move(t));
            }
        }
    }

    mpv_free_node_contents(&node);
    return result;
}

namespace {
std::string FriendlyTrackName(const MpvPlayer::TrackItem& t, int index, int total, bool isAudio) {
    std::string lang = t.lang;
    for (char& c : lang) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

    std::string label;
    if (lang == "por" || lang == "pt" || lang == "pob" || lang == "pt-br") label = "Portugues";
    else if (lang == "eng" || lang == "en" || lang == "en-us" || lang == "en-gb") label = "Ingles";
    else if (lang == "spa" || lang == "es") label = "Espanhol";
    else if (lang == "jpn" || lang == "ja") label = "Japones";
    else if (lang == "fra" || lang == "fre" || lang == "fr") label = "Frances";
    else if (lang == "deu" || lang == "ger" || lang == "de") label = "Alemao";
    else if (lang == "ita" || lang == "it") label = "Italiano";
    else if (lang == "kor" || lang == "ko") label = "Coreano";
    else if (lang == "rus" || lang == "ru") label = "Russo";
    else if (lang == "zho" || lang == "chi" || lang == "zh") label = "Chines";
    else if (!t.title.empty()) label = t.title;

    std::string prefix = isAudio ? "Audio " : "Legenda ";
    if (total > 1) {
        prefix += std::to_string(index + 1) + "/" + std::to_string(total);
    } else {
        prefix += "Faixa 1";
    }

    if (!label.empty() && label != "und") {
        return prefix + " (" + label + ")";
    }
    return prefix;
}
}  // namespace

std::string MpvPlayer::CycleAudioTrack() {
    if (!mMpv) return "Audio indisponivel";

    auto tracks = GetTracks("audio");
    if (tracks.empty()) {
        return "Nenhuma faixa de audio encontrada";
    }

    if (tracks.size() == 1) {
        // Se só existe 1 faixa de áudio, certifique-se de que está ativa e NUNCA silencie
        std::string idStr = std::to_string(tracks[0].id);
        mpv_set_property_string(mMpv, "aid", idStr.c_str());
        return FriendlyTrackName(tracks[0], 0, 1, true) + " (Unica)";
    }

    int currentIndex = -1;
    for (size_t i = 0; i < tracks.size(); ++i) {
        if (tracks[i].selected) {
            currentIndex = static_cast<int>(i);
            break;
        }
    }

    // Cicla somente entre as faixas válidas (0 -> 1 -> ... -> N-1 -> 0), nunca para 'no'
    int nextIndex = (currentIndex + 1) % static_cast<int>(tracks.size());
    std::string nextIdStr = std::to_string(tracks[nextIndex].id);
    mpv_set_property_string(mMpv, "aid", nextIdStr.c_str());

    return FriendlyTrackName(tracks[nextIndex], nextIndex, static_cast<int>(tracks.size()), true);
}

std::string MpvPlayer::CycleSubtitleTrack() {
    if (!mMpv) return "Legendas indisponiveis";

    auto tracks = GetTracks("sub");
    if (tracks.empty()) {
        return "Sem legendas disponiveis";
    }

    int currentIndex = -1;
    for (size_t i = 0; i < tracks.size(); ++i) {
        if (tracks[i].selected) {
            currentIndex = static_cast<int>(i);
            break;
        }
    }

    // Se nenhuma estiver ativa (currentIndex == -1): ativa a primeira faixa
    // Se a última estiver ativa (currentIndex == tracks.size() - 1): desativa as legendas (sid = "no")
    // Senão: vai para a próxima faixa
    if (currentIndex == -1) {
        std::string idStr = std::to_string(tracks[0].id);
        mpv_set_property_string(mMpv, "sid", idStr.c_str());
        return FriendlyTrackName(tracks[0], 0, static_cast<int>(tracks.size()), false);
    } else if (currentIndex + 1 < static_cast<int>(tracks.size())) {
        int nextIndex = currentIndex + 1;
        std::string nextIdStr = std::to_string(tracks[nextIndex].id);
        mpv_set_property_string(mMpv, "sid", nextIdStr.c_str());
        return FriendlyTrackName(tracks[nextIndex], nextIndex, static_cast<int>(tracks.size()), false);
    } else {
        mpv_set_property_string(mMpv, "sid", "no");
        return "Legendas: Desativadas";
    }
}

// ---------------------------------------------------------------------------
// Wallpaper animado em Idle no Horizon OS
// ---------------------------------------------------------------------------
void MpvPlayer::StartWallpaperLoop() {
    if (!mMpv) return;

    static const char* kCandidatePaths[] = {
        "sdmc:/switch/SwitchCast/wallpaper.mp4",
        "romfs:/wallpaper.mp4",
        "switch/SwitchCast/wallpaper.mp4",
        "wallpaper.mp4"
    };

    std::string wallpaperPath;
    for (const char* p : kCandidatePaths) {
        FILE* f = std::fopen(p, "rb");
        if (f) {
            std::fclose(f);
            wallpaperPath = p;
            break;
        }
    }

    if (wallpaperPath.empty()) {
        Command({"stop"});
        return;
    }

    {
        std::lock_guard<std::mutex> lock(mMutex);
        mIsWallpaper = true;
        mLoadRequested = false;
        const int volume = mSnapshot.volume;
        const bool muted = mSnapshot.muted;
        mSnapshot = PlayerSnapshot{};
        mSnapshot.volume = volume;
        mSnapshot.muted = muted;
        mSnapshot.state = PlaybackState::Idle;
    }

    mpv_set_property_string(mMpv, "loop-file", "inf");
    mpv_set_property_string(mMpv, "aid", "no");
    mpv_set_property_string(mMpv, "sid", "no");
    mpv_set_property_string(mMpv, "pause", "no");
    Command({"loadfile", wallpaperPath, "replace"});
}

void MpvPlayer::StopWallpaperLoop() {
    if (!mMpv) return;
    {
        std::lock_guard<std::mutex> lock(mMutex);
        if (!mIsWallpaper) return;
        mIsWallpaper = false;
    }
    mpv_set_property_string(mMpv, "loop-file", "no");
    mpv_set_property_string(mMpv, "aid", "auto");
    mpv_set_property_string(mMpv, "sid", "auto");
    Command({"stop"});
}

bool MpvPlayer::IsWallpaperActive() const {
    std::lock_guard<std::mutex> lock(mMutex);
    return mIsWallpaper;
}

void MpvPlayer::SetOsdOverlay(const std::string& assEvents, int resX, int resY) {
    if (assEvents.empty()) {
        Command({"osd-overlay", "1", "none"});
    } else {
        Command({"osd-overlay", "1", "ass-events", assEvents, std::to_string(resX), std::to_string(resY)});
    }
}

PlayerSnapshot MpvPlayer::Snapshot() const {
    std::lock_guard<std::mutex> lock(mMutex);
    if (mIsWallpaper) {
        PlayerSnapshot s;
        s.volume = mSnapshot.volume;
        s.muted = mSnapshot.muted;
        s.state = PlaybackState::Idle;
        return s;
    }
    return mSnapshot;
}

bool MpvPlayer::HasMedia() const {
    std::lock_guard<std::mutex> lock(mMutex);
    if (mIsWallpaper) return false;
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
            bool isWallpaper = false;
            {
                std::lock_guard<std::mutex> lock(mMutex);
                isWallpaper = mIsWallpaper;
            }
            if (isWallpaper) {
                std::lock_guard<std::mutex> lock(mMutex);
                mSnapshot.state = PlaybackState::Idle;
                break;
            }
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
            bool isWallpaper = false;
            {
                std::lock_guard<std::mutex> lock(mMutex);
                isWallpaper = mIsWallpaper;
            }
            if (isWallpaper) {
                break;
            }

            // Um `loadfile replace` gera END_FILE(stop) do arquivo antigo: não é "ocioso".
            if (end->reason == MPV_END_FILE_REASON_STOP && mLoadRequested) break;

            {
                std::lock_guard<std::mutex> lock(mMutex);
                const int volume = mSnapshot.volume;
                const bool muted = mSnapshot.muted;
                const std::string err =
                    end->reason == MPV_END_FILE_REASON_ERROR ? mpv_error_string(end->error) : std::string();
                mSnapshot = PlayerSnapshot{};
                mSnapshot.volume = volume;
                mSnapshot.muted = muted;
                mSnapshot.lastError = err;
                mLoadRequested = false;
            }
            StartWallpaperLoop();
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
    if (mIsWallpaper) {
        if (id == kObsVolume && prop.format == MPV_FORMAT_DOUBLE) {
            mSnapshot.volume = std::clamp(static_cast<int>(*static_cast<double*>(prop.data) + 0.5), 0, 150);
        } else if (id == kObsMute && prop.format == MPV_FORMAT_FLAG) {
            mSnapshot.muted = *static_cast<int*>(prop.data) != 0;
        }
        return;
    }

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
                mSnapshot.volume = std::clamp(static_cast<int>(*static_cast<double*>(prop.data) + 0.5), 0, 150);
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
