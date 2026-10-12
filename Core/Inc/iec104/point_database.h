#ifndef IEC104_POINT_DATABASE_H
#define IEC104_POINT_DATABASE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define IEC104_COMMON_ADDRESS       1U
#define IEC104_POINT_COUNT          6U
#define IEC104_COMMAND_COUNT        6U
#define IEC104_POINT_EVENT_CAPACITY 32U

#define POINT_IOA_SINGLE            1001U
#define POINT_IOA_DOUBLE            1002U
#define POINT_IOA_STEP              1003U
#define POINT_IOA_NORMALIZED        2001U
#define POINT_IOA_SCALED            2002U
#define POINT_IOA_FLOAT             2003U

typedef enum
{
  POINT_TYPE_SINGLE,
  POINT_TYPE_DOUBLE,
  POINT_TYPE_STEP,
  POINT_TYPE_NORMALIZED,
  POINT_TYPE_SCALED,
  POINT_TYPE_FLOAT
} PointType;

typedef enum
{
  CONTROL_TYPE_SINGLE,
  CONTROL_TYPE_DOUBLE,
  CONTROL_TYPE_STEP,
  CONTROL_TYPE_NORMALIZED,
  CONTROL_TYPE_SCALED,
  CONTROL_TYPE_FLOAT
} ControlType;

typedef struct
{
  uint32_t command_ioa;
  ControlType type;
  uint32_t monitor_ioa;
} CommandPointDefinition;

typedef union
{
  bool single;
  uint8_t double_state;
  struct
  {
    int8_t position;
    bool transient;
  } step;
  float normalized;
  int32_t scaled;
  float real;
} PointValue;

typedef struct
{
  uint32_t ioa;
  PointType type;
  PointValue value;
  uint8_t quality;
  uint32_t sample_tick_ms;
  uint64_t source_utc_ms; /* Zero when the source has no synchronized clock. */
  uint32_t version;
} PointSnapshot;

bool PointDatabase_Init(void);
size_t PointDatabase_Count(void);
bool PointDatabase_ReadByIndex(size_t index, PointSnapshot *out);
bool PointDatabase_ReadByIOA(uint32_t ioa, PointSnapshot *out);
bool PointDatabase_FindCommand(uint32_t ioa, ControlType type,
                               CommandPointDefinition *out);

/* Task context only. A future input driver can call this same API. */
bool PointDatabase_Update(uint32_t ioa, PointValue value, uint8_t quality,
                          uint64_t source_utc_ms);

/* Simulation cannot overwrite a point after control takes ownership of it. */
bool PointDatabase_UpdateSimulated(uint32_t ioa, PointValue value,
                                   uint8_t quality);

/* Control changes remain visible even when below the measurement deadband. */
bool PointDatabase_UpdateControlled(uint32_t ioa, PointValue value,
                                    uint8_t quality);
                                    
/* Apply one regulating step atomically; false at the protocol limits. */
bool PointDatabase_StepControlled(uint32_t ioa, bool higher);
bool PointDatabase_TakeEvent(PointSnapshot *out);
uint32_t PointDatabase_DroppedEvents(void);

#endif
