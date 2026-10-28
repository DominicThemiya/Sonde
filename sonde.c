#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "platform.h"
#include "text.h"

#define PORT 8080

#define RESPONSE_BUFFER_SIZE 4096

#define HEADER_BUFFER_SIZE 4096

#define SONG_PATH_SIZE 2048

#define CHOICE_BUFFER_SIZE 32

// keep sending until all len bytes are out, -1 on error
int send_all(sock_t fd, const char* buf, size_t len) {
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

// connect to sonded and send a GET for path. returns the socket, SOCK_INVALID on error
sock_t send_get(const char* path) {
    sock_t conn_fd = socket(AF_INET, SOCK_STREAM, 0);

    if (conn_fd == SOCK_INVALID) {
        sock_perror("socket");
        return SOCK_INVALID;
    }

    // sonded on this machine. Windows won't connect to 0.0.0.0
    struct sockaddr_in serv_addr = {0};
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(PORT);
    serv_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    int connect_err = connect(
        conn_fd,
        (struct sockaddr*)&serv_addr,
        sizeof(serv_addr)
    );

    if (connect_err) {
        sock_perror("connect");
        sock_close(conn_fd);
        return SOCK_INVALID;
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
        sock_perror("send");
        sock_close(conn_fd);
        return SOCK_INVALID;
    }

    return conn_fd;
}

// where a response body goes: a file like stdout, or text in memory
struct body_out {
    FILE* file;
    struct text* text;
};

// -1 on error
int write_out(struct body_out* out, const char* data, size_t len) {
    if (out->text) {
        return text_append(out->text, data, len);
    }
    return fwrite(data, 1, len, out->file) == len ? 0 : -1;
}

// read the response headers and check for 200 OK. any body bytes that
// arrived with the headers are written to out. -1 on error
int read_headers(sock_t conn_fd, struct body_out* out) {
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
                sock_perror("recv");
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

    if (write_out(out, body_start, body_len) == -1) {
        perror("write");
        return -1;
    }
    return 0;
}

// copy the rest of the response body to out until sonded closes, -1 on error
int read_body(sock_t conn_fd, struct body_out* out) {
    char response_buf[RESPONSE_BUFFER_SIZE];
    ssize_t response_len;

    while ((response_len = recv(conn_fd, response_buf, sizeof(response_buf), 0)) > 0) {
        if (write_out(out, response_buf, response_len) == -1) {
            perror("write");
            return -1;
        }
    }

    if (response_len == -1) {
        sock_perror("recv");
        return -1;
    }
    return 0;
}

// GET path and write the response body to out, -1 on error
int http_get(const char* path, struct body_out* out) {
    sock_t conn_fd = send_get(path);

    if (conn_fd == SOCK_INVALID) {
        return -1;
    }

    int err = read_headers(conn_fd, out) == -1 || read_body(conn_fd, out) == -1;

    sock_close(conn_fd);
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
    (void)argc;

    // the song goes to stdout, which makes a mess if that's the terminal
    if (isatty(fileno(stdout))) {
        fprintf(stderr, "Pipe sonde into a player, e.g.: %s | mpv -\n", argv[0]);
        return EXIT_FAILURE;
    }

#ifdef _WIN32
    // stdout would otherwise turn every \n byte in the song into \r\n
    _setmode(_fileno(stdout), _O_BINARY);
    SetConsoleOutputCP(CP_UTF8);
#endif

    if (net_init() == -1) {
        fprintf(stderr, "Couldn't start networking\n");
        return EXIT_FAILURE;
    }

    // fetch the song list into memory
    struct text list_text = {0};
    struct body_out list_out = { .text = &list_text };

    if (http_get("/list.txt", &list_out) == -1 || list_text.failed) {
        text_free(&list_text);
        return EXIT_FAILURE;
    }

    // no songs means nothing was added, so give split_lines an empty string
    char* list = list_text.data ? list_text.data : calloc(1, 1);

    if (list == NULL) {
        perror("calloc");
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

    struct body_out song_out = { .file = stdout };
    int song_err = http_get(song_path, &song_out);

    free(names);
    free(list);

    return song_err ? EXIT_FAILURE : EXIT_SUCCESS;
}
