// Minimal cross-platform TCP helpers (Winsock on Windows, BSD sockets elsewhere).
#pragma once
#include <cstdint>
#include <cstring>
#include <string>

#ifdef _WIN32
  #ifndef NOMINMAX
    #define NOMINMAX  // windows.h's min/max macros break std::min
  #endif
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #include <winsock2.h>
  #include <ws2tcpip.h>
  using sock_t = SOCKET;
  #define SOCK_INVALID INVALID_SOCKET
#else
  #include <arpa/inet.h>
  #include <errno.h>
  #include <fcntl.h>
  #include <netdb.h>
  #include <netinet/in.h>
  #include <netinet/tcp.h>
  #include <sys/select.h>
  #include <sys/socket.h>
  #include <sys/time.h>
  #include <unistd.h>
  using sock_t = int;
  #define SOCK_INVALID (-1)
#endif

namespace net {

inline bool init()
{
#ifdef _WIN32
    static bool done = false;
    if (!done)
    {
        WSADATA wsa;
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return false;
        done = true;
    }
#endif
    return true;
}

inline void closeSock(sock_t s)
{
#ifdef _WIN32
    closesocket(s);
#else
    ::close(s);
#endif
}

inline void setBlocking(sock_t s, bool blocking)
{
#ifdef _WIN32
    u_long mode = blocking ? 0 : 1;
    ioctlsocket(s, FIONBIO, &mode);
#else
    int fl = fcntl(s, F_GETFL, 0);
    fcntl(s, F_SETFL, blocking ? (fl & ~O_NONBLOCK) : (fl | O_NONBLOCK));
#endif
}

inline void setRecvTimeout(sock_t s, int ms)
{
#ifdef _WIN32
    DWORD t = (DWORD)ms;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&t, sizeof(t));
#else
    timeval tv;
    tv.tv_sec = ms / 1000;
    tv.tv_usec = (ms % 1000) * 1000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif
}

inline bool connectInProgress()
{
#ifdef _WIN32
    return WSAGetLastError() == WSAEWOULDBLOCK;
#else
    return errno == EINPROGRESS;
#endif
}

// Connects with a timeout. Returns SOCK_INVALID and fills err on failure.
inline sock_t connectTcp(const std::string &host, uint16_t port, int timeoutMs, std::string &err)
{
    addrinfo hints;
    std::memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    addrinfo *res = nullptr;
    const std::string portStr = std::to_string(port);
    if (getaddrinfo(host.c_str(), portStr.c_str(), &hints, &res) != 0 || !res)
    {
        err = "could not resolve host '" + host + "'";
        return SOCK_INVALID;
    }

    sock_t s = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (s == SOCK_INVALID)
    {
        freeaddrinfo(res);
        err = "socket() failed";
        return SOCK_INVALID;
    }

    setBlocking(s, false);
    int r = ::connect(s, res->ai_addr, (int)res->ai_addrlen);
    freeaddrinfo(res);

    if (r != 0)
    {
        if (!connectInProgress())
        {
            closeSock(s);
            err = "connect() failed immediately";
            return SOCK_INVALID;
        }
        fd_set wfds;
        FD_ZERO(&wfds);
        FD_SET(s, &wfds);
        timeval tv;
        tv.tv_sec = timeoutMs / 1000;
        tv.tv_usec = (timeoutMs % 1000) * 1000;
        r = select((int)s + 1, nullptr, &wfds, nullptr, &tv);
        if (r <= 0)
        {
            closeSock(s);
            err = "connection timed out (is the console on and XBDM running on port 730?)";
            return SOCK_INVALID;
        }
        int soerr = 0;
#ifdef _WIN32
        int len = sizeof(soerr);
#else
        socklen_t len = sizeof(soerr);
#endif
        getsockopt(s, SOL_SOCKET, SO_ERROR, (char *)&soerr, &len);
        if (soerr != 0)
        {
            closeSock(s);
            err = "connection refused or failed (error " + std::to_string(soerr) + ")";
            return SOCK_INVALID;
        }
    }

    setBlocking(s, true);
    setRecvTimeout(s, timeoutMs);
    int one = 1;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof(one));
    return s;
}

inline bool sendAll(sock_t s, const void *data, size_t len)
{
    const char *p = (const char *)data;
    while (len > 0)
    {
        int n = (int)::send(s, p, (int)len, 0);
        if (n <= 0) return false;
        p += n;
        len -= (size_t)n;
    }
    return true;
}

// Returns bytes received, 0 on orderly close, -1 on error/timeout.
inline int recvSome(sock_t s, void *buf, size_t len)
{
    return (int)::recv(s, (char *)buf, (int)len, 0);
}

} // namespace net
