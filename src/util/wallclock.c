#include "util/wallclock.h"
#include "pico/time.h"

// Host-set local time as (epoch seconds, microsecond timestamp at set).
// The epoch is stored as local time and rendered with gmtime so no timezone
// math happens on the device — the host already applied its zone/DST.
static volatile int64_t  s_epoch_s = -1;
static volatile uint64_t s_base_us;

void wallclock_set(int64_t local_epoch_s) {
    s_base_us = time_us_64();
    s_epoch_s = local_epoch_s;
}

bool wallclock_get(struct tm *out) {
    int64_t epoch = s_epoch_s;
    if (epoch < 0) return false;
    time_t t = (time_t)(epoch + (int64_t)((time_us_64() - s_base_us) / 1000000ull));
    gmtime_r(&t, out);
    return true;
}
