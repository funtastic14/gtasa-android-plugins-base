
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
    Rainbow,
    ThreeColorGradient,
    ColorCycle,
    ColorWave,
    ColorPulse,
    Alternating
};

enum class LedColorDirection : uint8_t {
    LeftToRight,
    RightToLeft,
    TopToBottom,
    BottomToTop,
    DiagonalDown,
    DiagonalUp,
    CenterOut,
    EdgeToCenter
};

// Dedicated Strobo Eyes controls. These are intentionally independent
// from the general-purpose LED motion pattern engine.
enum class EyeShape : uint8_t {
    Classic,
    Narrow,
    Wide,
    Round,
    Cat,
    Square
};

enum class EyeMotion : uint8_t {
    Idle,
    LookLeft,
    LookRight,
    LookUp,
    LookDown,
    Scan,
    Wink,
    Shake,
    RapidBlink
};

enum class EyeExpression : uint8_t {
    Neutral,
    Happy,
    Angry,
    Sad,
    Surprised,
    Sleepy,
    Love,
    Evil
};

enum class EyeVisualStyle : uint8_t {
    Normal,
    Sharingan,
    Mangekyou,
    Rinnegan
};

enum class EyeBrowStyle : uint8_t {
    None,
    Natural,
    Sharp,
    Thick
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
    Starburst,
    LaserWrite,
    LaserWeld,
    ArrowLeft,
    ArrowRight,
    ArrowUp,
    ArrowDown,
    OpposingArrows
};

enum class LedMotionTransition : uint8_t {
    None,
    CenterCollision,
    CenterBurst
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
    LedMotionTransition motionTransition = LedMotionTransition::None;
    MotionPlaybackMode motionPlayback = MotionPlaybackMode::Manual;
    uint32_t motionIntervalMs = 3000;
    uint8_t secondaryRed = 0;
    uint8_t secondaryGreen = 120;
    uint8_t secondaryBlue = 255;
    uint8_t tertiaryRed = 0;
    uint8_t tertiaryGreen = 80;
    uint8_t tertiaryBlue = 255;
    LedColorDirection colorDirection = LedColorDirection::LeftToRight;
    float colorSpread = 1.0f;

    // Dedicated eye geometry and expression settings.
    EyeShape eyeShape = EyeShape::Classic;
    EyeMotion eyeMotion = EyeMotion::Idle;
    EyeExpression eyeExpression = EyeExpression::Neutral;
    uint32_t eyeBlinkIntervalMs = 3200;

    // Dedicated playback controls; these do not affect general LED motion.
    MotionPlaybackMode eyeMotionPlayback = MotionPlaybackMode::Manual;
    MotionPlaybackMode eyeExpressionPlayback = MotionPlaybackMode::Manual;
    uint32_t eyeMotionIntervalMs = 3000;
    uint32_t eyeExpressionIntervalMs = 3500;

    // Independent iris and pupil palettes.
    uint8_t eyeIrisRed = 20;
    uint8_t eyeIrisGreen = 155;
    uint8_t eyeIrisBlue = 255;
    uint8_t eyePupilRed = 5;
    uint8_t eyePupilGreen = 8;
    uint8_t eyePupilBlue = 18;

    // Procedural eye motifs and optional timed transformation cycle.
    EyeVisualStyle eyeVisualStyle = EyeVisualStyle::Normal;
    bool eyeTransformationEnabled = false;
    uint32_t eyeTransformationIntervalMs = 9000;

    // Eyebrow geometry and palette.
    EyeBrowStyle eyeBrowStyle = EyeBrowStyle::None;
    uint8_t eyeBrowRed = 255;
    uint8_t eyeBrowGreen = 255;
    uint8_t eyeBrowBlue = 255;
    float eyeBrowTilt = 0.0f;

    // Teks untuk efek RunningText pada LED Matrix.
    std::string text;
    float textSpacing = 1.0f;
    float textScale = 1.0f;
    RunningTextMotion textMotion = RunningTextMotion::ScrollLeft;
    std::string fontName = "default";
    std::string textureName;

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
