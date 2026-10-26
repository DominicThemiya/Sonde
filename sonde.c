#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <netinet/in.h>

#define PORT 8080

int main(int argc, char** argv) {
    int conn_fd = socket(AF_INET, SOCK_STREAM, 0);

    struct sockaddr_in serv_addr;
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(PORT);
    serv_addr.sin_addr.s_addr = INADDR_ANY;

    int connect_err = connect(
        conn_fd,
        (struct sockaddr*)&serv_addr,
        sizeof(serv_addr)
    );

    return EXIT_SUCCESS;
}
