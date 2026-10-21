#include "Upnp.hpp"

#include <cstring>

#include "../utils/Text.hpp"

namespace SwitchCast::Upnp {

namespace {

struct Arg { const char* name; bool in; const char* var; };
struct Action { const char* name; std::vector<Arg> args; };
struct StateVar {
    const char* name;
    const char* type;
    bool events;
    std::vector<const char*> allowed;
};
struct ServiceDef {
    std::vector<Action> actions;
    std::vector<StateVar> vars;
};

const char* const kIid = "A_ARG_TYPE_InstanceID";

const ServiceDef& AvTransportDef() {
    static const ServiceDef def{
        {
            {"SetAVTransportURI", {{"InstanceID", true, kIid}, {"CurrentURI", true, "AVTransportURI"},
                                   {"CurrentURIMetaData", true, "AVTransportURIMetaData"}}},
            {"GetMediaInfo", {{"InstanceID", true, kIid}, {"NrTracks", false, "NumberOfTracks"},
                              {"MediaDuration", false, "CurrentMediaDuration"},
                              {"CurrentURI", false, "AVTransportURI"},
                              {"CurrentURIMetaData", false, "AVTransportURIMetaData"},
                              {"NextURI", false, "NextAVTransportURI"},
                              {"NextURIMetaData", false, "NextAVTransportURIMetaData"},
                              {"PlayMedium", false, "PlaybackStorageMedium"},
                              {"RecordMedium", false, "RecordStorageMedium"},
                              {"WriteStatus", false, "RecordMediumWriteStatus"}}},
            {"GetTransportInfo", {{"InstanceID", true, kIid}, {"CurrentTransportState", false, "TransportState"},
                                  {"CurrentTransportStatus", false, "TransportStatus"},
                                  {"CurrentSpeed", false, "TransportPlaySpeed"}}},
            {"GetPositionInfo", {{"InstanceID", true, kIid}, {"Track", false, "CurrentTrack"},
                                 {"TrackDuration", false, "CurrentTrackDuration"},
                                 {"TrackMetaData", false, "CurrentTrackMetaData"},
                                 {"TrackURI", false, "CurrentTrackURI"},
                                 {"RelTime", false, "RelativeTimePosition"},
                                 {"AbsTime", false, "AbsoluteTimePosition"},
                                 {"RelCount", false, "RelativeCounterPosition"},
                                 {"AbsCount", false, "AbsoluteCounterPosition"}}},
            {"GetDeviceCapabilities", {{"InstanceID", true, kIid},
                                       {"PlayMedia", false, "PossiblePlaybackStorageMedia"},
                                       {"RecMedia", false, "PossibleRecordStorageMedia"},
                                       {"RecQualityModes", false, "PossibleRecordQualityModes"}}},
            {"GetTransportSettings", {{"InstanceID", true, kIid}, {"PlayMode", false, "CurrentPlayMode"},
                                      {"RecQualityMode", false, "CurrentRecordQualityMode"}}},
            {"Stop", {{"InstanceID", true, kIid}}},
            {"Play", {{"InstanceID", true, kIid}, {"Speed", true, "TransportPlaySpeed"}}},
            {"Pause", {{"InstanceID", true, kIid}}},
            {"Seek", {{"InstanceID", true, kIid}, {"Unit", true, "A_ARG_TYPE_SeekMode"},
                      {"Target", true, "A_ARG_TYPE_SeekTarget"}}},
            {"Next", {{"InstanceID", true, kIid}}},
            {"Previous", {{"InstanceID", true, kIid}}},
            {"GetCurrentTransportActions", {{"InstanceID", true, kIid},
                                            {"Actions", false, "CurrentTransportActions"}}},
        },
        {
            {"TransportState", "string", false,
             {"STOPPED", "PLAYING", "PAUSED_PLAYBACK", "TRANSITIONING", "NO_MEDIA_PRESENT"}},
            {"TransportStatus", "string", false, {"OK", "ERROR_OCCURRED"}},
            {"PlaybackStorageMedium", "string", false, {}},
            {"RecordStorageMedium", "string", false, {}},
            {"PossiblePlaybackStorageMedia", "string", false, {}},
            {"PossibleRecordStorageMedia", "string", false, {}},
            {"CurrentPlayMode", "string", false, {"NORMAL"}},
            {"TransportPlaySpeed", "string", false, {"1"}},
            {"RecordMediumWriteStatus", "string", false, {}},
            {"CurrentRecordQualityMode", "string", false, {}},
            {"PossibleRecordQualityModes", "string", false, {}},
            {"NumberOfTracks", "ui4", false, {}},
            {"CurrentTrack", "ui4", false, {}},
            {"CurrentTrackDuration", "string", false, {}},
            {"CurrentMediaDuration", "string", false, {}},
            {"CurrentTrackMetaData", "string", false, {}},
            {"CurrentTrackURI", "string", false, {}},
            {"AVTransportURI", "string", false, {}},
            {"AVTransportURIMetaData", "string", false, {}},
            {"NextAVTransportURI", "string", false, {}},
            {"NextAVTransportURIMetaData", "string", false, {}},
            {"RelativeTimePosition", "string", false, {}},
            {"AbsoluteTimePosition", "string", false, {}},
            {"RelativeCounterPosition", "i4", false, {}},
            {"AbsoluteCounterPosition", "ui4", false, {}},
            {"CurrentTransportActions", "string", false, {}},
            {"LastChange", "string", true, {}},
            {"A_ARG_TYPE_SeekMode", "string", false, {"REL_TIME", "ABS_TIME", "TRACK_NR"}},
            {"A_ARG_TYPE_SeekTarget", "string", false, {}},
            {kIid, "ui4", false, {}},
        }};
    return def;
}

const ServiceDef& RenderingControlDef() {
    static const ServiceDef def{
        {
            {"ListPresets", {{"InstanceID", true, kIid}, {"CurrentPresetNameList", false, "PresetNameList"}}},
            {"SelectPreset", {{"InstanceID", true, kIid}, {"PresetName", true, "A_ARG_TYPE_PresetName"}}},
            {"GetMute", {{"InstanceID", true, kIid}, {"Channel", true, "A_ARG_TYPE_Channel"},
                         {"CurrentMute", false, "Mute"}}},
            {"SetMute", {{"InstanceID", true, kIid}, {"Channel", true, "A_ARG_TYPE_Channel"},
                         {"DesiredMute", true, "Mute"}}},
            {"GetVolume", {{"InstanceID", true, kIid}, {"Channel", true, "A_ARG_TYPE_Channel"},
                           {"CurrentVolume", false, "Volume"}}},
            {"SetVolume", {{"InstanceID", true, kIid}, {"Channel", true, "A_ARG_TYPE_Channel"},
                           {"DesiredVolume", true, "Volume"}}},
        },
        {
            {"LastChange", "string", true, {}},
            {"PresetNameList", "string", false, {}},
            {"Mute", "boolean", false, {}},
            {"Volume", "ui2", false, {}},
            {"A_ARG_TYPE_Channel", "string", false, {"Master"}},
            {"A_ARG_TYPE_PresetName", "string", false, {"FactoryDefaults"}},
            {kIid, "ui4", false, {}},
        }};
    return def;
}

const ServiceDef& ConnectionManagerDef() {
    static const ServiceDef def{
        {
            {"GetProtocolInfo", {{"Source", false, "SourceProtocolInfo"}, {"Sink", false, "SinkProtocolInfo"}}},
            {"GetCurrentConnectionIDs", {{"ConnectionIDs", false, "CurrentConnectionIDs"}}},
            {"GetCurrentConnectionInfo", {{"ConnectionID", true, "A_ARG_TYPE_ConnectionID"},
                                          {"RcsID", false, "A_ARG_TYPE_RcsID"},
                                          {"AVTransportID", false, "A_ARG_TYPE_AVTransportID"},
                                          {"ProtocolInfo", false, "A_ARG_TYPE_ProtocolInfo"},
                                          {"PeerConnectionManager", false, "A_ARG_TYPE_ConnectionManager"},
                                          {"PeerConnectionID", false, "A_ARG_TYPE_ConnectionID"},
                                          {"Direction", false, "A_ARG_TYPE_Direction"},
                                          {"Status", false, "A_ARG_TYPE_ConnectionStatus"}}},
        },
        {
            {"SourceProtocolInfo", "string", true, {}},
            {"SinkProtocolInfo", "string", true, {}},
            {"CurrentConnectionIDs", "string", true, {}},
            {"A_ARG_TYPE_ConnectionStatus", "string", false, {"OK", "Unknown"}},
            {"A_ARG_TYPE_ConnectionManager", "string", false, {}},
            {"A_ARG_TYPE_Direction", "string", false, {"Input", "Output"}},
            {"A_ARG_TYPE_ProtocolInfo", "string", false, {}},
            {"A_ARG_TYPE_ConnectionID", "i4", false, {}},
            {"A_ARG_TYPE_AVTransportID", "i4", false, {}},
            {"A_ARG_TYPE_RcsID", "i4", false, {}},
        }};
    return def;
}

const ServiceDef& DefFor(Service s) {
    switch (s) {
        case Service::AVTransport:       return AvTransportDef();
        case Service::RenderingControl:  return RenderingControlDef();
        case Service::ConnectionManager: return ConnectionManagerDef();
    }
    return AvTransportDef();
}

constexpr const char* kXmlHeader = "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n";

std::string ServiceBlock(Service s) {
    const std::string name = ServiceName(s);
    return std::string("<service><serviceType>") + ServiceType(s) + "</serviceType><serviceId>urn:upnp-org:serviceId:" +
           name + "</serviceId><SCPDURL>/upnp/scpd/" + name + ".xml</SCPDURL><controlURL>/upnp/control/" + name +
           "</controlURL><eventSubURL>/upnp/event/" + name + "</eventSubURL></service>";
}

}  // namespace

const char* ServiceType(Service s) {
    switch (s) {
        case Service::AVTransport:       return "urn:schemas-upnp-org:service:AVTransport:1";
        case Service::RenderingControl:  return "urn:schemas-upnp-org:service:RenderingControl:1";
        case Service::ConnectionManager: return "urn:schemas-upnp-org:service:ConnectionManager:1";
    }
    return "";
}

const char* ServiceName(Service s) {
    switch (s) {
        case Service::AVTransport:       return "AVTransport";
        case Service::RenderingControl:  return "RenderingControl";
        case Service::ConnectionManager: return "ConnectionManager";
    }
    return "";
}

bool ServiceFromName(const std::string& name, Service& out) {
    for (Service s : {Service::AVTransport, Service::RenderingControl, Service::ConnectionManager}) {
        if (name == ServiceName(s)) { out = s; return true; }
    }
    return false;
}

std::string BuildDeviceDescription(const DeviceIdentity& id) {
    std::string xml = kXmlHeader;
    xml += "<root xmlns=\"urn:schemas-upnp-org:device-1-0\" xmlns:dlna=\"urn:schemas-dlna-org:device-1-0\">"
           "<specVersion><major>1</major><minor>0</minor></specVersion><device>"
           "<dlna:X_DLNADOC>DMR-1.50</dlna:X_DLNADOC>";
    xml += std::string("<deviceType>") + kDeviceType + "</deviceType>";
    xml += "<friendlyName>" + Text::XmlEscape(id.friendlyName) + "</friendlyName>";
    xml += "<manufacturer>Aleff959</manufacturer><modelDescription>SwitchCast Media Renderer</modelDescription>"
           "<modelName>SwitchCast</modelName><modelNumber>" SWITCHCAST_VERSION_STR "</modelNumber>";
    xml += "<UDN>uuid:" + id.uuid + "</UDN><serviceList>";
    xml += ServiceBlock(Service::AVTransport);
    xml += ServiceBlock(Service::RenderingControl);
    xml += ServiceBlock(Service::ConnectionManager);
    xml += "</serviceList></device></root>";
    return xml;
}

std::string BuildScpd(Service s) {
    const ServiceDef& def = DefFor(s);
    std::string xml = kXmlHeader;
    xml += "<scpd xmlns=\"urn:schemas-upnp-org:service-1-0\"><specVersion><major>1</major><minor>0</minor>"
           "</specVersion><actionList>";
    for (const Action& a : def.actions) {
        xml += std::string("<action><name>") + a.name + "</name><argumentList>";
        for (const Arg& arg : a.args) {
            xml += std::string("<argument><name>") + arg.name + "</name><direction>" + (arg.in ? "in" : "out") +
                   "</direction><relatedStateVariable>" + arg.var + "</relatedStateVariable></argument>";
        }
        xml += "</argumentList></action>";
    }
    xml += "</actionList><serviceStateTable>";
    for (const StateVar& v : def.vars) {
        xml += std::string("<stateVariable sendEvents=\"") + (v.events ? "yes" : "no") + "\"><name>" + v.name +
               "</name><dataType>" + v.type + "</dataType>";
        if (!v.allowed.empty()) {
            xml += "<allowedValueList>";
            for (const char* val : v.allowed) xml += std::string("<allowedValue>") + val + "</allowedValue>";
            xml += "</allowedValueList>";
        }
        xml += "</stateVariable>";
    }
    xml += "</serviceStateTable></scpd>";
    return xml;
}

bool ParseSoapRequest(Service service, const std::string& header, const std::string& body, SoapRequest& out) {
    std::string h = Text::Trim(header);
    if (!h.empty() && h.front() == '"') h.erase(0, 1);
    if (!h.empty() && h.back() == '"') h.pop_back();
    const std::size_t hash = h.find('#');
    if (hash == std::string::npos || hash + 1 >= h.size()) return false;

    out = SoapRequest{};
    out.service = service;
    out.action = h.substr(hash + 1);

    // Varredura simples: todo elemento-folha <Nome>texto</Nome> vira um argumento.
    // Envelope/Body/<u:Action> têm filhos, portanto nunca são folhas.
    std::size_t pos = 0;
    while ((pos = body.find('<', pos)) != std::string::npos) {
        const std::size_t close = body.find('>', pos);
        if (close == std::string::npos) break;
        if (body[pos + 1] == '/' || body[pos + 1] == '?' || body[pos + 1] == '!' || body[close - 1] == '/') {
            pos = close + 1;
            continue;
        }
        std::string tag = body.substr(pos + 1, close - pos - 1);
        const std::size_t space = tag.find_first_of(" \t\r\n");
        if (space != std::string::npos) tag.resize(space);
        const std::size_t colon = tag.find(':');
        const std::string local = colon == std::string::npos ? tag : tag.substr(colon + 1);

        const std::size_t next = body.find('<', close + 1);
        if (next == std::string::npos) break;
        if (body.compare(next, 2, "</") == 0) {
            out.args[local] = Text::XmlUnescape(body.substr(close + 1, next - close - 1));
        }
        pos = close + 1;
    }
    return true;
}

std::string BuildSoapResponse(Service s, const std::string& action, const OutArgs& args) {
    std::string xml = kXmlHeader;
    xml += "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" "
           "s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\"><s:Body>";
    xml += "<u:" + action + "Response xmlns:u=\"" + ServiceType(s) + "\">";
    for (const auto& [name, value] : args) xml += "<" + name + ">" + Text::XmlEscape(value) + "</" + name + ">";
    xml += "</u:" + action + "Response></s:Body></s:Envelope>";
    return xml;
}

std::string BuildSoapFault(int code, const std::string& description) {
    std::string xml = kXmlHeader;
    xml += "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" "
           "s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\"><s:Body><s:Fault>"
           "<faultcode>s:Client</faultcode><faultstring>UPnPError</faultstring><detail>"
           "<UPnPError xmlns=\"urn:schemas-upnp-org:control-1-0\"><errorCode>";
    xml += std::to_string(code) + "</errorCode><errorDescription>" + Text::XmlEscape(description) +
           "</errorDescription></UPnPError></detail></s:Fault></s:Body></s:Envelope>";
    return xml;
}

std::string ExtractTagText(const std::string& xml, const std::string& tag) {
    const std::string open = "<" + tag;
    std::size_t start = 0;
    while ((start = xml.find(open, start)) != std::string::npos) {
        const char after = start + open.size() < xml.size() ? xml[start + open.size()] : '\0';
        if (after == '>' || after == ' ' || after == '/') break;  // evita casar <dc:titleX>
        start += open.size();
    }
    if (start == std::string::npos) return {};
    const std::size_t gt = xml.find('>', start);
    if (gt == std::string::npos || xml[gt - 1] == '/') return {};
    const std::size_t end = xml.find("</" + tag + ">", gt);
    if (end == std::string::npos) return {};
    return Text::XmlUnescape(xml.substr(gt + 1, end - gt - 1));
}

DidlInfo ParseDidl(const std::string& didl) {
    DidlInfo info;
    info.title = ExtractTagText(didl, "dc:title");
    // Samsung/BubbleUPnP publicam legendas em <sec:CaptionInfoEx> ou <sec:CaptionInfo>.
    info.subtitleUrl = Text::Trim(ExtractTagText(didl, "sec:CaptionInfoEx"));
    if (info.subtitleUrl.empty()) info.subtitleUrl = Text::Trim(ExtractTagText(didl, "sec:CaptionInfo"));
    return info;
}

}  // namespace SwitchCast::Upnp
