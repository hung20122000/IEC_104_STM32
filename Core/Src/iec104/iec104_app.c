#include "iec104/iec104_app.h"

#include "iec104/control_service.h"
#include "iec104/iec104_handlers.h"
#include "iec104/measurement_simulator.h"
#include "iec104/point_database.h"

#include "app_status.h"
#include "lwip.h"
#include "lwip/netif.h"

#include "FreeRTOS.h"
#include "cmsis_os2.h"
#include "task.h"

extern struct netif gnetif;

#define IEC104_TASK_STACK_BYTES 8192U
#define IEC104_TICK_DELAY       2U
#define IEC104_RETRY_DELAY_MS   1000U

static StaticTask_t iec104_task_control;
static StackType_t iec104_task_stack[IEC104_TASK_STACK_BYTES / sizeof(StackType_t)];
static volatile uint32_t iec104_stack_free_bytes;
static volatile bool listening;

static bool NetworkReady(void)
{
  return AppStatus_IsEthernetLinkUp() &&
         netif_is_up(&gnetif) && netif_is_link_up(&gnetif);
}

static void IEC104_Task(void *argument)
{
  CS104_Slave slave = NULL;
  PointSnapshot discarded_event;
  uint32_t last_attempt = 0U;
  unsigned index;
  (void)argument;

  for (;;)
  {
    if (!NetworkReady())
    {
      if (slave != NULL)
      {
        listening = false;
        IEC104_HandlersDetach();
        CS104_Slave_destroy(slave);
        slave = NULL;
      }
    }
    else if (slave == NULL)
    {
      if ((last_attempt == 0U) ||
          ((uint32_t)(osKernelGetTickCount() - last_attempt) >=
           IEC104_RETRY_DELAY_MS))
      {
        last_attempt = osKernelGetTickCount();
        slave = CS104_Slave_create(16, 8);
        if (slave != NULL)
        {
          CS104_Slave_setLocalPort(slave, 2404);
          CS104_Slave_setMaxOpenConnections(slave, 1);
          IEC104_HandlersAttach(slave);
          CS104_Slave_startThreadless(slave);
          listening = CS104_Slave_isRunning(slave);
          if (!listening)
          {
            IEC104_HandlersDetach();
            CS104_Slave_destroy(slave);
            slave = NULL;
          }
        }
      }
    }
    else
    {
      CS104_Slave_tick(slave);
      IEC104_HandlersProcess();
      if (!CS104_Slave_isRunning(slave))
      {
        listening = false;
        IEC104_HandlersDetach();
        CS104_Slave_destroy(slave);
        slave = NULL;
      }
    }

    if (slave == NULL)
    {
      /* No master can receive these events. A later GI reads current values. */
      for (index = 0U; index < 4U; index++)
      {
        if (!PointDatabase_TakeEvent(&discarded_event))
          break;
      }
    }

    iec104_stack_free_bytes =
      uxTaskGetStackHighWaterMark(NULL) * sizeof(StackType_t);
    osDelay(IEC104_TICK_DELAY);
  }
}

bool IEC104_AppInit(void)
{
  const osThreadAttr_t attributes =
  {
    .name = "IEC104Task",
    .stack_mem = iec104_task_stack,
    .stack_size = sizeof(iec104_task_stack),
    .cb_mem = &iec104_task_control,
    .cb_size = sizeof(iec104_task_control),
    .priority = osPriorityNormal
  };

  if (!PointDatabase_Init() || !ControlService_Init() ||
      !MeasurementSimulator_Init())
    return false;

  return osThreadNew(IEC104_Task, NULL, &attributes) != NULL;
}

uint32_t IEC104_AppStackFreeBytes(void)
{
  return iec104_stack_free_bytes;
}

bool IEC104_AppIsListening(void)
{
  return listening;
}
