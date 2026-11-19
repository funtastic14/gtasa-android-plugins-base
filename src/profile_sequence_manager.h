#pragma once

#include "vehicle_profile.h"

#include <cstdint>

namespace VLS {

class ProfileSequenceRuntime {
public:
    // Memulai sequence yang aktif pada profil kendaraan.
    bool Start(
        const VehicleProfile& profile,
        uint32_t sequenceIndex,
        uint32_t nowMs
    );

    // Menghentikan sequence secara manual.
    void Stop();

    // Memperbarui langkah aktif berdasarkan waktu dalam milidetik.
    // Mengembalikan true jika sequence masih berjalan.
    bool Update(
        const VehicleProfile& profile,
        uint32_t nowMs
    );

    // Mengambil langkah yang sedang aktif.
    const EffectSequenceStep* GetCurrentStep(
        const VehicleProfile& profile
    ) const;

    bool IsRunning() const;
    bool IsFinished() const;

    uint32_t GetSequenceIndex() const;
    uint32_t GetStepIndex() const;

private:
    bool m_running = false;
    bool m_finished = false;

    uint32_t m_sequenceIndex = 0;
    uint32_t m_stepIndex = 0;
    uint32_t m_stepStartTimeMs = 0;
};

} // namespace VLS
