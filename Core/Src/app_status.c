#include "app_status.h"

static volatile bool tcp_connected;
static volatile bool ethernet_link_up;

void AppStatus_SetTcpConnected(bool connected)
{
  tcp_connected = connected;
}

bool AppStatus_IsTcpConnected(void)
{
  return tcp_connected;
}

void AppStatus_SetEthernetLink(bool link_up)
{
  ethernet_link_up = link_up;
}

bool AppStatus_IsEthernetLinkUp(void)
{
  return ethernet_link_up;
}
