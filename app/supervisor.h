#ifndef SUPERVISOR_H
#define SUPERVISOR_H

#include <stdint.h>

// Watchdog supervision
//
// The independent watchdog is fed only by the supervisor task, and only when
// every registered task has reported a heartbeat since the last check. A task
// that stops running, whether deadlocked, starved or stuck in a loop, stops
// beating, the watchdog is no longer fed, and the part resets. On a trial
// image that reset counts as a failed attempt.
//
// The watchdog is never fed from the idle hook: idle runs whenever nothing
// else is ready, including when a task is blocked forever on something that
// will never happen.

#define SUPERVISOR_MAX_TASKS 8u

typedef uint32_t supervisor_id_t;

// Called before the scheduler starts, once per task that must keep running
supervisor_id_t supervisor_register(void);

// Called by a registered task at least once per supervisor period
void supervisor_beat(supervisor_id_t id);

// Creates the supervisor task. Called before the scheduler starts
void supervisor_start(void);

#endif // SUPERVISOR_H
