#ifndef COMMS_H
#define COMMS_H

#include "boot_info.h"

// The node's side of the link to the gateway: receives framed messages over
// LPUART1 and answers them. For now it answers STATUS_REQ with STATUS.

// Configures the UART and creates the comms task. Called before the
// scheduler starts, after boot_info has been loaded.
void comms_start(const boot_info_t *info);

#endif // COMMS_H
