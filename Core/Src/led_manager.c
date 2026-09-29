#include "led_manager.h"

#include "app_status.h"
#include "cmsis_os2.h"
#include "lwip/netif.h"
#include "main.h"

extern struct netif gnetif;

#define LED_TASK_STACK_SIZE     (1024U)
#define LED_TASK_INTERVAL_MS    (500U)

static void LedManager_Task(void *argument);

static const osThreadAttr_t led_task_attributes =
{
  .name = "ledManager",
  .stack_size = LED_TASK_STACK_SIZE,
  .priority = osPriorityLow
};

void LedManager_Init(void)
{
  (void)osThreadNew(LedManager_Task, NULL, &led_task_attributes);
}

static void LedManager_Task(void *argument)
{
  (void)argument;

  BSP_LED_Off(LED1);
  BSP_LED_Off(LED2);
  BSP_LED_Off(LED3);

  for (;;)
  {
    /* Toggle every 500 ms: one complete ON/OFF cycle is 1 second. */
    BSP_LED_Toggle(LED1);

    if (AppStatus_IsEthernetLinkUp())
    {
      BSP_LED_On(LED2);
    }
    else
    {
      BSP_LED_Off(LED2);
    }

    if (AppStatus_IsTcpConnected())
    {
      BSP_LED_On(LED3);
    }
    else
    {
      BSP_LED_Off(LED3);
    }

    osDelay(LED_TASK_INTERVAL_MS);
  }
}
