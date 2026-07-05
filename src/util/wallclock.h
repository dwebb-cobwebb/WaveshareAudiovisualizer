#ifndef AV_WALLCLOCK_H
#define AV_WALLCLOCK_H

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

// Wall-clock time for the clock view. The RP2350 has no battery-backed RTC,
// so time is pushed from the host over the CDC serial port ("T<epoch>\n",
// local-time epoch seconds — see scripts/set_clock.ps1) and free-runs on the
// microsecond timebase between updates.

// Set from the host. `local_epoch_s` is seconds since 1970 in LOCAL time.
void wallclock_set(int64_t local_epoch_s);

// Fill `out` with the current local time. Returns false if never set.
bool wallclock_get(struct tm *out);

#endif // AV_WALLCLOCK_H
