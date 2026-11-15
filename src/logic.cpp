
#include "logic.h"
#include <mod/config.h>

namespace Logic {

struct VLSSettings {
    bool enabled;
    bool allVehicles;

    bool runningTextEnabled;
    const char* runningText;
    float runningTextSpeed;

    bool stroboEnabled;
    int stroboSpeed;

    bool rgbEnabled;
    int rgbMode;
    float rgbSpeed;

    int animationPreset;
    bool animationLoop;
};

static VLSSettings settings;

void Initialize() {
    if (!cfg) {
        return;
    }

    settings.enabled =
        cfg->GetBool("Enabled", true, "GENERAL");

    settings.allVehicles =
        cfg->GetBool("AllVehicles", true, "GENERAL");

    settings.runningTextEnabled =
        cfg->GetBool("Enabled", false, "RUNNING_TEXT");

    settings.runningText =
        cfg->GetString("Text", "VEHICLE LED SYSTEM", "RUNNING_TEXT");

    settings.runningTextSpeed =
        cfg->GetFloat("Speed", 1.0f, "RUNNING_TEXT");

    settings.stroboEnabled =
        cfg->GetBool("Enabled", false, "STROBO");

    settings.stroboSpeed =
        cfg->GetInt("Speed", 5, "STROBO");

    settings.rgbEnabled =
        cfg->GetBool("Enabled", false, "RGB");

    settings.rgbMode =
        cfg->GetInt("Mode", 0, "RGB");

    settings.rgbSpeed =
        cfg->GetFloat("Speed", 2.0f, "RGB");

    settings.animationPreset =
        cfg->GetInt("Preset", 0, "ANIMATION");

    settings.animationLoop =
        cfg->GetBool("Loop", true, "ANIMATION");
}

void Shutdown() {
    // Cleanup akan ditambahkan saat sistem efek kendaraan dibuat.
}

}
