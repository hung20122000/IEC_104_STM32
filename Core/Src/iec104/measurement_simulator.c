#include "iec104/measurement_simulator.h"

#include "iec104/point_database.h"

#include "FreeRTOS.h"
#include "cmsis_os2.h"
#include "task.h"

#define SIMULATOR_STACK_BYTES 2048U

static StaticTask_t simulator_task_control;
static StackType_t simulator_task_stack[SIMULATOR_STACK_BYTES / sizeof(StackType_t)];
static volatile uint32_t simulator_stack_free_bytes;

static void MeasurementSimulator_Task(void *argument)
{
  uint32_t step = 0U;
  float triangle = 0.0f;
  float direction = 0.1f;
  PointValue value;

  (void)argument;

  for (;;)
  {
    osDelay(1000U);
    step++;

    triangle += direction;
    if ((triangle >= 0.9f) || (triangle <= -0.9f))
      direction = -direction;

    value.normalized = triangle;
    (void)PointDatabase_UpdateSimulated(POINT_IOA_NORMALIZED, value, 0U);

    value.scaled = (int32_t)(step % 100U);
    (void)PointDatabase_UpdateSimulated(POINT_IOA_SCALED, value, 0U);

    value.real = 20.0f + (float)(step % 20U) * 0.25f;
    (void)PointDatabase_UpdateSimulated(POINT_IOA_FLOAT, value, 0U);

    simulator_stack_free_bytes =
      uxTaskGetStackHighWaterMark(NULL) * sizeof(StackType_t);
  }
}

uint32_t MeasurementSimulator_StackFreeBytes(void)
{
  return simulator_stack_free_bytes;
}

bool MeasurementSimulator_Init(void)
{
  const osThreadAttr_t attributes =
  {
    .name = "measureSim",
    .stack_mem = simulator_task_stack,
    .stack_size = sizeof(simulator_task_stack),
    .cb_mem = &simulator_task_control,
    .cb_size = sizeof(simulator_task_control),
    .priority = osPriorityBelowNormal
  };

  return osThreadNew(MeasurementSimulator_Task, NULL, &attributes) != NULL;
}
