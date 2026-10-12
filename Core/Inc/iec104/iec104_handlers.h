#ifndef IEC104_HANDLERS_H
#define IEC104_HANDLERS_H

#include "cs104_slave.h"

#include <stdbool.h>
#include <stdint.h>

/* All functions, including callbacks, run on IEC104Task. */
void IEC104_HandlersAttach(CS104_Slave slave);
void IEC104_HandlersDetach(void);
void IEC104_HandlersProcess(void);
bool IEC104_HandlersHasActiveMaster(void);
/* Call from IEC104Task only. False before successful ClockSync. */
bool IEC104_HandlersGetUtcTime(uint64_t *utc_ms);

#endif
