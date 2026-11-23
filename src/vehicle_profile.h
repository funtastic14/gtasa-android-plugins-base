
#pragma once

#include <cstdint>
#include <vector>
#include <string>

namespace VLS {

// Zona pemasangan LED
enum class LedZone : uint8_t {
    Windshield,
    ExteriorBody,
    Headlight,
    Taillight,
    Interior,
    FrontUpWindshield,
    Roof
};

// Jenis efek LED.
// Urutan enum lama WAJIB dipertahankan untuk kompatibilitas JSON.
enum class LedEffectType : uint8_t {
    RunningLED,
    RunningText,
    RGB,
    StroboWave,
    StroboEyes,
    CustomAnimation
};

// Arah gerakan masing-masing LED/piksel.
enum class LedColorMode : uint8_t {
    Solid,
    Gradient,
    Rainbow
};

// Motion is independent from color style and effect family.
enum class LedMotionPattern : uint8_t {
    LeftToRight,
    RightToLeft,
    CenterOut,
    EdgeToCenter,
    ZoomIn,
    ZoomOut,
    FadeIn,
    FadeOut,
    Rolling,
    Flow,
    Wave,
    Diagonal,
    Ripple,
    Pulse,
    AlternateRows,
    AlternateColumns,
    Checkerboard,
    Windmill,
    Spiral,
    Starburst
};

enum class MotionPlaybackMode : uint8_t {
    Manual,
    Automatic,
    Sequential,
    Random
};

enum class RunningTextMotion : uint8_t {
    ScrollLeft,
    ScrollRight,
    BounceHorizontal,
    ScrollUp,
    ScrollDown,
    BounceVertical,
    Static
};

enum class LedDirection : uint8_t {
    LeftToRight,
    RightToLeft,
    TopToBottom,
    BottomToTop,
    CustomAngle
};

// Posisi dan ukuran LED relatif terhadap kendaraan
struct LedTransform {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;

    float rotationX = 0.0f;
    float rotationY = 0.0f;
    float rotationZ = 0.0f;

    // Lengkungan permukaan LED (offset kedalaman maksimum di tengah, unit kendaraan).
    float curvature = 0.0f;

    // Koreksi arah hadap/POV setelah rotasi pemasangan.
    float facingYaw = 0.0f;
    float facingPitch = 0.0f;

    float width = 1.0f;
    float height = 1.0f;
    float stretchX = 1.0f;
    float stretchY = 1.0f;
};

// Konfigurasi satu LED/piksel matriks.
struct LedMatrixPixel {
    bool enabled = true;

    // Arah gerakan LED ini, independen dari LED lainnya.
    LedDirection direction = LedDirection::LeftToRight;

    // Digunakan jika direction = CustomAngle.
    // Satuan derajat.
    float customAngle = 0.0f;

    // Warna individual LED.
    uint8_t red = 255;
    uint8_t green = 255;
    uint8_t blue = 255;

    float brightness = 1.0f;
    float speed = 1.0f;
};

// Konfigurasi sistem LED Matrix.
struct LedMatrixConfig {
    bool enabled = false;

    // Jumlah kolom dan baris matriks.
    uint16_t columns = 12;
    uint16_t rows = 1;

    // Setiap elemen mewakili satu LED/piksel.
    // Urutan: kiri ke kanan, kemudian baris berikutnya.
    std::vector<LedMatrixPixel> pixels;
};

// Posisi dan ukuran LED relatif terhadap kendaraan
// (transformasi satu placement).
struct LedEffectConfig {
    LedEffectType type = LedEffectType::RunningLED;

    bool enabled = true;

    uint8_t red = 255;
    uint8_t green = 255;
    uint8_t blue = 255;

    float brightness = 1.0f;
    float opacity = 1.0f;
    float speed = 1.0f;
    uint32_t durationMs = 1000; // Per-step duration for this LED's Custom sequence.

    LedColorMode colorMode = LedColorMode::Solid;
    LedMotionPattern motionPattern = LedMotionPattern::LeftToRight;
    MotionPlaybackMode motionPlayback = MotionPlaybackMode::Manual;
    uint32_t motionIntervalMs = 3000;
    uint8_t secondaryRed = 0;
    uint8_t secondaryGreen = 120;
    uint8_t secondaryBlue = 255;

    // Teks untuk efek RunningText pada LED Matrix.
    std::string text;
    float textSpacing = 1.0f;
    RunningTextMotion textMotion = RunningTextMotion::ScrollLeft;
    std::string fontName = "default";

    // Konfigurasi matriks untuk efek ini.
    LedMatrixConfig matrix;
};

// Satu titik pemasangan LED.
struct LedPlacement {
    uint32_t id = 0;
    bool enabled = true;

    // Animation control is independent for every physical LED placement.
    uint8_t animationMode = 0; // AnimationMode::Manual (kept numeric to avoid header dependency).
    uint32_t animationIntervalMs = 3000;
    uint32_t activeEffectIndex = 0;

    LedZone zone = LedZone::ExteriorBody;

    LedTransform transform;

    std::vector<LedEffectConfig> effects;
};

// Satu langkah dalam Effect Sequence.
struct EffectSequenceStep {
    uint32_t placementId = 0;
    uint32_t effectIndex = 0;

    uint32_t durationMs = 1000;
};

// Rangkaian animasi.
struct EffectSequence {
    bool enabled = false;
    bool loop = true;

    std::vector<EffectSequenceStep> steps;
};

// Profil konfigurasi satu model kendaraan.
struct VehicleProfile {
    int modelId = -1;

    bool enabled = true;

    std::vector<LedPlacement> placements;

    std::vector<EffectSequence> sequences;
};

} // namespace VLS
