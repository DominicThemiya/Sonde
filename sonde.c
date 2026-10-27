#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>

#define PORT 8080

#define RESPONSE_BUFFER_SIZE 4096

#define HEADER_BUFFER_SIZE 4096

#define SONG_PATH_SIZE 2048

#define CHOICE_BUFFER_SIZE 32

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

// connect to sonded and send a GET for path. returns the socket, -1 on error
int send_get(const char* path) {
    int conn_fd = socket(AF_INET, SOCK_STREAM, 0);

    if (conn_fd == -1) {
        perror("socket");
        return -1;
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
        return -1;
    }

    char request[SONG_PATH_SIZE + 128];

    int request_len = snprintf(
        request, sizeof(request),
        "GET %s HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "Connection: close\r\n"
        "\r\n",
        path
    );

    if (send_all(conn_fd, request, request_len) == -1) {
        perror("send");
        close(conn_fd);
        return -1;
    }

    return conn_fd;
}

// read the response headers and check for 200 OK. any body bytes that
// arrived with the headers are written to out. -1 on error
int read_headers(int conn_fd, FILE* out) {
    char header_buf[HEADER_BUFFER_SIZE];
    size_t header_len = 0;
    char* header_end = NULL;

    // headers end at the first blank line
    while (header_end == NULL) {
        if (header_len == sizeof(header_buf) - 1) {
            fprintf(stderr, "Response headers too long\n");
            return -1;
        }

        ssize_t n = recv(
            conn_fd, header_buf + header_len,
            sizeof(header_buf) - 1 - header_len, 0
        );

        if (n <= 0) {
            if (n == -1) {
                perror("recv");
            } else {
                fprintf(stderr, "sonded closed the connection early\n");
            }
            return -1;
        }

        header_len += n;
        header_buf[header_len] = '\0'; // recv doesn't add terminator
        header_end = strstr(header_buf, "\r\n\r\n");
    }

    // first line is e.g. "HTTP/1.1 200 OK"
    int status;

    if (sscanf(header_buf, "HTTP/%*s %d", &status) != 1) {
        fprintf(stderr, "Malformed response from sonded\n");
        return -1;
    }

    if (status != 200) {
        char* line_end = strstr(header_buf, "\r\n");
        fprintf(stderr, "sonded replied: %.*s\n", (int)(line_end - header_buf), header_buf);
        return -1;
    }

    char* body_start = header_end + strlen("\r\n\r\n");
    size_t body_len = header_len - (body_start - header_buf);

    fwrite(body_start, 1, body_len, out);
    return 0;
}

// copy the rest of the response body to out until sonded closes, -1 on error
int read_body(int conn_fd, FILE* out) {
    char response_buf[RESPONSE_BUFFER_SIZE];
    ssize_t response_len;

    while ((response_len = recv(conn_fd, response_buf, sizeof(response_buf), 0)) > 0) {
        if (fwrite(response_buf, 1, response_len, out) != (size_t)response_len) {
            perror("fwrite");
            return -1;
        }
    }

    if (response_len == -1) {
        perror("recv");
        return -1;
    }
    return 0;
}

// GET path and write the response body to out, -1 on error
int http_get(const char* path, FILE* out) {
    int conn_fd = send_get(path);

    if (conn_fd == -1) {
        return -1;
    }

    int err = read_headers(conn_fd, out) == -1 || read_body(conn_fd, out) == -1;

    close(conn_fd);
    return err ? -1 : 0;
}

// write str into out with everything but letters, digits and -._~ as %XX.
// -1 if it doesn't fit
int url_encode(char* out, size_t out_size, const char* str) {
    size_t len = 0;

    for (; *str; str++) {
        unsigned char c = *str;

        if (len + 4 > out_size) {
            return -1;
        }

        if (isalnum(c) || strchr("-._~", c)) {
            out[len++] = c;
        } else {
            len += snprintf(out + len, out_size - len, "%%%02X", c);
        }
    }

    out[len] = '\0';
    return 0;
}

// split the one-per-line song list into names, in place. free the array only
char** split_lines(char* list, size_t* count_out) {
    size_t count = 0;

    for (char* c = list; *c; c++) {
        if (*c == '\n') {
            count++;
        }
    }

    char** names = malloc((count ? count : 1) * sizeof(*names));

    if (names == NULL) {
        perror("malloc");
        return NULL;
    }

    size_t i = 0;

    for (char* line = strtok(list, "\n"); line && i < count; line = strtok(NULL, "\n")) {
        names[i++] = line;
    }

    *count_out = i;
    return names;
}

int main(int argc, char** argv) {
    // the song goes to stdout, which makes a mess if that's the terminal
    if (isatty(STDOUT_FILENO)) {
        fprintf(stderr, "Pipe sonde into a player, e.g.: %s | mpv -\n", argv[0]);
        return EXIT_FAILURE;
    }

    // fetch the song list into memory
    char* list = NULL;
    size_t list_len = 0;
    FILE* list_stream = open_memstream(&list, &list_len);

    if (list_stream == NULL) {
        perror("open_memstream");
        return EXIT_FAILURE;
    }

    int list_err = http_get("/list.txt", list_stream);
    fclose(list_stream);

    if (list_err) {
        free(list);
        return EXIT_FAILURE;
    }

    size_t count;
    char** names = split_lines(list, &count);

    if (names == NULL) {
        free(list);
        return EXIT_FAILURE;
    }

    if (count == 0) {
        fprintf(stderr, "sonded has no songs\n");
        free(names);
        free(list);
        return EXIT_FAILURE;
    }

    // stdout is the audio, so the menu goes to stderr
    for (size_t i = 0; i < count; i++) {
        fprintf(stderr, "%3zu. %s\n", i + 1, names[i]);
    }

    fprintf(stderr, "Pick a song [1-%zu]: ", count);

    char choice_buf[CHOICE_BUFFER_SIZE];
    long choice = 0;

    if (fgets(choice_buf, sizeof(choice_buf), stdin)) {
        choice = strtol(choice_buf, NULL, 10);
    }

    if (choice < 1 || (size_t)choice > count) {
        fprintf(stderr, "Not a song number\n");
        free(names);
        free(list);
        return EXIT_FAILURE;
    }

    const char* name = names[choice - 1];

    char song_path[SONG_PATH_SIZE] = "/songs/";
    size_t prefix_len = strlen(song_path);

    if (url_encode(song_path + prefix_len, sizeof(song_path) - prefix_len, name) == -1) {
        fprintf(stderr, "Song name too long\n");
        free(names);
        free(list);
        return EXIT_FAILURE;
    }

    fprintf(stderr, "Playing %s\n", name);

    int song_err = http_get(song_path, stdout);

    free(names);
    free(list);

    return song_err ? EXIT_FAILURE : EXIT_SUCCESS;
}
