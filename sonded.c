#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <netinet/in.h>

#define STREAM_PORT 8080

int main(int argc, char** argv) {
    // sonded server socket
    int listen_fd = socket(
        AF_INET, SOCK_STREAM, 0
    );

    if (listen_fd == -1) {
        perror("socket");
        return EXIT_FAILURE;
    }

    // setup ipv4 address
    struct sockaddr_in sonded_addr = {0};
    sonded_addr.sin_family = AF_INET;
    sonded_addr.sin_port = htons(STREAM_PORT);
    sonded_addr.sin_addr.s_addr = INADDR_ANY;

    int sock_bind_err = bind(
        listen_fd, 
        (struct sockaddr*)&sonded_addr,
        sizeof(sonded_addr)
    );

    if (sock_bind_err) {
        perror("bind");
        return EXIT_FAILURE;
    }
    
    int listen_err = listen(listen_fd, 1); 
    if (listen_err) {
        perror("listen");
        return EXIT_FAILURE;
    }

    // sonde client socket
    int conn_fd = accept(listen_fd, NULL, NULL);

    if (conn_fd == -1) {
        perror("accept");
        return EXIT_FAILURE;
    }


    return EXIT_SUCCESS;
}
