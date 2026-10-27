#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/sendfile.h>

#define STREAM_PORT 8080

#define REQUEST_BUFFER_SIZE 1000

#define SONG_PATH "/home/dominic/Source/Sonde/music/Buckshot, fakemink - Fever.mp3"

// keep sending until all len bytes are out, -1 on error
int send_all(int fd, const char* buf, size_t len) {
    size_t sent = 0;

    while (sent < len) {
        ssize_t n = send(fd, buf + sent, len - sent, 0);

        if (n == -1) {
            return -1;
        }
        sent += n;
    }
    return 0;
}

// sonded server socket, bound and listening on port. -1 on error
int create_listen_socket(int port) {
    int listen_fd = socket(
        AF_INET, SOCK_STREAM, 0
    );

    if (listen_fd == -1) {
        perror("socket");
        return -1;
    }

    // allow restarting without waiting for the port to free up
    int reuse_addr = 1;
    setsockopt(
        listen_fd, SOL_SOCKET, SO_REUSEADDR,
        &reuse_addr, sizeof(reuse_addr)
    );

    // setup ipv4 address
    struct sockaddr_in sonded_addr = {0};
    sonded_addr.sin_family = AF_INET;
    sonded_addr.sin_port = htons(port);
    sonded_addr.sin_addr.s_addr = INADDR_ANY;

    int sock_bind_err = bind(
        listen_fd,
        (struct sockaddr*)&sonded_addr,
        sizeof(sonded_addr)
    );

    if (sock_bind_err) {
        perror("bind");
        close(listen_fd);
        return -1;
    }

    int listen_err = listen(listen_fd, 1);
    if (listen_err) {
        perror("listen");
        close(listen_fd);
        return -1;
    }

    return listen_fd;
}

// read the request into buf as a string, -1 on error or empty request
int read_request(int conn_fd, char* buf, size_t buf_size) {
    ssize_t request_len = recv(
        conn_fd, buf,
        buf_size - 1, 0
    );

    // -1 is an error, 0 means the client closed without sending anything
    if (request_len <= 0) {
        if (request_len == -1) {
            perror("recv");
        }
        return -1;
    }

    buf[request_len] = '\0'; // recv doesn't add terminator
    return 0;
}

// send an HTTP response with the song at path as the body, -1 on error
int send_song(int conn_fd, const char* path) {
    int song_fd = open(path, O_RDONLY);

    if (song_fd == -1) {
        perror("open");
        return -1;
    }

    // get its size for Content-Length
    struct stat song_stat;

    if (fstat(song_fd, &song_stat) == -1) {
        perror("fstat");
        close(song_fd);
        return -1;
    }

    char header[256];

    int header_len = snprintf(
        header, sizeof(header),
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: audio/mpeg\r\n"
        "Content-Length: %lld\r\n"
        "Connection: close\r\n"
        "\r\n",
        (long long)song_stat.st_size
    );

    if (send_all(conn_fd, header, header_len) == -1) {
        perror("send header");
        close(song_fd);
        return -1;
    }

    // sendfile can send less than asked, so loop until the whole song is out
    off_t offset = 0;

    while (offset < song_stat.st_size) {
        ssize_t n = sendfile(
            conn_fd, song_fd, &offset,
            song_stat.st_size - offset
        );

        // 0 means the file got shorter while sending
        if (n <= 0) {
            if (n == -1) {
                perror("sendfile"); // usually the client disconnected
            }
            close(song_fd);
            return -1;
        }
    }

    close(song_fd);
    return 0;
}

// serve one sonde client connection, then close it
void handle_client(int conn_fd) {
    char request_buf[REQUEST_BUFFER_SIZE];

    if (read_request(conn_fd, request_buf, sizeof(request_buf)) == 0) {
        printf("%s", request_buf);
        send_song(conn_fd, SONG_PATH);
    }

    close(conn_fd);
}

int main(int argc, char** argv) {
    signal(SIGPIPE, SIG_IGN);

    int listen_fd = create_listen_socket(STREAM_PORT);

    if (listen_fd == -1) {
        return EXIT_FAILURE;
    }

    while (1) {
        // sonde client socket
        int conn_fd = accept(listen_fd, NULL, NULL);
        if (conn_fd == -1) {
            perror("accept");
            continue;
        }

        handle_client(conn_fd);
    }
    close(listen_fd);

    return EXIT_SUCCESS;
}
