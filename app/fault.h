#ifndef FAULT_H
#define FAULT_H

#include <stdint.h>

// Fault injection with the B1 button, for demonstrating trial and rollback on
// the bench.
//
// Pressing B1 makes the fault task stop reporting its heartbeat while it keeps
// running, exactly like a task that is alive but stuck. The supervisor stops
// feeding the watchdog and the part resets within about five seconds. On an
// image in trial, that reset costs an attempt: pressing B1 on three trial
// boots in a row makes the bootloader reject the image and fall back.

// Creates the fault task. Called before the scheduler starts.
void fault_start(void);

// Non-zero once a fault has been injected, until the reset
uint32_t fault_injected(void);

#endif // FAULT_H
