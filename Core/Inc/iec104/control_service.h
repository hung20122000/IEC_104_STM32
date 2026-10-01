#ifndef IEC104_CONTROL_SERVICE_H
#define IEC104_CONTROL_SERVICE_H

#include "iec104/point_database.h"

#include <stdbool.h>
#include <stdint.h>

#define IEC104_CONTROL_QUEUE_CAPACITY 8U

typedef struct
{
  uint32_t ioa;
  ControlType type;
  PointValue value;
  uint8_t qualifier;
  uint8_t step_direction; /* IEC60870_STEP_LOWER or IEC60870_STEP_HIGHER. */
} ControlCommand;

typedef struct
{
  uint32_t transaction_id;
  bool success;
} ControlResult;

bool ControlService_Init(void);
bool ControlService_Select(const ControlCommand *command, uint32_t session_id);
bool ControlService_Execute(const ControlCommand *command, uint32_t session_id,
                            uint32_t transaction_id);
void ControlService_CancelSelection(uint32_t session_id);
bool ControlService_TakeResult(ControlResult *result);
uint32_t ControlService_StackFreeBytes(void);

#endif
