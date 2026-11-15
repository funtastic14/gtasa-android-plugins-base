
#include <mod/amlmod.h>
#include <mod/config.h>
#include <mod/logger.h>
#include <plugin-sdk/game_sa/Events.h>
#include <src/logic.h>

MYMODCFG(funtastic14.vehicleledsystem, VehicleLEDSystem, 1.0, 0xavion)

ON_MOD_LOAD() {
    logger->SetTag("Vehicle LED System");
    logger->Info("Vehicle LED System loading...");

    Events::shutdownRwEvent += Logic::Shutdown;

    logger->Info("Vehicle LED System loaded successfully");
}
