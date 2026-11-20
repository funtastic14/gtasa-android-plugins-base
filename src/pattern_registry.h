
#pragma once

#include <cstddef>
#include <cstdint>

#include "vehicle_profile.h"

namespace VLS {

enum class PatternCategory : uint8_t {
    Directional,
    Color,
    Warning,
    Special,
    Matrix,
    Transition
};

struct PatternInfo {
    LedEffectType type;
    PatternCategory category;
    const char* name;
    const char* description;
    uint32_t compatibleZones;
};

class PatternRegistry {
public:
    static const PatternInfo* GetPatterns(size_t& count);

    static const PatternInfo* FindPattern(
        LedEffectType type
    );

    static bool IsCompatible(
        LedEffectType type,
        LedZone zone
    );

    static bool IsCompatible(
        const PatternInfo& pattern,
        LedZone zone
    );
};

} // namespace VLS
