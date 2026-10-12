#ifndef IEC104_LOG_H
#define IEC104_LOG_H

#include "cs104_slave.h"

#include <stdbool.h>
#include <stdint.h>

/* Diagnostic frame logging can increase IEC104Task processing latency. */
#ifndef IEC104_LOG_FRAMES_ENABLED
#define IEC104_LOG_FRAMES_ENABLED 1
#endif

#ifndef IEC104_LOG_FRAME_HEX_ENABLED
#define IEC104_LOG_FRAME_HEX_ENABLED 1
#endif

/* These functions run on IEC104Task, not from an ISR or another task. */
void IEC104_Log(const char *format, ...);
void IEC104_LogRawMessage(void *parameter, IMasterConnection connection,
                         uint8_t *message, int message_size, bool sent);

#endif
