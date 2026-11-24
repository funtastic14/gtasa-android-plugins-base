
#include <mod/amlmod.h>
#include <mod/config.h>
#include <mod/logger.h>
#include <plugin-sdk/game_sa/Events.h>
#include <src/logic.h>
#include <src/profile_storage.h>

MYMODCFG(funtastic14.vehicleledsystem, VehicleLEDSystem, 1.0, 0xavion)

ON_MOD_LOAD() {
    logger->SetTag("Vehicle LED System");

    if (VLS::ProfileStorage::Init()) {
        logger->Info("Profile storage initialized");
    } else {
        logger->Info("Profile storage initialization failed");
    }

    Logic::Init();

    logger->Info("Vehicle LED System loaded successfully");

    Events::shutdownRwEvent += Logic::Shutdown;
}
