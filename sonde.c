#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>

#define PORT 8080

#define RESPONSE_BUFFER_SIZE 1000

int main(int argc, char** argv) {
    int conn_fd = socket(AF_INET, SOCK_STREAM, 0);

    if (conn_fd == -1) {
        perror("socket");
        return EXIT_FAILURE;
    }

    struct sockaddr_in serv_addr = {0};
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(PORT);
    serv_addr.sin_addr.s_addr = INADDR_ANY;

    int connect_err = connect(
        conn_fd,
        (struct sockaddr*)&serv_addr,
        sizeof(serv_addr)
    );

    if (connect_err) {
        perror("connect");
        close(conn_fd);
        return EXIT_FAILURE;
    }

    const char* request =
        "GET / HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "Connection: close\r\n"
        "\r\n";

    if (send(conn_fd, request, strlen(request), 0) == -1) {
        perror("send");
        close(conn_fd);
        return EXIT_FAILURE;
    }

    // read until sonded closes the connection
    char response_buf[RESPONSE_BUFFER_SIZE];
    ssize_t response_len;

    while ((response_len = recv(conn_fd, response_buf, sizeof(response_buf), 0)) > 0) {
        fwrite(response_buf, 1, response_len, stdout);
    }

    if (response_len == -1) {
        perror("recv");
        close(conn_fd);
        return EXIT_FAILURE;
    }

    close(conn_fd);

    return EXIT_SUCCESS;
}
