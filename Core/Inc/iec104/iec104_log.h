#ifndef IEC104_LOG_H
#define IEC104_LOG_H

#include "cs104_slave.h"

#include <stdbool.h>
#include <stdint.h>

/* Set to 0 at build time to omit per-response ASDU logs. */
#ifndef IEC104_LOG_TX_ASDU_ENABLED
#define IEC104_LOG_TX_ASDU_ENABLED 1
#endif

void IEC104_Log(const char *format, ...);
void IEC104_LogTxASDU(CS101_ASDU asdu, int32_t ioa, bool sent);

#endif
