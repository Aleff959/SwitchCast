#include "updater/ModuleManager.hpp"
#include "updater/OtaUpdater.hpp"
#include <nlohmann/json.hpp>
#include <fstream>
#include <sys/stat.h>
#include <cstdio>

namespace SwitchCast::Updater {

using json = nlohmann::json;

namespace {
    void EnsureDirectories(const std::string& path) {
        std::string current;
        for (char c : path) {
            if (c == '/' || c == '\\') {
                if (!current.empty() && current != "sdmc:" && current != "sdmc:/switch") {
                    mkdir(current.c_str(), 0777);
                }
            }
            current += c;
        }
    }
}

ModuleManager& ModuleManager::Instance() {
    static ModuleManager instance;
    return instance;
}

bool ModuleManager::LoadInstalledModules() {
    std::lock_guard<std::mutex> lock(mMutex);
    mModules.clear();

    std::ifstream ifs(kLocalManifest);
    if (!ifs.is_open()) return false;

    try {
        json root = json::parse(ifs);
        if (root.contains("modules") && root["modules"].is_array()) {
            for (const auto& item : root["modules"]) {
                ModuleDescriptor desc;
                desc.id = item.value("id", "");
                desc.name = item.value("name", "");
                desc.version = item.value("version", "");
                desc.entryFile = item.value("entry_file", "");
                desc.sha256 = item.value("sha256", "");
                desc.hotReload = item.value("hot_reload", true);
                desc.enabled = item.value("enabled", true);

                if (!desc.id.empty()) {
                    mModules[desc.id] = desc;
                }
            }
        }
        return true;
    } catch (...) {
        return false;
    }
}

bool ModuleManager::InstallOrUpdateModule(const ModuleDescriptor& descriptor, const std::string& tempPayloadPath) {
    if (!OtaUpdater::VerifySha256(tempPayloadPath, descriptor.sha256)) {
        std::remove(tempPayloadPath.c_str());
        return false;
    }

    std::string destination = std::string(kModulesDir) + "/" + descriptor.entryFile;
    EnsureDirectories(destination);

    std::remove(destination.c_str());
    if (std::rename(tempPayloadPath.c_str(), destination.c_str()) != 0) {
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(mMutex);
        mModules[descriptor.id] = descriptor;
    }

    SaveLocalManifest();

    if (descriptor.hotReload) {
        ReloadModuleInMemory(descriptor.id);
    }
    return true;
}

bool ModuleManager::ReloadModuleInMemory(const std::string& moduleId) {
    std::lock_guard<std::mutex> lock(mMutex);
    auto it = mModules.find(moduleId);
    if (it == mModules.end() || !it->second.enabled) return false;

    // Notificação ou hot-reload de listeners/scripts
    return true;
}

bool ModuleManager::DisableModule(const std::string& moduleId) {
    std::lock_guard<std::mutex> lock(mMutex);
    auto it = mModules.find(moduleId);
    if (it == mModules.end()) return false;

    it->second.enabled = false;
    SaveLocalManifest();
    return true;
}

std::vector<ModuleDescriptor> ModuleManager::GetInstalledModules() const {
    std::lock_guard<std::mutex> lock(mMutex);
    std::vector<ModuleDescriptor> out;
    out.reserve(mModules.size());
    for (const auto& [_, desc] : mModules) {
        out.push_back(desc);
    }
    return out;
}

bool ModuleManager::SaveLocalManifest() {
    EnsureDirectories(std::string(kModulesDir) + "/dummy");

    json root;
    root["schema_version"] = 1;
    root["modules"] = json::array();

    for (const auto& [_, desc] : mModules) {
        json item;
        item["id"] = desc.id;
        item["name"] = desc.name;
        item["version"] = desc.version;
        item["entry_file"] = desc.entryFile;
        item["sha256"] = desc.sha256;
        item["hot_reload"] = desc.hotReload;
        item["enabled"] = desc.enabled;
        root["modules"].push_back(item);
    }

    std::ofstream ofs(kLocalManifest, std::ios::trunc);
    if (!ofs.is_open()) return false;
    ofs << root.dump(2);
    return true;
}

} // namespace SwitchCast::Updater
