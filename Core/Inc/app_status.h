#ifndef APP_STATUS_H
#define APP_STATUS_H

#include <stdbool.h>

void AppStatus_SetTcpConnected(bool connected);
bool AppStatus_IsTcpConnected(void);
void AppStatus_SetEthernetLink(bool link_up);
bool AppStatus_IsEthernetLinkUp(void);

#endif /* APP_STATUS_H */
