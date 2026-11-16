
#pragma once

#include <cstdint>
#include <vector>

namespace VLS {

// Zona pemasangan LED
enum class LedZone : uint8_t {
    Windshield,
    ExteriorBody,
    Headlight,
    Taillight,
    Interior
};

// Jenis efek LED
enum class LedEffectType : uint8_t {
    RunningLED,
    RunningText,
    RGB,
    StroboWave,
    StroboEyes,
    CustomAnimation
};

// Posisi dan ukuran LED relatif terhadap kendaraan
struct LedTransform {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;

    float rotationX = 0.0f;
    float rotationY = 0.0f;
    float rotationZ = 0.0f;

    float width = 1.0f;
    float height = 1.0f;
};

// Konfigurasi satu efek LED
struct LedEffectConfig {
    LedEffectType type = LedEffectType::RunningLED;

    bool enabled = true;

    uint8_t red = 255;
    uint8_t green = 255;
    uint8_t blue = 255;

    float brightness = 1.0f;
    float speed = 1.0f;
};

// Satu titik pemasangan LED
struct LedPlacement {
    uint32_t id = 0;

    LedZone zone = LedZone::ExteriorBody;

    LedTransform transform;

    std::vector<LedEffectConfig> effects;
};

// Satu langkah dalam Effect Sequence
struct EffectSequenceStep {
    uint32_t placementId = 0;
    uint32_t effectIndex = 0;

    uint32_t durationMs = 1000;
};

// Rangkaian animasi
struct EffectSequence {
    bool enabled = false;
    bool loop = true;

    std::vector<EffectSequenceStep> steps;
};

// Profil konfigurasi satu model kendaraan
struct VehicleProfile {
    int modelId = -1;

    bool enabled = true;

    std::vector<LedPlacement> placements;

    std::vector<EffectSequence> sequences;
};

} // namespace VLS
