#ifndef IEC104_APP_H
#define IEC104_APP_H

#include <stdbool.h>
#include <stdint.h>

bool IEC104_AppInit(void);
uint32_t IEC104_AppStackFreeBytes(void);
bool IEC104_AppIsListening(void);

#endif
