#ifndef IEC104_HANDLERS_H
#define IEC104_HANDLERS_H

#include "cs104_slave.h"

#include <stdbool.h>

/* All functions, including callbacks, run on IEC104Task. */
void IEC104_HandlersAttach(CS104_Slave slave);
void IEC104_HandlersDetach(void);
void IEC104_HandlersProcess(void);
bool IEC104_HandlersHasActiveMaster(void);

#endif
