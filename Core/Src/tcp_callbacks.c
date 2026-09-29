#include "tcp_server.h"

/**
 * @brief  Notification when the TCP client connection state changes.
 * @param  connected true when a client has connected to port 3001;
 *                    false after that client disconnects or the connection fails.
 * @note   This callback runs in the TCP server task. Keep processing short and
 *         non-blocking. For lengthy work, notify another FreeRTOS task using a
 *         queue, task notification, or event flag.
 */
void TcpServer_ConnectionCallback(bool connected)
{
  if (connected)
  {
    /* USER CODE BEGIN TCP_CONNECTED
     * Example: notify the application that a TCP client is ready.
     * Do not call TcpServer_Init() from this callback.
     */
    /* USER CODE END TCP_CONNECTED */
  }
  else
  {
    /* USER CODE BEGIN TCP_DISCONNECTED
     * Example: clear connection-dependent state or notify another task.
     * TcpServer_Send() will return TCP_SERVER_ERROR_NOT_CONNECTED here.
     */
    /* USER CODE END TCP_DISCONNECTED */
  }
}

/**
 * @brief  Handles a block of data received from the connected TCP client.
 * @param  data   Read-only pointer to the received bytes.
 * @param  length Number of valid bytes in data; the payload is binary and is
 *                not guaranteed to end with '\0'.
 * @warning data is valid only while this callback is executing. Copy it to an
 *          application-owned buffer or FreeRTOS queue before returning if it
 *          must be processed later.
 * @note   TCP is a byte stream: one application message can arrive in several
 *         callbacks, or several messages can arrive in one callback. Implement
 *         framing (length, delimiter, fixed-size packet, etc.) as required.
 * @note   This callback runs in the TCP server task; avoid long blocking work.
 */
void TcpServer_RxCallback(const uint8_t *data, uint16_t length)
{
  (void)data;
  (void)length;

  /* USER CODE BEGIN TCP_RX
   * Parse/copy data here. No echo is performed automatically.
   * To transmit a response, for example:
   *
   *   static const uint8_t response[] = { 'O', 'K', '\r', '\n' };
   *   TcpServer_Status_t result =
   *       TcpServer_Send(response, (uint16_t)sizeof(response));
   *   if (result != TCP_SERVER_OK)
   *   {
   *     // Handle disconnected client or transmit failure.
   *   }
   */
  /* USER CODE END TCP_RX */
}

/**
 * @brief  Reports the result of a TcpServer_Send() request.
 * @param  data   The same source pointer supplied to TcpServer_Send().
 * @param  length Requested number of bytes.
 * @param  status TCP_SERVER_OK when lwIP accepted the copied data, otherwise
 *                an error value from TcpServer_Status_t.
 * @note   TcpServer_Send() uses NETCONN_COPY, so the source buffer may be reused
 *         as soon as TcpServer_Send() returns. TCP_SERVER_OK does not mean that
 *         the remote application has already read or acknowledged the data.
 */
void TcpServer_TxCallback(const uint8_t *data, uint16_t length,
                          TcpServer_Status_t status)
{
  (void)data;
  (void)length;
  (void)status;

  /* USER CODE BEGIN TCP_TX
   * Update counters/status or notify another task here. Keep this callback
   * short; it executes synchronously in the task that calls TcpServer_Send().
   */
  /* USER CODE END TCP_TX */
}
