
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
    float speed = 1.0f;

    // Konfigurasi matriks untuk efek ini.
    LedMatrixConfig matrix;
};

// Satu titik pemasangan LED.
struct LedPlacement {
    uint32_t id = 0;

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
