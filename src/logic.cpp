
#include "logic.h"

#include <mod/config.h>
#include <mod/logger.h>

namespace Logic {

static bool g_enabled;
static bool g_runningLED;
static bool g_strobo;
static bool g_rgbLighting;
static bool g_runningText;

static int g_brightness;
static float g_animationSpeed;

void Init() {
    logger->Info("Initializing VLS configuration...");

    g_enabled = cfg->GetBool("Enabled", true, "General");

    g_runningLED = cfg->GetBool("RunningLED", true, "Effects");
    g_strobo = cfg->GetBool("Strobo", false, "Effects");
    g_rgbLighting = cfg->GetBool("RGBLighting", true, "Effects");
    g_runningText = cfg->GetBool("RunningText", false, "Effects");

    g_brightness = cfg->GetInt("Brightness", 100, "Animation");
    g_animationSpeed = cfg->GetFloat("Speed", 1.0f, "Animation");

    if (g_brightness < 0) g_brightness = 0;
    if (g_brightness > 100) g_brightness = 100;

    if (g_animationSpeed < 0.1f) g_animationSpeed = 0.1f;
    if (g_animationSpeed > 10.0f) g_animationSpeed = 10.0f;

    logger->Info("VLS configuration initialized.");
}

void Shutdown() {
    // VLS cleanup will be implemented later.
}

}
