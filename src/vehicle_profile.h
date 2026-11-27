
#pragma once

#include <cstdint>
#include "effect_transition_engine.h"
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

enum class LedLayerBlendMode : uint8_t {
    Alpha,
    Additive
};

// Dedicated Strobo Eyes controls. These are intentionally independent
// from the general-purpose LED motion pattern engine.
enum class EyeShape : uint8_t {
    Original,
    Round,
    Sleepy
};

// Gaze motion is independent from the eye silhouette and blink timing.
// Expression presets and decorative iris motifs are intentionally excluded.
enum class EyeMotion : uint8_t {
    Idle,
    LookLeft,
    LookRight,
    LookUp,
    LookDown,
    LookUpLeft,
    LookUpRight,
    LookDownLeft,
    LookDownRight,
    Scan,
    Shake,
    Wink
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
    OpposingArrows,
    CenterCollision,
    CenterBurst,
    DiagonalCollision,
    DiagonalCenterBurst,
    MultiCollision,
    CrossBurst,
    RadialBurst,
    PingPong,
    MirrorReflection,
    ReflectiveWave,
    CollisionRebound,
    CenterSplit,
    DualFlow,
    MirroredDiagonal,
    DoubleRebound,
    VerticalSweep,
    SideCollision,
    CornerBurst,
    TwinSpiral,
    OrbitRing,
    MeteorShower,
    CometChase,
    KnightScanner,
    Heartbeat,
    AuroraFlow,
    Sparkle,
    FlameFlicker,
    Waterfall,
    Vortex,
    PoliceFlash,
    RotatingSquare,
    CurvedSweep,
    SquareExpand,
    SquareCollapse,
    CircleExpand,
    CircleCollapse,
    TriangleConverge,
    TriangleOrbit,
    MultiLineFlutter,
    TriangleFanSweep,
    DiagonalXCross,
    MultiAxisCenterBurst,
    TripleMirrorWave,
    SegmentedCenterExpand,
    AccumulateLeftToRight,
    AccumulateRightToLeft,
    AccumulateCenterOut,
    AccumulateEdgesToCenter,
    SideAnchoredCenterSlider,
    CenterAnchoredSideSlider
};

enum class MotionPlaybackMode : uint8_t {
    Manual,
    Automatic,
    Sequential,
    Random
};

// Texture-set playback is independent from gaze/blink motion playback.
enum class EyeTexturePlaybackMode : uint8_t {
    Manual,
    Automatic,
    Sequence,
    Random
};

struct EyeTextureDesign {
    std::string name;
    std::string textureName;
    bool mappingConfigured = false;
    float irisCenterX = 0.50f;
    float irisCenterY = 0.50f;
    float irisRadiusX = 0.25f;
    float irisRadiusY = 0.30f;
    float pupilCenterX = 0.50f;
    float pupilCenterY = 0.50f;
    float pupilRadiusX = 0.10f;
    float pupilRadiusY = 0.14f;
};

// Immutable per-frame snapshot shared by strip and matrix renderers.
// Pattern selection and base animation phase are resolved once, not per pixel.
struct MotionFrameState {
    uint32_t timeMs = 0;
    uint32_t intervalMs = 3000;
    uint32_t cycleIndex = 0;
    float phase = 0.0f;
    LedMotionPattern pattern = LedMotionPattern::LeftToRight;
    LedMotionPattern previousPattern = LedMotionPattern::LeftToRight;
    float transitionProgress = 1.0f;
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

enum class LedElementShape : uint8_t {
    Square = 0,
    CircleFilled = 1,
    CircleOutline = 2,
    Diamond = 3,
    Triangle = 4,
    Hexagon = 5,
    Octagon = 6,
    Inherit = 255
};

// Konfigurasi satu LED/piksel matriks.
struct LedMatrixPixel {
    bool enabled = true;

    // Per-pixel shape; Inherit uses the effect-level shape for compatibility.
    LedElementShape elementShape = LedElementShape::Inherit;

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

    // Optional shared logical canvas. Multiple physical matrix regions can
    // address one continuous coordinate space by using the same canvas size
    // and different offsets. Disabled by default for backward compatibility.
    bool canvasEnabled = false;
    uint16_t canvasColumns = 12;
    uint16_t canvasRows = 1;
    uint16_t canvasOffsetColumn = 0;
    uint16_t canvasOffsetRow = 0;

    // Setiap elemen mewakili satu LED/piksel.
    // Urutan: kiri ke kanan, kemudian baris berikutnya.
    std::vector<LedMatrixPixel> pixels;
};

// Posisi dan ukuran LED relatif terhadap kendaraan
// (transformasi satu placement).
struct LedEffectConfig {
    LedEffectType type = LedEffectType::RunningLED;

    bool enabled = true;

    // Multi-layer composition: lower order draws first; higher order draws on top.
    uint8_t layerOrder = 0;
    LedLayerBlendMode blendMode = LedLayerBlendMode::Alpha;
    // Physical shape of each LED/pixel; panel bounds remain rectangular.
    LedElementShape elementShape = LedElementShape::Square;

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
    uint8_t tertiaryRed = 0;
    uint8_t tertiaryGreen = 80;
    uint8_t tertiaryBlue = 255;
    LedColorDirection colorDirection = LedColorDirection::LeftToRight;
    float colorSpread = 1.0f;

    // Strobo Eyes: three silhouettes, independent gaze motion and palettes.
    EyeShape eyeShape = EyeShape::Original;
    EyeMotion eyeMotion = EyeMotion::Idle;
    MotionPlaybackMode eyeMotionPlayback = MotionPlaybackMode::Manual;
    uint32_t eyeMotionIntervalMs = 3000;
    uint32_t eyeBlinkIntervalMs = 3200;

    // Multiple authored eye artworks. This controller changes designs only;
    // gaze, blink and other eye motion remain on their own timeline.
    std::vector<EyeTextureDesign> eyeTextures;
    EyeTexturePlaybackMode eyeTexturePlayback = EyeTexturePlaybackMode::Manual;
    uint8_t eyeTextureIndex = 0;
    uint32_t eyeTextureIntervalMs = 5000;
    std::vector<uint8_t> eyeTextureSequence;
    uint8_t eyeIrisRed = 20;
    uint8_t eyeIrisGreen = 155;
    uint8_t eyeIrisBlue = 255;
    uint8_t eyePupilRed = 5;
    uint8_t eyePupilGreen = 8;
    uint8_t eyePupilBlue = 18;

    // Normalized per-eye UV regions used to animate a flattened custom PNG.
    // These values are profile metadata, not additional animation images.
    float eyeTextureIrisCenterX = 0.50f;
    float eyeTextureIrisCenterY = 0.50f;
    float eyeTextureIrisRadiusX = 0.25f;
    float eyeTextureIrisRadiusY = 0.30f;
    float eyeTexturePupilCenterX = 0.50f;
    float eyeTexturePupilCenterY = 0.50f;
    float eyeTexturePupilRadiusX = 0.10f;
    float eyeTexturePupilRadiusY = 0.14f;

    // Dedicated playback controls; these do not affect general LED motion.

    // Teks untuk efek RunningText pada LED Matrix.
    std::string text;
    float textSpacing = 1.0f;
    float textScale = 1.0f;
    RunningTextMotion textMotion = RunningTextMotion::ScrollLeft;
    std::string fontName = "default";
    std::string textureName;

    // Konfigurasi matriks untuk efek ini.
    LedMatrixConfig matrix;
    // Skip dim/off matrix cells so the vehicle surface remains visible behind them.
    bool transparentMatrixBackground = false;
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
    EffectTransitionMode transitionMode = EffectTransitionMode::Crossfade;
    EffectTransitionDirection transitionDirection = EffectTransitionDirection::LeftToRight;

    std::vector<EffectSequenceStep> steps;
};

enum class NativeLightScanMode : uint8_t {
    ModelId = 0,
    TextureName = 1
};

// Profil konfigurasi satu model kendaraan.
struct NativeVehicleLightConfig {
    NativeLightScanMode scanMode = NativeLightScanMode::ModelId;
    bool highBeamEnabled = false;
    float highBeamRange = 500.0f;
    float highBeamIntensity = 1.25f;
    bool rgbHeadlightsEnabled = false;
    bool rgbTaillightsEnabled = false;
    float rgbFlowSpeed = 1.0f;
    float rgbFlowIntensity = 1.0f;
    // Native RGB is the base lighting layer; placement layers render above it.
    LedLayerBlendMode blendMode = LedLayerBlendMode::Alpha;
};

struct VehicleProfile {
    int modelId = -1;

    bool enabled = true;
    NativeVehicleLightConfig nativeLights;

    std::vector<LedPlacement> placements;

    std::vector<EffectSequence> sequences;
};

} // namespace VLS
