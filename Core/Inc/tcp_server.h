#ifndef TCP_SERVER_H
#define TCP_SERVER_H

#include <stdbool.h>
#include <stdint.h>

typedef enum
{
  TCP_SERVER_OK = 0,
  TCP_SERVER_ERROR_INVALID_ARGUMENT,
  TCP_SERVER_ERROR_NOT_CONNECTED,
  TCP_SERVER_ERROR_SEND
} TcpServer_Status_t;

void TcpServer_Init(void);
bool TcpServer_IsClientConnected(void);
TcpServer_Status_t TcpServer_Send(const void *data, uint16_t length);

/* These callbacks execute in the TCP server task. The RX data pointer is valid
 * only for the duration of TcpServer_RxCallback(). Applications can provide
 * strong definitions to replace the weak default implementations. */
void TcpServer_ConnectionCallback(bool connected);
void TcpServer_RxCallback(const uint8_t *data, uint16_t length);
void TcpServer_TxCallback(const uint8_t *data, uint16_t length,
                          TcpServer_Status_t status);

#endif /* TCP_SERVER_H */
