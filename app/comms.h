#ifndef COMMS_H
#define COMMS_H

#include "boot_info.h"
#include "flash_if.h"

// The node's side of the link to the gateway: receives framed messages over
// LPUART1 and answers them. Answers STATUS_REQ, passes the update messages to
// the update agent and acts on REBOOT.

// Configures the UART and creates the comms task. Called before the
// scheduler starts, after boot_info has been loaded.
void comms_start(const boot_info_t *info, const flash_if_t *flash);

#endif // COMMS_H
