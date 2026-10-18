#pragma once
#include <string>
#include <vector>
#include <unordered_map>
#include <mutex>

namespace SwitchCast::Updater {

struct ModuleDescriptor {
    std::string id;          // Ex: "protocol.roku_bridge"
    std::string name;        // Ex: "Roku & DLNA Extended Bridge"
    std::string version;     // Ex: "1.0.2"
    std::string entryFile;   // Ex: "protocols/roku_bridge.js"
    std::string sha256;
    bool hotReload = true;
    bool enabled = true;
};

class ModuleManager {
public:
    static ModuleManager& Instance();

    bool LoadInstalledModules();
    bool InstallOrUpdateModule(const ModuleDescriptor& descriptor, const std::string& tempPayloadPath);
    bool ReloadModuleInMemory(const std::string& moduleId);
    bool DisableModule(const std::string& moduleId);
    std::vector<ModuleDescriptor> GetInstalledModules() const;

private:
    ModuleManager() = default;
    bool SaveLocalManifest();

    mutable std::mutex mMutex;
    std::unordered_map<std::string, ModuleDescriptor> mModules;

    static constexpr const char* kModulesDir    = "sdmc:/switch/SwitchCast/modules";
    static constexpr const char* kLocalManifest = "sdmc:/switch/SwitchCast/modules/manifest.json";
};

} // namespace SwitchCast::Updater
