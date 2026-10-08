#ifndef STATUS_H
#define STATUS_H

#include "boot_info.h"

// Shows the device's state on the RGB LED and LD2, without a debugger
//
//   RGB LED, the system state:
//     green          slot A, confirmed
//     blue           slot B, confirmed
//     blinking       that slot is on trial, not yet confirmed
//     red flash      every 2 s: the newest boot record reports a rejection
//     white          a fault was injected with B1. A watchdog reset follows
//
//   LD2, the firmware's heartbeat: toggles every 500 ms while this task runs,
//   every 125 ms if the last reset was the watchdog

// Creates the status task. Called before the scheduler starts, after
// boot_info has been loaded.
void status_start(const boot_info_t *info);

#endif // STATUS_H
