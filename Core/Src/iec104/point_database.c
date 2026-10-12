#include "iec104/point_database.h"

#include "FreeRTOS.h"
#include "cmsis_os2.h"
#include "queue.h"
#include "semphr.h"
#include "stm32h7xx_hal.h"

#include <string.h>

typedef struct
{
  PointSnapshot current;
  PointValue last_event_value;
  uint8_t last_event_quality;
  float deadband;
  bool simulator_suppressed;
} PointRecord;

#define POINT_INIT(address, point_type, initial_value, threshold) \
  { .current = { .ioa = (address), .type = (point_type), .value = initial_value, \
                 .quality = 0U }, .last_event_value = initial_value, \
    .last_event_quality = 0U, .deadband = (threshold) }

static PointRecord points[IEC104_POINT_COUNT] =
{
  POINT_INIT(POINT_IOA_SINGLE, POINT_TYPE_SINGLE,       { .single = false }, 0.0f),
  POINT_INIT(POINT_IOA_DOUBLE, POINT_TYPE_DOUBLE,       { .double_state = 1U }, 0.0f),
  POINT_INIT(POINT_IOA_STEP, POINT_TYPE_STEP,           { .step = { .position = 0 } }, 0.0f),
  POINT_INIT(POINT_IOA_NORMALIZED, POINT_TYPE_NORMALIZED, { .normalized = 0.0f }, 0.05f),
  POINT_INIT(POINT_IOA_SCALED, POINT_TYPE_SCALED,       { .scaled = 0 }, 1.0f),
  POINT_INIT(POINT_IOA_FLOAT, POINT_TYPE_FLOAT,         { .real = 20.0f }, 0.2f)
};

/* Command metadata is separate from the monitor state enumerated by GI. */
static const CommandPointDefinition command_points[IEC104_COMMAND_COUNT] =
{
  { 5001U, CONTROL_TYPE_SINGLE,     POINT_IOA_SINGLE },
  { 5002U, CONTROL_TYPE_DOUBLE,     POINT_IOA_DOUBLE },
  { 5006U, CONTROL_TYPE_STEP,       POINT_IOA_STEP },
  { 5003U, CONTROL_TYPE_NORMALIZED, POINT_IOA_NORMALIZED },
  { 5004U, CONTROL_TYPE_SCALED,     POINT_IOA_SCALED },
  { 5005U, CONTROL_TYPE_FLOAT,      POINT_IOA_FLOAT }
};

static StaticSemaphore_t database_mutex_storage;
static StaticQueue_t event_queue_control;
static uint8_t event_queue_storage[IEC104_POINT_EVENT_CAPACITY * sizeof(PointSnapshot)];
static osMutexId_t database_mutex;
static osMessageQueueId_t event_queue;
static volatile uint32_t dropped_events;

static PointRecord *FindPoint(uint32_t ioa)
{
  size_t index;

  for (index = 0U; index < IEC104_POINT_COUNT; index++)
  {
    if (points[index].current.ioa == ioa)
      return &points[index];
  }

  return NULL;
}

static bool ValueCrossedDeadband(const PointRecord *record, PointValue value,
                                 bool ignore_deadband)
{
  float difference;

  switch (record->current.type)
  {
    case POINT_TYPE_SINGLE:

      return value.single != record->last_event_value.single;

    case POINT_TYPE_DOUBLE:

      return value.double_state != record->last_event_value.double_state;

    case POINT_TYPE_STEP:

      return (value.step.position != record->last_event_value.step.position) ||
             (value.step.transient != record->last_event_value.step.transient);

    case POINT_TYPE_NORMALIZED:

      difference = value.normalized - record->last_event_value.normalized;
      break;

    case POINT_TYPE_SCALED:

      difference = (float)(value.scaled - record->last_event_value.scaled);
      break;

    case POINT_TYPE_FLOAT:

      difference = value.real - record->last_event_value.real;
      break;

    default:
      return false;
  }

  if (difference < 0.0f)
    difference = -difference;

  return difference > 0.0f &&
         (ignore_deadband || (difference >= record->deadband));
}

bool PointDatabase_Init(void)
{
  const osMutexAttr_t mutex_attributes =
  {
    .name = "pointDb",
    .cb_mem = &database_mutex_storage,
    .cb_size = sizeof(database_mutex_storage)
  };

  const osMessageQueueAttr_t queue_attributes =
  {
    .name = "pointEvents",
    .cb_mem = &event_queue_control,
    .cb_size = sizeof(event_queue_control),
    .mq_mem = event_queue_storage,
    .mq_size = sizeof(event_queue_storage)
  };

  size_t index;

  database_mutex = osMutexNew(&mutex_attributes);
  event_queue = osMessageQueueNew(IEC104_POINT_EVENT_CAPACITY,
                                  sizeof(PointSnapshot), &queue_attributes);

  if ((database_mutex == NULL) || (event_queue == NULL))
    return false;

  for (index = 0U; index < IEC104_POINT_COUNT; index++)
    points[index].current.sample_tick_ms = HAL_GetTick();

  return true;
}

size_t PointDatabase_Count(void)
{
  return IEC104_POINT_COUNT;
}

bool PointDatabase_ReadByIndex(size_t index, PointSnapshot *out)
{
  if ((out == NULL) || (index >= IEC104_POINT_COUNT) ||
      (osMutexAcquire(database_mutex, osWaitForever) != osOK))

    return false;

  *out = points[index].current;
  (void)osMutexRelease(database_mutex);

  return true;
}

bool PointDatabase_ReadByIOA(uint32_t ioa, PointSnapshot *out)
{
  PointRecord *record;

  if ((out == NULL) || (osMutexAcquire(database_mutex, osWaitForever) != osOK))
    return false;

  record = FindPoint(ioa);
  if (record != NULL)
    *out = record->current;

  (void)osMutexRelease(database_mutex);

  return record != NULL;
}

bool PointDatabase_FindCommand(uint32_t ioa, ControlType type,
                               CommandPointDefinition *out)
{
  size_t index;

  if (out == NULL)
    return false;

  for (index = 0U; index < IEC104_COMMAND_COUNT; index++)
  {
    if ((command_points[index].command_ioa == ioa) &&
        (command_points[index].type == type))
    {
      *out = command_points[index];
      return true;
    }
  }

  return false;
}

/* Caller holds database_mutex. */
static void ApplyUpdateLocked(PointRecord *record, PointValue value,
                              uint8_t quality, uint64_t source_utc_ms,
                              bool controlled)
{
  bool publish;

  if (controlled)
    record->simulator_suppressed = true;

  publish = (quality != record->last_event_quality) ||
            ValueCrossedDeadband(record, value, controlled);

  record->current.value = value;
  record->current.quality = quality;
  record->current.sample_tick_ms = HAL_GetTick();
  record->current.source_utc_ms = source_utc_ms;
  record->current.version++;

  if (publish)
  {
    if (osMessageQueuePut(event_queue, &record->current, 0U, 0U) == osOK)
    {
      record->last_event_value = value;
      record->last_event_quality = quality;
    }
    else
    {
      dropped_events++;
    }
  }
}

static bool UpdatePoint(uint32_t ioa, PointValue value, uint8_t quality,
                        uint64_t source_utc_ms, bool simulated,
                        bool controlled)
{
  PointRecord *record;

  if (osMutexAcquire(database_mutex, osWaitForever) != osOK)
    return false;

  record = FindPoint(ioa);

  if ((record == NULL) || (simulated && record->simulator_suppressed))
  {
    (void)osMutexRelease(database_mutex);

    return false;
  }

  ApplyUpdateLocked(record, value, quality, source_utc_ms, controlled);

  (void)osMutexRelease(database_mutex);

  return true;
}

bool PointDatabase_Update(uint32_t ioa, PointValue value, uint8_t quality,
                          uint64_t source_utc_ms)
{
  return UpdatePoint(ioa, value, quality, source_utc_ms, false, false);
}

bool PointDatabase_UpdateSimulated(uint32_t ioa, PointValue value,
                                   uint8_t quality)
{
  return UpdatePoint(ioa, value, quality, 0U, true, false);
}

bool PointDatabase_UpdateControlled(uint32_t ioa, PointValue value,
                                    uint8_t quality)
{
  return UpdatePoint(ioa, value, quality, 0U, false, true);
}

bool PointDatabase_StepControlled(uint32_t ioa, bool higher)
{
  PointRecord *record;
  PointValue value;
  int position;

  if (osMutexAcquire(database_mutex, osWaitForever) != osOK)
    return false;

  record = FindPoint(ioa);

  if ((record == NULL) || (record->current.type != POINT_TYPE_STEP))
  {
    (void)osMutexRelease(database_mutex);

    return false;
  }

  position = record->current.value.step.position;

  if ((position < -64) || (position > 63) ||
      (higher && (position == 63)) || (!higher && (position == -64)))
  {
    (void)osMutexRelease(database_mutex);

    return false;
  }

  value = record->current.value;
  value.step.position = (int8_t)(position + (higher ? 1 : -1));
  value.step.transient = false;
  ApplyUpdateLocked(record, value, record->current.quality, 0U, true);

  (void)osMutexRelease(database_mutex);
  
  return true;
}

bool PointDatabase_TakeEvent(PointSnapshot *out)
{
  return (out != NULL) &&
         (osMessageQueueGet(event_queue, out, NULL, 0U) == osOK);
}

uint32_t PointDatabase_DroppedEvents(void)
{
  return dropped_events;
}
