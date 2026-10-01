#include "FreeRTOS.h"
#include "task.h"

#include <reent.h>

/* newlib heap operations are task-only; no allocation is allowed in an ISR. */
void __malloc_lock(struct _reent *reent)
{
  (void)reent;
  if (xTaskGetSchedulerState() != taskSCHEDULER_NOT_STARTED)
    vTaskSuspendAll();
}

void __malloc_unlock(struct _reent *reent)
{
  (void)reent;
  if (xTaskGetSchedulerState() != taskSCHEDULER_NOT_STARTED)
    (void)xTaskResumeAll();
}
