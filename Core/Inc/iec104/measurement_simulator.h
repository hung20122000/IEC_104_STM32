#ifndef IEC104_MEASUREMENT_SIMULATOR_H
#define IEC104_MEASUREMENT_SIMULATOR_H

#include <stdbool.h>
#include <stdint.h>

bool MeasurementSimulator_Init(void);
uint32_t MeasurementSimulator_StackFreeBytes(void);

#endif
