/*
 *  socket_lwip.c
 *
 *  LwIP socket HAL implementation for lib60870.
 *
 *  This file is part of the Platform Abstraction Layer (libpal)
 *  for libiec61850, libmms, and lib60870.
 */

#include "hal_socket.h"
#include "lib_memory.h"

#include "lwip/opt.h"

#if !LWIP_SOCKET
#error "socket_lwip.c requires LWIP_SOCKET=1"
#endif

#if !LWIP_SOCKET_POLL
#error "socket_lwip.c requires LWIP_SOCKET_POLL=1"
#endif

#include "lwip/api.h"
#include "lwip/inet.h"
#include "lwip/ip_addr.h"
#include "lwip/sockets.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

#define SOCKET_ADDRESS_STRING_SIZE 60
#define DEFAULT_CONNECT_TIMEOUT_MS 5000U

#if LWIP_IPV6
#define IP_ADDRESS_STRING_SIZE INET6_ADDRSTRLEN
#else
#define IP_ADDRESS_STRING_SIZE INET_ADDRSTRLEN
#endif

struct sSocket
{
    int fd;
    uint32_t connectTimeout;
};

struct sServerSocket
{
    int fd;
    int backLog;
};

struct sUdpSocket
{
    int fd;
    int addressFamily;
};

/*
 * LwIP cannot have more active sockets than MEMP_NUM_NETCONN. Keeping the
 * handles in a fixed-size array avoids heap allocation in Handleset_reset,
 * which is called repeatedly by the threadless CS104 slave.
 */
struct sHandleSet
{
    Socket sockets[MEMP_NUM_NETCONN];
    int socketCount;
};

static bool
isWouldBlockError(void)
{
    return (errno == EAGAIN) || (errno == EWOULDBLOCK);
}

static bool
setFdNonBlocking(int fd)
{
    int flags = lwip_fcntl(fd, F_GETFL, 0);

    if (flags < 0)
        return false;

    return (lwip_fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0);
}

static void
activateTcpNoDelay(int fd)
{
    int enabled = 1;

    (void)lwip_setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &enabled, sizeof(enabled));
}

static bool
resolveIpv4Address(const char* address, ip4_addr_t* ipAddress)
{
    if (ip4addr_aton(address, ipAddress) != 0)
        return true;

#if LWIP_DNS
    ip_addr_t resolvedAddress;

    if ((netconn_gethostbyname(address, &resolvedAddress) == ERR_OK) && IP_IS_V4(&resolvedAddress))
    {
        ip4_addr_copy(*ipAddress, *ip_2_ip4(&resolvedAddress));
        return true;
    }
#endif

    return false;
}

static bool
prepareIpv4Address(const char* address, int port, struct sockaddr_in* socketAddress)
{
    memset(socketAddress, 0, sizeof(*socketAddress));

    socketAddress->sin_family = AF_INET;
    socketAddress->sin_port = lwip_htons((uint16_t)port);

    if (address == NULL)
    {
        socketAddress->sin_addr.s_addr = PP_HTONL(INADDR_ANY);
        return true;
    }

    ip4_addr_t ipAddress;

    if (!resolveIpv4Address(address, &ipAddress))
        return false;

    socketAddress->sin_addr.s_addr = ip4_addr_get_u32(&ipAddress);

    return true;
}

static bool
formatSocketAddress(const struct sockaddr_storage* socketAddress, char* output, size_t outputSize)
{
    char ipAddress[IP_ADDRESS_STRING_SIZE];
    int written;

    if ((socketAddress == NULL) || (output == NULL) || (outputSize == 0U))
        return false;

    if (socketAddress->ss_family == AF_INET)
    {
        const struct sockaddr_in* ipv4Address = (const struct sockaddr_in*)socketAddress;

        if (lwip_inet_ntop(AF_INET, &ipv4Address->sin_addr, ipAddress, sizeof(ipAddress)) == NULL)
            return false;

        written = snprintf(output, outputSize, "%s:%u", ipAddress, (unsigned int)lwip_ntohs(ipv4Address->sin_port));
    }
#if LWIP_IPV6
    else if (socketAddress->ss_family == AF_INET6)
    {
        const struct sockaddr_in6* ipv6Address = (const struct sockaddr_in6*)socketAddress;

        if (lwip_inet_ntop(AF_INET6, &ipv6Address->sin6_addr, ipAddress, sizeof(ipAddress)) == NULL)
            return false;

        written = snprintf(output, outputSize, "[%s]:%u", ipAddress, (unsigned int)lwip_ntohs(ipv6Address->sin6_port));
    }
#endif
    else
    {
        return false;
    }

    return (written >= 0) && ((size_t)written < outputSize);
}

static char*
getSocketAddress(int fd, bool peerAddress)
{
    struct sockaddr_storage socketAddress;
    socklen_t socketAddressLength = sizeof(socketAddress);
    char addressString[SOCKET_ADDRESS_STRING_SIZE];
    int result;

    if (peerAddress)
        result = lwip_getpeername(fd, (struct sockaddr*)&socketAddress, &socketAddressLength);
    else
        result = lwip_getsockname(fd, (struct sockaddr*)&socketAddress, &socketAddressLength);

    if (result != 0)
        return NULL;

    if (!formatSocketAddress(&socketAddress, addressString, sizeof(addressString)))
        return NULL;

    size_t addressLength = strlen(addressString) + 1U;
    char* resultAddress = (char*)GLOBAL_MALLOC(addressLength);

    if (resultAddress != NULL)
        memcpy(resultAddress, addressString, addressLength);

    return resultAddress;
}

HandleSet
Handleset_new(void)
{
    return (HandleSet)GLOBAL_CALLOC(1, sizeof(struct sHandleSet));
}

void
Handleset_reset(HandleSet self)
{
    if (self != NULL)
    {
        memset(self->sockets, 0, sizeof(self->sockets));
        self->socketCount = 0;
    }
}

void
Handleset_addSocket(HandleSet self, const Socket socket)
{
    if ((self == NULL) || (socket == NULL) || (socket->fd < 0))
        return;

    for (int i = 0; i < self->socketCount; i++)
    {
        if (self->sockets[i] == socket)
            return;
    }

    if (self->socketCount < MEMP_NUM_NETCONN)
    {
        self->sockets[self->socketCount] = socket;
        self->socketCount++;
    }
}

void
Handleset_removeSocket(HandleSet self, const Socket socket)
{
    if ((self == NULL) || (socket == NULL))
        return;

    for (int i = 0; i < self->socketCount; i++)
    {
        if (self->sockets[i] == socket)
        {
            for (int remaining = i; remaining < (self->socketCount - 1); remaining++)
                self->sockets[remaining] = self->sockets[remaining + 1];

            self->socketCount--;
            self->sockets[self->socketCount] = NULL;
            return;
        }
    }
}

int
Handleset_waitReady(HandleSet self, unsigned int timeoutMs)
{
    struct pollfd pollDescriptors[MEMP_NUM_NETCONN];

    if (self == NULL)
        return -1;

    if (self->socketCount == 0)
        return 0;

    memset(pollDescriptors, 0, sizeof(pollDescriptors));

    for (int i = 0; i < self->socketCount; i++)
    {
        pollDescriptors[i].fd = self->sockets[i]->fd;
        pollDescriptors[i].events = POLLIN;
    }

    int pollTimeout = (timeoutMs > (unsigned int)INT_MAX) ? INT_MAX : (int)timeoutMs;

    return lwip_poll(pollDescriptors, (nfds_t)self->socketCount, pollTimeout);
}

void
Handleset_destroy(HandleSet self)
{
    if (self != NULL)
        GLOBAL_FREEMEM(self);
}

void
Socket_activateTcpKeepAlive(Socket self, int idleTime, int interval, int count)
{
    if ((self == NULL) || (self->fd < 0))
        return;

    int enabled = 1;

    (void)lwip_setsockopt(self->fd, SOL_SOCKET, SO_KEEPALIVE, &enabled, sizeof(enabled));

#if LWIP_TCP_KEEPALIVE
    (void)lwip_setsockopt(self->fd, IPPROTO_TCP, TCP_KEEPIDLE, &idleTime, sizeof(idleTime));
    (void)lwip_setsockopt(self->fd, IPPROTO_TCP, TCP_KEEPINTVL, &interval, sizeof(interval));
    (void)lwip_setsockopt(self->fd, IPPROTO_TCP, TCP_KEEPCNT, &count, sizeof(count));
#else
    (void)idleTime;
    (void)interval;
    (void)count;
#endif
}

ServerSocket
TcpServerSocket_create(const char* address, int port)
{
    struct sockaddr_in serverAddress;

    if (!prepareIpv4Address(address, port, &serverAddress))
        return NULL;

    int fd = lwip_socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);

    if (fd < 0)
        return NULL;

    int reuseAddress = 1;
    (void)lwip_setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuseAddress, sizeof(reuseAddress));

    if (lwip_bind(fd, (struct sockaddr*)&serverAddress, sizeof(serverAddress)) != 0)
    {
        (void)lwip_close(fd);
        return NULL;
    }

    if (!setFdNonBlocking(fd))
    {
        (void)lwip_close(fd);
        return NULL;
    }

    ServerSocket self = (ServerSocket)GLOBAL_MALLOC(sizeof(struct sServerSocket));

    if (self == NULL)
    {
        (void)lwip_close(fd);
        return NULL;
    }

    self->fd = fd;
    self->backLog = 2;

    return self;
}

void
ServerSocket_listen(ServerSocket self)
{
    if ((self != NULL) && (self->fd >= 0))
        (void)lwip_listen(self->fd, self->backLog);
}

Socket
ServerSocket_accept(ServerSocket self)
{
    if ((self == NULL) || (self->fd < 0))
        return NULL;

    int fd = lwip_accept(self->fd, NULL, NULL);

    if (fd < 0)
        return NULL;

    Socket connection = (Socket)GLOBAL_CALLOC(1, sizeof(struct sSocket));

    if (connection == NULL)
    {
        (void)lwip_close(fd);
        return NULL;
    }

    connection->fd = fd;
    connection->connectTimeout = DEFAULT_CONNECT_TIMEOUT_MS;

    if (!setFdNonBlocking(fd))
    {
        (void)lwip_close(fd);
        GLOBAL_FREEMEM(connection);
        return NULL;
    }

    activateTcpNoDelay(fd);

    return connection;
}

void
ServerSocket_setBacklog(ServerSocket self, int backlog)
{
    if (self != NULL)
        self->backLog = backlog;
}

void
ServerSocket_destroy(ServerSocket self)
{
    if (self == NULL)
        return;

    int fd = self->fd;
    self->fd = -1;

    if (fd >= 0)
    {
        (void)lwip_shutdown(fd, SHUT_RDWR);
        (void)lwip_close(fd);
    }

    GLOBAL_FREEMEM(self);
}

Socket
TcpSocket_create(void)
{
    int fd = lwip_socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);

    if (fd < 0)
        return NULL;

    Socket self = (Socket)GLOBAL_CALLOC(1, sizeof(struct sSocket));

    if (self == NULL)
    {
        (void)lwip_close(fd);
        return NULL;
    }

    self->fd = fd;
    self->connectTimeout = DEFAULT_CONNECT_TIMEOUT_MS;

    return self;
}

void
Socket_setConnectTimeout(Socket self, uint32_t timeoutInMs)
{
    if (self != NULL)
        self->connectTimeout = timeoutInMs;
}

bool
Socket_bind(Socket self, const char* sourceAddress, int sourcePort)
{
    struct sockaddr_in localAddress;

    if ((self == NULL) || (self->fd < 0))
        return false;

    if (!prepareIpv4Address(sourceAddress, (sourcePort < 1) ? 0 : sourcePort, &localAddress))
        return false;

    if (lwip_bind(self->fd, (struct sockaddr*)&localAddress, sizeof(localAddress)) != 0)
    {
        (void)lwip_close(self->fd);
        self->fd = -1;
        return false;
    }

    return true;
}

bool
Socket_connectAsync(Socket self, const char* address, int port)
{
    struct sockaddr_in serverAddress;

    if ((self == NULL) || (self->fd < 0) || (address == NULL))
        return false;

    if (!prepareIpv4Address(address, port, &serverAddress))
        return false;

    activateTcpNoDelay(self->fd);

    if (!setFdNonBlocking(self->fd))
        return false;

    int result = lwip_connect(self->fd, (struct sockaddr*)&serverAddress, sizeof(serverAddress));

    if (result == 0)
        return true;

    return (errno == EINPROGRESS) || (errno == EALREADY) || isWouldBlockError();
}

SocketState
Socket_checkAsyncConnectState(Socket self)
{
    if ((self == NULL) || (self->fd < 0))
        return SOCKET_STATE_FAILED;

    fd_set writeSet;
    fd_set errorSet;
    struct timeval timeout = {0, 0};

    FD_ZERO(&writeSet);
    FD_ZERO(&errorSet);
    FD_SET(self->fd, &writeSet);
    FD_SET(self->fd, &errorSet);

    int result = lwip_select(self->fd + 1, NULL, &writeSet, &errorSet, &timeout);

    if (result == 0)
        return SOCKET_STATE_CONNECTING;

    if (result < 0)
        return SOCKET_STATE_FAILED;

    int socketError = 0;
    socklen_t errorLength = sizeof(socketError);

    if (lwip_getsockopt(self->fd, SOL_SOCKET, SO_ERROR, &socketError, &errorLength) != 0)
        return SOCKET_STATE_FAILED;

    if (socketError == 0)
        return SOCKET_STATE_CONNECTED;

    if ((socketError == EINPROGRESS) || (socketError == EALREADY) || (socketError == EWOULDBLOCK))
        return SOCKET_STATE_CONNECTING;

    return SOCKET_STATE_FAILED;
}

bool
Socket_connect(Socket self, const char* address, int port)
{
    if (!Socket_connectAsync(self, address, port))
        return false;

    fd_set writeSet;
    fd_set errorSet;
    struct timeval timeout;

    timeout.tv_sec = (long)(self->connectTimeout / 1000U);
    timeout.tv_usec = (long)((self->connectTimeout % 1000U) * 1000U);

    FD_ZERO(&writeSet);
    FD_ZERO(&errorSet);
    FD_SET(self->fd, &writeSet);
    FD_SET(self->fd, &errorSet);

    int result = lwip_select(self->fd + 1, NULL, &writeSet, &errorSet, &timeout);

    if (result > 0)
    {
        int socketError = 0;
        socklen_t errorLength = sizeof(socketError);

        if ((lwip_getsockopt(self->fd, SOL_SOCKET, SO_ERROR, &socketError, &errorLength) == 0) &&
            (socketError == 0))
        {
            return true;
        }
    }

    (void)lwip_close(self->fd);
    self->fd = -1;

    return false;
}

char*
Socket_getPeerAddress(Socket self)
{
    if ((self == NULL) || (self->fd < 0))
        return NULL;

    return getSocketAddress(self->fd, true);
}

char*
Socket_getLocalAddress(Socket self)
{
    if ((self == NULL) || (self->fd < 0))
        return NULL;

    return getSocketAddress(self->fd, false);
}

char*
Socket_getPeerAddressStatic(Socket self, char* peerAddressString)
{
    struct sockaddr_storage socketAddress;
    socklen_t socketAddressLength = sizeof(socketAddress);

    if ((self == NULL) || (self->fd < 0) || (peerAddressString == NULL))
        return NULL;

    if (lwip_getpeername(self->fd, (struct sockaddr*)&socketAddress, &socketAddressLength) != 0)
        return NULL;

    if (!formatSocketAddress(&socketAddress, peerAddressString, SOCKET_ADDRESS_STRING_SIZE))
        return NULL;

    return peerAddressString;
}

int
Socket_read(Socket self, uint8_t* buffer, int size)
{
    if ((self == NULL) || (self->fd < 0) || (buffer == NULL) || (size < 0))
        return -1;

    if (size == 0)
        return 0;

    int receivedBytes = (int)lwip_recv(self->fd, buffer, (size_t)size, MSG_DONTWAIT);

    if (receivedBytes > 0)
        return receivedBytes;

    if (receivedBytes == 0)
        return -1;

    if (isWouldBlockError())
        return 0;

    return -1;
}

int
Socket_write(Socket self, uint8_t* buffer, int size)
{
    if ((self == NULL) || (self->fd < 0) || (buffer == NULL) || (size < 0))
        return -1;

    if (size == 0)
        return 0;

    int writtenBytes = (int)lwip_send(self->fd, buffer, (size_t)size, MSG_DONTWAIT | MSG_NOSIGNAL);

    if ((writtenBytes < 0) && isWouldBlockError())
        return 0;

    return writtenBytes;
}

void
Socket_destroy(Socket self)
{
    if (self == NULL)
        return;

    int fd = self->fd;
    self->fd = -1;

    if (fd >= 0)
    {
        (void)lwip_shutdown(fd, SHUT_RDWR);
        (void)lwip_close(fd);
    }

    GLOBAL_FREEMEM(self);
}

static UdpSocket
createUdpSocket(int addressFamily)
{
#if !LWIP_UDP
    (void)addressFamily;
    return NULL;
#else
    int fd = lwip_socket(addressFamily, SOCK_DGRAM, IPPROTO_UDP);

    if (fd < 0)
        return NULL;

    UdpSocket self = (UdpSocket)GLOBAL_CALLOC(1, sizeof(struct sUdpSocket));

    if (self == NULL)
    {
        (void)lwip_close(fd);
        return NULL;
    }

    self->fd = fd;
    self->addressFamily = addressFamily;

    return self;
#endif
}

UdpSocket
UdpSocket_create(void)
{
    return createUdpSocket(AF_INET);
}

UdpSocket
UdpSocket_createIpV6(void)
{
#if LWIP_IPV6
    return createUdpSocket(AF_INET6);
#else
    return NULL;
#endif
}

bool
UdpSocket_addGroupMembership(UdpSocket self, const char* multicastAddress)
{
    if ((self == NULL) || (self->fd < 0) || (multicastAddress == NULL))
        return false;

    if (self->addressFamily == AF_INET)
    {
#if LWIP_IGMP
        struct ip_mreq membership;

        memset(&membership, 0, sizeof(membership));

        if (lwip_inet_pton(AF_INET, multicastAddress, &membership.imr_multiaddr) != 1)
            return false;

        membership.imr_interface.s_addr = PP_HTONL(INADDR_ANY);

        return (lwip_setsockopt(self->fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &membership, sizeof(membership)) == 0);
#else
        return false;
#endif
    }

#if LWIP_IPV6 && LWIP_IPV6_MLD
    if (self->addressFamily == AF_INET6)
    {
        struct ipv6_mreq membership;

        memset(&membership, 0, sizeof(membership));

        if (lwip_inet_pton(AF_INET6, multicastAddress, &membership.ipv6mr_multiaddr) != 1)
            return false;

        membership.ipv6mr_interface = 0;

        return (lwip_setsockopt(self->fd, IPPROTO_IPV6, IPV6_JOIN_GROUP, &membership, sizeof(membership)) == 0);
    }
#endif

    return false;
}

bool
UdpSocket_setMulticastTtl(UdpSocket self, int ttl)
{
    if ((self == NULL) || (self->fd < 0) || (ttl < 0) || (ttl > 255))
        return false;

#if LWIP_IPV4 && LWIP_MULTICAST_TX_OPTIONS && LWIP_UDP
    if (self->addressFamily == AF_INET)
    {
        uint8_t multicastTtl = (uint8_t)ttl;

        return (lwip_setsockopt(self->fd, IPPROTO_IP, IP_MULTICAST_TTL, &multicastTtl,
                                sizeof(multicastTtl)) == 0);
    }
#else
    (void)ttl;
#endif

    /* LwIP 2.2.1 does not expose IPV6_MULTICAST_HOPS in its socket API. */
    return false;
}

bool
UdpSocket_bind(UdpSocket self, const char* address, int port)
{
    struct sockaddr_in localAddress;

    if ((self == NULL) || (self->fd < 0) || (self->addressFamily != AF_INET))
        return false;

    if (!prepareIpv4Address(address, port, &localAddress))
        return false;

    return (lwip_bind(self->fd, (struct sockaddr*)&localAddress, sizeof(localAddress)) == 0);
}

bool
UdpSocket_sendTo(UdpSocket self, const char* address, int port, uint8_t* message, int messageSize)
{
    struct sockaddr_in remoteAddress;

    if ((self == NULL) || (self->fd < 0) || (self->addressFamily != AF_INET) ||
        (address == NULL) || (message == NULL) || (messageSize < 0))
    {
        return false;
    }

    if (!prepareIpv4Address(address, port, &remoteAddress))
        return false;

    int sentBytes = (int)lwip_sendto(self->fd, message, (size_t)messageSize, 0,
                                     (struct sockaddr*)&remoteAddress, sizeof(remoteAddress));

    return (sentBytes == messageSize);
}

int
UdpSocket_receiveFrom(UdpSocket self, char* address, int maxAddressSize, uint8_t* message, int messageSize)
{
    struct sockaddr_storage remoteAddress;
    socklen_t remoteAddressLength = sizeof(remoteAddress);

    if ((self == NULL) || (self->fd < 0) || (message == NULL) || (messageSize < 0))
        return -1;

    int receivedBytes = (int)lwip_recvfrom(self->fd, message, (size_t)messageSize, MSG_DONTWAIT,
                                           (struct sockaddr*)&remoteAddress, &remoteAddressLength);

    if (receivedBytes < 0)
        return -1;

    if ((address != NULL) && (maxAddressSize > 0))
    {
        if (!formatSocketAddress(&remoteAddress, address, (size_t)maxAddressSize))
            return -1;
    }

    return receivedBytes;
}
