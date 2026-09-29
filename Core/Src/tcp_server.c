#include "tcp_server.h"

#include "app_status.h"
#include "cmsis_os2.h"
#include "lwip/api.h"

#define TCP_SERVER_PORT          (3001U)
#define TCP_SERVER_STACK_SIZE    (4096U)
#define TCP_SERVER_RETRY_MS      (1000U)

static void TcpServer_Task(void *argument);
static void TcpServer_HandleClient(struct netconn *client);
static void TcpServer_SetActiveClient(struct netconn *client);

static struct netconn *active_client;
static osMutexId_t active_client_mutex;

static const osThreadAttr_t tcp_server_task_attributes =
{
  .name = "tcpServer",
  .stack_size = TCP_SERVER_STACK_SIZE,
  .priority = osPriorityNormal
};

void TcpServer_Init(void)
{
  active_client_mutex = osMutexNew(NULL);
  (void)osThreadNew(TcpServer_Task, NULL, &tcp_server_task_attributes);
}

bool TcpServer_IsClientConnected(void)
{
  bool connected = false;

  if ((active_client_mutex != NULL) &&
      (osMutexAcquire(active_client_mutex, osWaitForever) == osOK))
  {
    connected = (active_client != NULL);
    (void)osMutexRelease(active_client_mutex);
  }

  return connected;
}

TcpServer_Status_t TcpServer_Send(const void *data, uint16_t length)
{
  TcpServer_Status_t status = TCP_SERVER_ERROR_NOT_CONNECTED;
  struct netconn *client;

  if ((data == NULL) || (length == 0U))
  {
    return TCP_SERVER_ERROR_INVALID_ARGUMENT;
  }

  if (active_client_mutex == NULL)
  {
    return TCP_SERVER_ERROR_NOT_CONNECTED;
  }

  if (osMutexAcquire(active_client_mutex, osWaitForever) == osOK)
  {
    client = active_client;
    if (client != NULL)
    {
      status = (netconn_write(client, data, length, NETCONN_COPY) == ERR_OK)
                 ? TCP_SERVER_OK
                 : TCP_SERVER_ERROR_SEND;
    }
    (void)osMutexRelease(active_client_mutex);
  }

  TcpServer_TxCallback((const uint8_t *)data, length, status);
  return status;
}

__attribute__((weak)) void TcpServer_ConnectionCallback(bool connected)
{
  (void)connected;
}

__attribute__((weak)) void TcpServer_RxCallback(const uint8_t *data, uint16_t length)
{
  (void)data;
  (void)length;
}

__attribute__((weak)) void TcpServer_TxCallback(const uint8_t *data, uint16_t length,
                                                TcpServer_Status_t status)
{
  (void)data;
  (void)length;
  (void)status;
}

static void TcpServer_SetActiveClient(struct netconn *client)
{
  if ((active_client_mutex != NULL) &&
      (osMutexAcquire(active_client_mutex, osWaitForever) == osOK))
  {
    active_client = client;
    (void)osMutexRelease(active_client_mutex);
  }
}

static void TcpServer_Task(void *argument)
{
  struct netconn *listener;
  struct netconn *client;
  err_t err;

  (void)argument;
  AppStatus_SetTcpConnected(false);

  for (;;)
  {
    listener = netconn_new(NETCONN_TCP);
    if (listener == NULL)
    {
      osDelay(TCP_SERVER_RETRY_MS);
      continue;
    }

    err = netconn_bind(listener, IP_ADDR_ANY, TCP_SERVER_PORT);
    if (err == ERR_OK)
    {
      err = netconn_listen(listener);
    }

    if (err != ERR_OK)
    {
      netconn_delete(listener);
      osDelay(TCP_SERVER_RETRY_MS);
      continue;
    }

    for (;;)
    {
      client = NULL;
      err = netconn_accept(listener, &client);
      if ((err != ERR_OK) || (client == NULL))
      {
        break;
      }

      TcpServer_SetActiveClient(client);
      AppStatus_SetTcpConnected(true);
      TcpServer_ConnectionCallback(true);
      TcpServer_HandleClient(client);
      TcpServer_SetActiveClient(NULL);
      AppStatus_SetTcpConnected(false);
      TcpServer_ConnectionCallback(false);

      netconn_close(client);
      netconn_delete(client);
    }

    AppStatus_SetTcpConnected(false);
    netconn_close(listener);
    netconn_delete(listener);
    osDelay(TCP_SERVER_RETRY_MS);
  }
}

static void TcpServer_HandleClient(struct netconn *client)
{
  struct netbuf *rx_buffer;
  void *data;
  u16_t length;

  while (netconn_recv(client, &rx_buffer) == ERR_OK)
  {
    do
    {
      netbuf_data(rx_buffer, &data, &length);
      if ((data != NULL) && (length > 0U))
      {
        TcpServer_RxCallback((const uint8_t *)data, length);
      }
    }
    while (netbuf_next(rx_buffer) >= 0);

    netbuf_delete(rx_buffer);
  }
}
