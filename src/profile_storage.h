
#pragma once

#include <string>

namespace VLS {
namespace ProfileStorage {

bool Init();

const char* GetProfilesDirectory();

bool SaveProfileJson(int modelId, const std::string& json);
bool LoadProfileJson(int modelId, std::string& json);

}
}
