#include "UpnpService.hpp"

#include <algorithm>
#include <cstdlib>

#include "../utils/Text.hpp"

namespace SwitchCast {

using Upnp::OutArgs;
using Upnp::Service;
using Upnp::SoapRequest;

namespace {

const char* TransportState(const PlayerSnapshot& s) {
    switch (s.state) {
        case PlaybackState::Idle:    return "NO_MEDIA_PRESENT";
        case PlaybackState::Loading: return "TRANSITIONING";
        case PlaybackState::Playing: return s.buffering ? "TRANSITIONING" : "PLAYING";
        case PlaybackState::Paused:  return "PAUSED_PLAYBACK";
    }
    return "STOPPED";
}

bool ParseBool(const std::string& v) {
    return v == "1" || Text::EqualsIgnoreCase(v, "true") || Text::EqualsIgnoreCase(v, "yes");
}

constexpr const char* kSinkProtocols =
    "http-get:*:video/mp4:*,http-get:*:video/x-matroska:*,http-get:*:video/webm:*,"
    "http-get:*:video/mpeg:*,http-get:*:video/x-msvideo:*,http-get:*:video/quicktime:*,"
    "http-get:*:video/MP2T:*,http-get:*:application/vnd.apple.mpegurl:*,"
    "http-get:*:application/x-mpegURL:*,http-get:*:audio/mpeg:*,http-get:*:audio/mp4:*,"
    "http-get:*:audio/flac:*,http-get:*:audio/x-wav:*,http-get:*:audio/ogg:*";

}  // namespace

UpnpService::Result UpnpService::Fault(int code, const char* description) const {
    return {500, Upnp::BuildSoapFault(code, description)};
}

UpnpService::Result UpnpService::Ok(Service s, const std::string& action, const OutArgs& out) const {
    return {200, Upnp::BuildSoapResponse(s, action, out)};
}

UpnpService::Result UpnpService::HandleControl(Service service, const std::string& header,
                                               const std::string& body) {
    SoapRequest req;
    if (!Upnp::ParseSoapRequest(service, header, body, req)) return Fault(401, "Invalid Action");
    switch (service) {
        case Service::AVTransport:       return HandleAvTransport(req);
        case Service::RenderingControl:  return HandleRenderingControl(req);
        case Service::ConnectionManager: return HandleConnectionManager(req);
    }
    return Fault(401, "Invalid Action");
}

UpnpService::Result UpnpService::HandleAvTransport(const SoapRequest& req) {
    const Service svc = Service::AVTransport;
    const std::string& a = req.action;
    auto arg = [&req](const char* k) {
        auto it = req.args.find(k);
        return it == req.args.end() ? std::string() : it->second;
    };

    if (a == "SetAVTransportURI") {
        const std::string uri = Text::Trim(arg("CurrentURI"));
        if (uri.empty()) return Fault(714, "Illegal MIME-type");
        const std::string meta = arg("CurrentURIMetaData");
        const Upnp::DidlInfo didl = Upnp::ParseDidl(meta);

        {
            std::lock_guard<std::mutex> lock(mMutex);
            mCurrentUri = uri;
            mCurrentMetadata = meta;
        }
        NetworkCommand cmd;
        cmd.type = NetworkCommand::Type::LoadMedia;
        cmd.media.url = uri;
        cmd.media.title = didl.title;
        cmd.media.autoPlay = false;  // DLNA: o controlador envia "Play" em seguida
        if (!didl.subtitleUrl.empty()) cmd.media.subtitles.push_back(didl.subtitleUrl);
        if (!mSubmit(std::move(cmd))) return Fault(501, "Action Failed");
        return Ok(svc, a);
    }

    if (a == "Play") {
        NetworkCommand cmd;
        cmd.type = NetworkCommand::Type::Play;
        mSubmit(std::move(cmd));
        return Ok(svc, a);
    }
    if (a == "Pause") {
        NetworkCommand cmd;
        cmd.type = NetworkCommand::Type::Pause;
        mSubmit(std::move(cmd));
        return Ok(svc, a);
    }
    if (a == "Stop") {
        NetworkCommand cmd;
        cmd.type = NetworkCommand::Type::Stop;
        mSubmit(std::move(cmd));
        return Ok(svc, a);
    }
    if (a == "Seek") {
        const std::string unit = arg("Unit");
        int64_t ms = 0;
        if (unit != "REL_TIME" && unit != "ABS_TIME") return Fault(710, "Seek mode not supported");
        if (!Text::ParseUpnpTime(arg("Target"), ms)) return Fault(711, "Illegal seek target");
        NetworkCommand cmd;
        cmd.type = NetworkCommand::Type::Seek;
        cmd.positionMs = ms;
        mSubmit(std::move(cmd));
        return Ok(svc, a);
    }
    if (a == "Next" || a == "Previous") return Ok(svc, a);

    if (a == "GetTransportInfo") {
        const PlayerSnapshot s = mSnapshot();
        return Ok(svc, a, {{"CurrentTransportState", TransportState(s)},
                           {"CurrentTransportStatus", s.lastError.empty() ? "OK" : "ERROR_OCCURRED"},
                           {"CurrentSpeed", "1"}});
    }
    if (a == "GetPositionInfo") {
        const PlayerSnapshot s = mSnapshot();
        std::string uri, meta;
        {
            std::lock_guard<std::mutex> lock(mMutex);
            uri = s.state == PlaybackState::Idle ? std::string() : mCurrentUri;
            meta = s.state == PlaybackState::Idle ? std::string() : mCurrentMetadata;
        }
        const std::string rel = Text::FormatUpnpTime(s.positionMs);
        return Ok(svc, a, {{"Track", s.state == PlaybackState::Idle ? "0" : "1"},
                           {"TrackDuration", Text::FormatUpnpTime(s.durationMs)},
                           {"TrackMetaData", meta},
                           {"TrackURI", uri},
                           {"RelTime", rel},
                           {"AbsTime", rel},
                           {"RelCount", "2147483647"},
                           {"AbsCount", "2147483647"}});
    }
    if (a == "GetMediaInfo") {
        const PlayerSnapshot s = mSnapshot();
        std::string uri, meta;
        {
            std::lock_guard<std::mutex> lock(mMutex);
            uri = mCurrentUri;
            meta = mCurrentMetadata;
        }
        return Ok(svc, a, {{"NrTracks", uri.empty() ? "0" : "1"},
                           {"MediaDuration", Text::FormatUpnpTime(s.durationMs)},
                           {"CurrentURI", uri},
                           {"CurrentURIMetaData", meta},
                           {"NextURI", ""},
                           {"NextURIMetaData", ""},
                           {"PlayMedium", "NETWORK"},
                           {"RecordMedium", "NOT_IMPLEMENTED"},
                           {"WriteStatus", "NOT_IMPLEMENTED"}});
    }
    if (a == "GetTransportSettings")
        return Ok(svc, a, {{"PlayMode", "NORMAL"}, {"RecQualityMode", "NOT_IMPLEMENTED"}});
    if (a == "GetDeviceCapabilities")
        return Ok(svc, a, {{"PlayMedia", "NETWORK"}, {"RecMedia", "NOT_IMPLEMENTED"},
                           {"RecQualityModes", "NOT_IMPLEMENTED"}});
    if (a == "GetCurrentTransportActions") return Ok(svc, a, {{"Actions", "Play,Pause,Stop,Seek"}});

    return Fault(401, "Invalid Action");
}

UpnpService::Result UpnpService::HandleRenderingControl(const SoapRequest& req) {
    const Service svc = Service::RenderingControl;
    const std::string& a = req.action;
    auto arg = [&req](const char* k) {
        auto it = req.args.find(k);
        return it == req.args.end() ? std::string() : it->second;
    };

    if (a == "GetVolume") return Ok(svc, a, {{"CurrentVolume", std::to_string(mSnapshot().volume)}});
    if (a == "SetVolume") {
        char* end = nullptr;
        const std::string v = arg("DesiredVolume");
        const long level = std::strtol(v.c_str(), &end, 10);
        if (end == v.c_str()) return Fault(402, "Invalid Args");
        NetworkCommand cmd;
        cmd.type = NetworkCommand::Type::SetVolume;
        cmd.volume = static_cast<int>(std::clamp<long>(level, 0, 150));
        mSubmit(std::move(cmd));
        return Ok(svc, a);
    }
    if (a == "GetMute") return Ok(svc, a, {{"CurrentMute", mSnapshot().muted ? "1" : "0"}});
    if (a == "SetMute") {
        NetworkCommand cmd;
        cmd.type = NetworkCommand::Type::SetMute;
        cmd.flag = ParseBool(arg("DesiredMute"));
        mSubmit(std::move(cmd));
        return Ok(svc, a);
    }
    if (a == "ListPresets") return Ok(svc, a, {{"CurrentPresetNameList", "FactoryDefaults"}});
    if (a == "SelectPreset") return Ok(svc, a);
    return Fault(401, "Invalid Action");
}

UpnpService::Result UpnpService::HandleConnectionManager(const SoapRequest& req) {
    const Service svc = Service::ConnectionManager;
    const std::string& a = req.action;
    if (a == "GetProtocolInfo") return Ok(svc, a, {{"Source", ""}, {"Sink", kSinkProtocols}});
    if (a == "GetCurrentConnectionIDs") return Ok(svc, a, {{"ConnectionIDs", "0"}});
    if (a == "GetCurrentConnectionInfo")
        return Ok(svc, a, {{"RcsID", "0"}, {"AVTransportID", "0"}, {"ProtocolInfo", ""},
                           {"PeerConnectionManager", ""}, {"PeerConnectionID", "-1"},
                           {"Direction", "Input"}, {"Status", "OK"}});
    return Fault(401, "Invalid Action");
}

}  // namespace SwitchCast
