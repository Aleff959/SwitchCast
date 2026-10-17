// network/Upnp.hpp - descrições XML, SCPDs e (de)serialização SOAP do MediaRenderer.
#pragma once

#include <map>
#include <string>
#include <utility>
#include <vector>

#include "Types.hpp"

namespace SwitchCast::Upnp {

inline constexpr const char* kDeviceType = "urn:schemas-upnp-org:device:MediaRenderer:1";

enum class Service { AVTransport, RenderingControl, ConnectionManager };

const char* ServiceType(Service s);  // urn:schemas-upnp-org:service:AVTransport:1
const char* ServiceName(Service s);  // AVTransport (usado nas rotas /upnp/...)
bool ServiceFromName(const std::string& name, Service& out);

std::string BuildDeviceDescription(const DeviceIdentity& id);
std::string BuildScpd(Service s);

struct SoapRequest {
    Service service = Service::AVTransport;
    std::string action;
    std::map<std::string, std::string> args;  // valores já com entidades XML decodificadas
};

// `soapActionHeader`: ex. "\"urn:schemas-upnp-org:service:AVTransport:1#Play\"".
bool ParseSoapRequest(Service service, const std::string& soapActionHeader, const std::string& body,
                      SoapRequest& out);

using OutArgs = std::vector<std::pair<std::string, std::string>>;  // valores em texto puro
std::string BuildSoapResponse(Service s, const std::string& action, const OutArgs& out);
std::string BuildSoapFault(int code, const std::string& description);

struct DidlInfo {
    std::string title;
    std::string subtitleUrl;
};
// `didl`: XML DIDL-Lite já decodificado (texto do argumento CurrentURIMetaData).
DidlInfo ParseDidl(const std::string& didl);

// Retorna o texto entre <tag ...> e </tag> (primeira ocorrência) já decodificado.
std::string ExtractTagText(const std::string& xml, const std::string& tag);

}  // namespace SwitchCast::Upnp
