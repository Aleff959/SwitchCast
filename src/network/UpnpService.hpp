// network/UpnpService.hpp - traduz ações SOAP (AVTransport/RenderingControl/ConnectionManager)
// em NetworkCommands e responde com o estado em cache do player.
#pragma once

#include <functional>
#include <mutex>
#include <string>

#include "Command.hpp"
#include "Types.hpp"
#include "Upnp.hpp"

namespace SwitchCast {

class UpnpService {
public:
    using SubmitFn = std::function<bool(NetworkCommand)>;
    using SnapshotFn = std::function<PlayerSnapshot()>;

    UpnpService(SubmitFn submit, SnapshotFn snapshot)
        : mSubmit(std::move(submit)), mSnapshot(std::move(snapshot)) {}

    struct Result {
        int httpStatus = 200;
        std::string body;
    };

    Result HandleControl(Upnp::Service service, const std::string& soapActionHeader, const std::string& body);

private:
    Result Fault(int code, const char* description) const;
    Result Ok(Upnp::Service s, const std::string& action, const Upnp::OutArgs& out = {}) const;

    Result HandleAvTransport(const Upnp::SoapRequest& req);
    Result HandleRenderingControl(const Upnp::SoapRequest& req);
    Result HandleConnectionManager(const Upnp::SoapRequest& req);

    SubmitFn mSubmit;
    SnapshotFn mSnapshot;

    std::mutex mMutex;
    std::string mCurrentUri;       // último SetAVTransportURI
    std::string mCurrentMetadata;  // DIDL-Lite decodificado
};

}  // namespace SwitchCast
