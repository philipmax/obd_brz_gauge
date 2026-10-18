#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Start RaceChrono BLE DIY GATTS service (UUID 0x1FF8) and the gauge pairing
// service (0x1FF9). Registers itself with ble_hub (GATTS app 0x52, brings the
// stack up if needed). When enable_racechrono is false, RC (0x1FF8) is skipped:
// a MASTER publishes and advertises only Pair (0x1FF9); any other role registers
// no GATTS app and stays invisible (no advertising).
// Channels are read from signal_store; a channel without a fresh valid value is
// not sent.
void racechrono_ble_diy_start(bool enable_racechrono);

#ifdef __cplusplus
}
#endif
