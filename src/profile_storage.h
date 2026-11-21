
#pragma once

#include <string>

namespace VLS {
namespace ProfileStorage {

bool Init();

const char* GetProfilesDirectory();
const char* GetDiagnosticsDirectory();

bool SaveProfileJson(int modelId, const std::string& json);
bool LoadProfileJson(int modelId, std::string& json);

bool SaveDiagnosticText(
    const std::string& fileName,
    const std::string& text
);

}
}
