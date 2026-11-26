
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
static std::string g_diagnosticsDirectory;
static std::string g_fontsDirectory;
static std::string g_texturesDirectory;
static bool g_initialized = false;

static const size_t MAX_PROFILE_SIZE = 1024 * 1024;
static const size_t MAX_DIAGNOSTIC_SIZE = 256 * 1024;
static const size_t MAX_FONT_SIZE = 4096;
// Encoded image ceiling; decoded RGBA is separately bounded to 1024x1024.
static const size_t MAX_TEXTURE_SIZE = 4 * 1024 * 1024;

static bool BuildProfilePath(
    int modelId,
    char* output,
    size_t outputSize
) {
    if (g_profilesDirectory.empty() ||
        output == nullptr ||
        outputSize == 0) {
        return false;
    }

    char fileName[32];

    if (modelId < 0) {
        std::snprintf(
            fileName,
            sizeof(fileName),
            "default.json"
        );
    } else {
        std::snprintf(
            fileName,
            sizeof(fileName),
            "%d.json",
            modelId
        );
    }

    const int result = std::snprintf(
        output,
        outputSize,
        "%s/%s",
        g_profilesDirectory.c_str(),
        fileName
    );

    return result > 0 &&
        static_cast<size_t>(result) < outputSize;
}

static bool IsSafeDiagnosticFileName(
    const std::string& fileName
) {
    if (fileName.empty() || fileName.size() > 64)
        return false;

    for (size_t i = 0; i < fileName.size(); ++i) {
        const char c = fileName[i];

        const bool valid =
            (c >= 'a' && c <= 'z') ||
            (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') ||
            c == '_' ||
            c == '-' ||
            c == '.';

        if (!valid)
            return false;
    }

    if (fileName == "." || fileName == "..")
        return false;

    return true;
}

static bool BuildDiagnosticPath(
    const std::string& fileName,
    char* output,
    size_t outputSize
) {
    if (g_diagnosticsDirectory.empty() ||
        output == nullptr ||
        outputSize == 0 ||
        !IsSafeDiagnosticFileName(fileName)) {
        return false;
    }

    const int result = std::snprintf(
        output,
        outputSize,
        "%s/%s",
        g_diagnosticsDirectory.c_str(),
        fileName.c_str()
    );

    return result > 0 &&
        static_cast<size_t>(result) < outputSize;
}

}

bool Init() {
    if (g_initialized)
        return true;

    if (aml == nullptr)
        return false;

    const char* basePath = aml->GetInternalModsPath();

    if (basePath == nullptr || basePath[0] == '\0')
        return false;

    std::string rootDirectory(basePath);

    if (rootDirectory.back() != '/')
        rootDirectory += '/';

    rootDirectory += "VehicleLEDSystem";

    if (!aml->IsDirectory(rootDirectory.c_str())) {
        if (!aml->CreateDirRecursive(rootDirectory.c_str()))
            return false;
    }

    g_profilesDirectory = rootDirectory + "/profiles";

    if (!aml->IsDirectory(g_profilesDirectory.c_str())) {
        if (!aml->CreateDirRecursive(g_profilesDirectory.c_str())) {
            g_profilesDirectory.clear();
            return false;
        }
    }

    g_diagnosticsDirectory = rootDirectory + "/diagnostics";

    if (!aml->IsDirectory(g_diagnosticsDirectory.c_str())) {
        if (!aml->CreateDirRecursive(g_diagnosticsDirectory.c_str())) {
            g_diagnosticsDirectory.clear();
            return false;
        }
    }

    g_fontsDirectory = rootDirectory + "/fonts";
    if (!aml->IsDirectory(g_fontsDirectory.c_str())) {
        if (!aml->CreateDirRecursive(g_fontsDirectory.c_str())) {
            g_fontsDirectory.clear();
            return false;
        }
    }

    g_texturesDirectory = rootDirectory + "/textures";
    if (!aml->IsDirectory(g_texturesDirectory.c_str())) {
        if (!aml->CreateDirRecursive(g_texturesDirectory.c_str())) {
            g_texturesDirectory.clear();
            return false;
        }
    }

    g_initialized = true;

    return true;
}

const char* GetProfilesDirectory() {
    if (!g_initialized)
        return nullptr;

    return g_profilesDirectory.c_str();
}

const char* GetDiagnosticsDirectory() {
    if (!g_initialized)
        return nullptr;

    return g_diagnosticsDirectory.c_str();
}

bool SaveProfileJson(
    int modelId,
    const std::string& json
) {
    if (!g_initialized ||
        aml == nullptr ||
        json.empty()) {
        return false;
    }

    if (json.size() > MAX_PROFILE_SIZE)
        return false;

    char filePath[512];

    if (!BuildProfilePath(
        modelId,
        filePath,
        sizeof(filePath)
    )) {
        return false;
    }

    return aml->WriteBufferToFile(
        filePath,
        json.data(),
        json.size()
    );
}

bool LoadProfileJson(
    int modelId,
    std::string& json
) {
    json.clear();

    if (!g_initialized || aml == nullptr)
        return false;

    char filePath[512];

    if (!BuildProfilePath(
        modelId,
        filePath,
        sizeof(filePath)
    )) {
        return false;
    }

    if (!aml->FileExists(filePath))
        return false;

    const size_t fileSize = aml->FileSize(filePath);

    if (fileSize == 0 || fileSize > MAX_PROFILE_SIZE)
        return false;

    std::vector<char> buffer(fileSize + 1, '\0');

    const int bytesRead = aml->ReadFileToBuffer(
        filePath,
        buffer.data(),
        fileSize + 1
    );

    if (bytesRead <= 0 ||
        static_cast<size_t>(bytesRead) > fileSize) {
        return false;
    }

    buffer[bytesRead] = '\0';

    json.assign(
        buffer.data(),
        static_cast<size_t>(bytesRead)
    );

    return true;
}

bool LoadFontBitmap(const std::string& fontName, std::string& bitmap) {
    bitmap.clear();
    if (!g_initialized || aml == nullptr || fontName.empty() || fontName.size() > 32)
        return false;
    for (char c : fontName) {
        const bool valid = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                           (c >= '0' && c <= '9') || c == '_' || c == '-';
        if (!valid) return false;
    }
    char path[512];
    const int length = std::snprintf(path, sizeof(path), "%s/%s.vlsfont",
        g_fontsDirectory.c_str(), fontName.c_str());
    if (length <= 0 || static_cast<size_t>(length) >= sizeof(path) || !aml->FileExists(path))
        return false;
    const size_t size = aml->FileSize(path);
    if (size == 0 || size > MAX_FONT_SIZE)
        return false;
    std::vector<char> buffer(size + 1, 0);
    const int read = aml->ReadFileToBuffer(path, buffer.data(), size + 1);
    if (read <= 0 || static_cast<size_t>(read) > size)
        return false;
    bitmap.assign(buffer.data(), static_cast<size_t>(read));
    return true;
}

bool LoadTexturePpm(const std::string& textureName, std::string& ppm) {
    ppm.clear();
    if (!g_initialized || aml == nullptr || textureName.empty() || textureName.size() > 32)
        return false;
    for (char c : textureName) {
        const bool valid = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                           (c >= '0' && c <= '9') || c == '_' || c == '-';
        if (!valid) return false;
    }
    char path[512];
    const int length = std::snprintf(path, sizeof(path), "%s/%s.ppm",
        g_texturesDirectory.c_str(), textureName.c_str());
    if (length <= 0 || static_cast<size_t>(length) >= sizeof(path) || !aml->FileExists(path))
        return false;
    const size_t size = aml->FileSize(path);
    if (size == 0 || size > MAX_TEXTURE_SIZE)
        return false;
    std::vector<char> buffer(size + 1, 0);
    const int read = aml->ReadFileToBuffer(path, buffer.data(), size + 1);
    if (read <= 0 || static_cast<size_t>(read) > size)
        return false;
    ppm.assign(buffer.data(), static_cast<size_t>(read));
    return true;
}

bool LoadTexturePng(const std::string& textureName, std::string& png) {
    png.clear();
    if (!g_initialized || aml == nullptr || textureName.empty() || textureName.size() > 32)
        return false;
    for (char c : textureName) {
        const bool valid = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                           (c >= '0' && c <= '9') || c == '_' || c == '-';
        if (!valid) return false;
    }
    char path[512];
    const int length = std::snprintf(path, sizeof(path), "%s/%s.png",
        g_texturesDirectory.c_str(), textureName.c_str());
    if (length <= 0 || static_cast<size_t>(length) >= sizeof(path) || !aml->FileExists(path))
        return false;
    const size_t size = aml->FileSize(path);
    if (size == 0 || size > MAX_TEXTURE_SIZE)
        return false;
    std::vector<char> buffer(size + 1, 0);
    const int read = aml->ReadFileToBuffer(path, buffer.data(), size + 1);
    if (read <= 0 || static_cast<size_t>(read) > size)
        return false;
    png.assign(buffer.data(), static_cast<size_t>(read));
    return true;
}

bool SaveDiagnosticText(
    const std::string& fileName,
    const std::string& text
) {
    if (!g_initialized ||
        aml == nullptr ||
        text.empty() ||
        text.size() > MAX_DIAGNOSTIC_SIZE) {
        return false;
    }

    char filePath[512];

    if (!BuildDiagnosticPath(
        fileName,
        filePath,
        sizeof(filePath)
    )) {
        return false;
    }

    return aml->WriteBufferToFile(
        filePath,
        text.data(),
        text.size()
    );
}

}
}
