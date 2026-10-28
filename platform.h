#ifndef PLATFORM_H
#define PLATFORM_H

// the few things sockets and files do differently on Windows, so sonded
// and sonde can use one name for each

#include <stdio.h>
#include <fcntl.h>

#ifdef _WIN32

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <io.h>

// sockets are their own handle type, not file descriptors
typedef SOCKET sock_t;
#define SOCK_INVALID INVALID_SOCKET
#define sock_close closesocket

// what a song name from a URL must not contain: anything that could
// point outside the music folder, a drive letter or an NTFS stream
#define PATH_FORBIDDEN "/\\:"

// winsock has to be started before any socket call, -1 on error
static inline int net_init(void) {
    WSADATA wsa;
    return WSAStartup(MAKEWORD(2, 2), &wsa) == 0 ? 0 : -1;
}

// perror for socket calls, which report errors through WSAGetLastError, not errno
static inline void sock_perror(const char* what) {
    char message[256];
    int code = WSAGetLastError();

    if (FormatMessageA(
            FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
            NULL, code, 0, message, sizeof(message), NULL) == 0) {
        snprintf(message, sizeof(message), "error %d\n", code);
    }
    fprintf(stderr, "%s: %s", what, message);
}

#else

#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>

typedef int sock_t;
#define SOCK_INVALID (-1)
#define sock_close close

#define PATH_FORBIDDEN "/"

// only Windows tells files and text apart
#ifndef O_BINARY
#define O_BINARY 0
#endif

static inline int net_init(void) {
    return 0;
}

static inline void sock_perror(const char* what) {
    perror(what);
}

#endif

#endif
