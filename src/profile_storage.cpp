
#include "profile_storage.h"

#include <mod/amlmod.h>

#include <cstdio>
#include <vector>

#include <string>

extern IAML* aml;

namespace VLS {
namespace ProfileStorage {

namespace {

static std::string g_profilesDirectory;
static bool g_initialized = false;

static const size_t MAX_PROFILE_SIZE = 1024 * 1024;

static bool BuildProfilePath(int modelId, char* output, size_t outputSize) {
    if (g_profilesDirectory.empty() || output == nullptr) {
        return false;
    }

    char fileName[32];

    if (modelId < 0) {
        std::snprintf(fileName, sizeof(fileName), "default.json");
    } else {
        std::snprintf(fileName, sizeof(fileName), "%d.json", modelId);
    }

    int result = std::snprintf(
        output,
        outputSize,
        "%s/%s",
        g_profilesDirectory.c_str(),
        fileName
    );

    return result > 0 && static_cast<size_t>(result) < outputSize;
}

}

bool Init() {
    if (g_initialized) {
        return true;
    }

    if (aml == nullptr) {
        return false;
    }

    const char* basePath = aml->GetInternalModsPath();

    if (basePath == nullptr || basePath[0] == '\0') {
        return false;
    }

    std::string rootDirectory(basePath);

    if (rootDirectory.back() != '/') {
        rootDirectory += '/';
    }

    rootDirectory += "VehicleLEDSystem";

    if (!aml->IsDirectory(rootDirectory.c_str())) {
        if (!aml->CreateDirRecursive(rootDirectory.c_str())) {
            return false;
        }
    }

    g_profilesDirectory = rootDirectory + "/profiles";

    if (!aml->IsDirectory(g_profilesDirectory.c_str())) {
        if (!aml->CreateDirRecursive(g_profilesDirectory.c_str())) {
            g_profilesDirectory.clear();
            return false;
        }
    }

    g_initialized = true;

    return true;
}

const char* GetProfilesDirectory() {
    if (!g_initialized) {
        return nullptr;
    }

    return g_profilesDirectory.c_str();
}

bool SaveProfileJson(int modelId, const std::string& json) {
    if (!g_initialized || aml == nullptr || json.empty()) {
        return false;
    }

    if (json.size() > MAX_PROFILE_SIZE) {
        return false;
    }

    char filePath[512];

    if (!BuildProfilePath(modelId, filePath, sizeof(filePath))) {
        return false;
    }

    return aml->WriteBufferToFile(
        filePath,
        json.data(),
        json.size()
    );
}

bool LoadProfileJson(int modelId, std::string& json) {
    json.clear();

    if (!g_initialized || aml == nullptr) {
        return false;
    }

    char filePath[512];

    if (!BuildProfilePath(modelId, filePath, sizeof(filePath))) {
        return false;
    }

    if (!aml->FileExists(filePath)) {
        return false;
    }

    size_t fileSize = aml->FileSize(filePath);

    if (fileSize == 0 || fileSize > MAX_PROFILE_SIZE) {
        return false;
    }

    std::vector<char> buffer(fileSize + 1, '\0');

    int bytesRead = aml->ReadFileToBuffer(
        filePath,
        buffer.data(),
        fileSize + 1
    );

    if (bytesRead <= 0 || static_cast<size_t>(bytesRead) > fileSize) {
        return false;
    }

    buffer[bytesRead] = '\0';

    json.assign(buffer.data(), static_cast<size_t>(bytesRead));

    return true;
}

}
}
