
#include "pattern_registry.h"

namespace VLS {

namespace {

constexpr uint32_t ZoneMask(LedZone zone) {
    return 1u << static_cast<uint8_t>(zone);
}

constexpr uint32_t WINDSHIELD =
    ZoneMask(LedZone::Windshield);

// Tahap pertama hanya mendaftarkan pola yang sudah
// memiliki implementasi pada engine saat ini.
const PatternInfo g_patterns[] = {
    {
        LedEffectType::RunningLED,
        PatternCategory::Directional,
        "Running LED",
        "LED bergerak secara berurutan",
        WINDSHIELD
    },
    {
        LedEffectType::RGB,
        PatternCategory::Color,
        "RGB",
        "Perubahan warna RGB",
        WINDSHIELD
    },
    {
        LedEffectType::StroboWave,
        PatternCategory::Warning,
        "Strobo Wave",
        "Efek kilatan strobo bergelombang",
        WINDSHIELD
    },
    {
        LedEffectType::StroboEyes,
        PatternCategory::Special,
        "Strobo Eyes",
        "Efek animasi khusus bergaya mata",
        WINDSHIELD
    }
};

constexpr size_t PATTERN_COUNT =
    sizeof(g_patterns) / sizeof(g_patterns[0]);

} // namespace

const PatternInfo* PatternRegistry::GetPatterns(size_t& count) {
    count = PATTERN_COUNT;
    return g_patterns;
}

const PatternInfo* PatternRegistry::FindPattern(
    LedEffectType type
) {
    for (size_t i = 0; i < PATTERN_COUNT; ++i) {
        if (g_patterns[i].type == type) {
            return &g_patterns[i];
        }
    }

    return nullptr;
}

bool PatternRegistry::IsCompatible(
    LedEffectType type,
    LedZone zone
) {
    const PatternInfo* pattern = FindPattern(type);

    if (!pattern) {
        return false;
    }

    return IsCompatible(*pattern, zone);
}

bool PatternRegistry::IsCompatible(
    const PatternInfo& pattern,
    LedZone zone
) {
    const uint32_t zoneBit = ZoneMask(zone);

    return (pattern.compatibleZones & zoneBit) != 0;
}

} // namespace VLS
