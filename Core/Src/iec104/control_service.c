#include "iec104/control_service.h"

#include "FreeRTOS.h"
#include "cmsis_os2.h"
#include "queue.h"
#include "task.h"
#include "stm32h7xx_hal.h"
#include "cs101_information_objects.h"

#include <math.h>

#define CONTROL_STACK_BYTES 2048U
#define SELECT_TIMEOUT_MS   10000U

typedef struct
{
  ControlCommand command;
  uint32_t transaction_id;
} ControlRequest;

static StaticTask_t control_task_control;
static StackType_t control_task_stack[CONTROL_STACK_BYTES / sizeof(StackType_t)];
static StaticQueue_t request_queue_control;
static StaticQueue_t result_queue_control;
static uint8_t request_queue_storage[IEC104_CONTROL_QUEUE_CAPACITY * sizeof(ControlRequest)];
static uint8_t result_queue_storage[IEC104_CONTROL_QUEUE_CAPACITY * sizeof(ControlResult)];
static osMessageQueueId_t request_queue;
static osMessageQueueId_t result_queue;
static volatile uint32_t control_stack_free_bytes;

/* Selection is only read and written by IEC104Task, including its callbacks. */
static bool has_selection;
static ControlCommand selection;
static uint32_t selection_session;
static uint32_t selection_tick;

static bool CommandIsValid(const ControlCommand *command)
{
  CommandPointDefinition definition;

  if ((command == NULL) ||
      !PointDatabase_FindCommand(command->ioa, command->type, &definition))
    return false;

  switch (command->type)
  {
    case CONTROL_TYPE_SINGLE:
      return true;
    case CONTROL_TYPE_DOUBLE:
      return (command->value.double_state == 1U) ||
             (command->value.double_state == 2U);
    case CONTROL_TYPE_STEP:
      return (command->qualifier == 0U) &&
             ((command->step_direction == IEC60870_STEP_LOWER) ||
              (command->step_direction == IEC60870_STEP_HIGHER));
    case CONTROL_TYPE_NORMALIZED:
      return isfinite(command->value.normalized) &&
             (command->value.normalized >= -1.0f) &&
             (command->value.normalized <= 1.0f);
    case CONTROL_TYPE_SCALED:
      return (command->value.scaled >= -32768) &&
             (command->value.scaled <= 32767);
    case CONTROL_TYPE_FLOAT:
      return isfinite(command->value.real) &&
             (command->value.real >= -10000.0f) &&
             (command->value.real <= 10000.0f);
    default:
      return false;
  }
}

static bool SameCommand(const ControlCommand *left, const ControlCommand *right)
{
  if ((left->ioa != right->ioa) || (left->type != right->type) ||
      (left->qualifier != right->qualifier))
    return false;

  switch (left->type)
  {
    case CONTROL_TYPE_SINGLE:
      return left->value.single == right->value.single;
    case CONTROL_TYPE_DOUBLE:
      return left->value.double_state == right->value.double_state;
    case CONTROL_TYPE_STEP:
      return left->step_direction == right->step_direction;
    case CONTROL_TYPE_NORMALIZED:
      return left->value.normalized == right->value.normalized;
    case CONTROL_TYPE_SCALED:
      return left->value.scaled == right->value.scaled;
    case CONTROL_TYPE_FLOAT:
      return left->value.real == right->value.real;
    default:
      return false;
  }
}

static void ControlService_Task(void *argument)
{
  ControlRequest request;
  ControlResult result;
  CommandPointDefinition definition;

  (void)argument;

  for (;;)
  {
    if (osMessageQueueGet(request_queue, &request, NULL, osWaitForever) != osOK)
      continue;

    result.transaction_id = request.transaction_id;
    if (!PointDatabase_FindCommand(request.command.ioa,
                                   request.command.type, &definition))
      result.success = false;
    else if (request.command.type == CONTROL_TYPE_STEP)
      result.success = PointDatabase_StepControlled(
        definition.monitor_ioa,
        request.command.step_direction == IEC60870_STEP_HIGHER);
    else
      result.success = PointDatabase_UpdateControlled(definition.monitor_ioa,
                                                      request.command.value, 0U);

    /* Never discard an execution result. IEC104Task drains this queue first. */
    (void)osMessageQueuePut(result_queue, &result, 0U, osWaitForever);
    control_stack_free_bytes =
      uxTaskGetStackHighWaterMark(NULL) * sizeof(StackType_t);
  }
}

bool ControlService_Init(void)
{
  const osMessageQueueAttr_t request_attributes =
  {
    .name = "controlReq",
    .cb_mem = &request_queue_control,
    .cb_size = sizeof(request_queue_control),
    .mq_mem = request_queue_storage,
    .mq_size = sizeof(request_queue_storage)
  };
  const osMessageQueueAttr_t result_attributes =
  {
    .name = "controlRes",
    .cb_mem = &result_queue_control,
    .cb_size = sizeof(result_queue_control),
    .mq_mem = result_queue_storage,
    .mq_size = sizeof(result_queue_storage)
  };
  const osThreadAttr_t thread_attributes =
  {
    .name = "controlSim",
    .stack_mem = control_task_stack,
    .stack_size = sizeof(control_task_stack),
    .cb_mem = &control_task_control,
    .cb_size = sizeof(control_task_control),
    .priority = osPriorityNormal
  };

  request_queue = osMessageQueueNew(IEC104_CONTROL_QUEUE_CAPACITY,
                                    sizeof(ControlRequest), &request_attributes);
  result_queue = osMessageQueueNew(IEC104_CONTROL_QUEUE_CAPACITY,
                                   sizeof(ControlResult), &result_attributes);

  if ((request_queue == NULL) || (result_queue == NULL))
    return false;

  return osThreadNew(ControlService_Task, NULL, &thread_attributes) != NULL;
}

bool ControlService_Select(const ControlCommand *command, uint32_t session_id)
{
  if ((session_id == 0U) || !CommandIsValid(command))
    return false;

  if (has_selection &&
      ((uint32_t)(HAL_GetTick() - selection_tick) < SELECT_TIMEOUT_MS))
    return false;

  selection = *command;
  selection_session = session_id;
  selection_tick = HAL_GetTick();
  has_selection = true;
  return true;
}

bool ControlService_Execute(const ControlCommand *command, uint32_t session_id,
                            uint32_t transaction_id)
{
  ControlRequest request;

  if (!has_selection || !CommandIsValid(command) ||
      (session_id != selection_session) ||
      ((uint32_t)(HAL_GetTick() - selection_tick) >= SELECT_TIMEOUT_MS) ||
      !SameCommand(command, &selection))
    return false;

  request.command = *command;
  request.transaction_id = transaction_id;

  if (osMessageQueuePut(request_queue, &request, 0U, 0U) != osOK)
    return false;

  has_selection = false;
  return true;
}

void ControlService_CancelSelection(uint32_t session_id)
{
  if (has_selection && (selection_session == session_id))
    has_selection = false;
}

bool ControlService_TakeResult(ControlResult *result)
{
  return (result != NULL) &&
         (osMessageQueueGet(result_queue, result, NULL, 0U) == osOK);
}

uint32_t ControlService_StackFreeBytes(void)
{
  return control_stack_free_bytes;
}
