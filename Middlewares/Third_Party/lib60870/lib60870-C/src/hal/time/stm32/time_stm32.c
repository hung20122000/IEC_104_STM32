/*
 *  time_stm32.c
 *
 *  STM32 time HAL implementation for lib60870.
 */

#include "hal_time.h"

#include "stm32h7xx_hal.h"

#include <stdint.h>

#define NANOSECONDS_PER_MILLISECOND 1000000ULL
#define STM32_TICK_RANGE            (1ULL << 32)

static uint32_t lastTick;
static uint64_t elapsedTickRanges;
static int64_t epochOffsetInMs;
static bool epochTimeIsSet;

/*
 * HAL_GetTick returns a 32-bit millisecond counter. lib60870 uses a 64-bit
 * monotonic clock for the IEC-104 timers, so extend the counter across wraps.
 * The threadless configuration calls this HAL from the IEC-104 task only.
 */
static uint64_t
getExtendedTickInMs(void)
{
    uint32_t currentTick = HAL_GetTick();

    if (currentTick < lastTick)
        elapsedTickRanges += STM32_TICK_RANGE;

    lastTick = currentTick;

    return elapsedTickRanges + (uint64_t)currentTick;
}

msSinceEpoch
Hal_getMonotonicTimeInMs(void)
{
    return getExtendedTickInMs();
}

nsSinceEpoch
Hal_getMonotonicTimeInNs(void)
{
    return Hal_getMonotonicTimeInMs() * NANOSECONDS_PER_MILLISECOND;
}

msSinceEpoch
Hal_getTimeInMs(void)
{
    uint64_t monotonicTime = getExtendedTickInMs();

    if (!epochTimeIsSet)
        return monotonicTime;

    if (epochOffsetInMs >= 0)
        return monotonicTime + (uint64_t)epochOffsetInMs;

    uint64_t negativeOffset = (uint64_t)(-epochOffsetInMs);

    if (negativeOffset > monotonicTime)
        return 0;

    return monotonicTime - negativeOffset;
}

nsSinceEpoch
Hal_getTimeInNs(void)
{
    return Hal_getTimeInMs() * NANOSECONDS_PER_MILLISECOND;
}

bool
Hal_setTimeInNs(nsSinceEpoch nsTime)
{
    uint64_t requestedTimeInMs = nsTime / NANOSECONDS_PER_MILLISECOND;
    uint64_t monotonicTime = getExtendedTickInMs();
    uint64_t difference;

    if (requestedTimeInMs >= monotonicTime)
    {
        difference = requestedTimeInMs - monotonicTime;

        if (difference > (uint64_t)INT64_MAX)
            return false;

        epochOffsetInMs = (int64_t)difference;
    }
    else
    {
        difference = monotonicTime - requestedTimeInMs;

        if (difference > (uint64_t)INT64_MAX)
            return false;

        epochOffsetInMs = -(int64_t)difference;
    }

    epochTimeIsSet = true;

    return true;
}
