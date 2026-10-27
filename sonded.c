#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <errno.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/sendfile.h>

#include "metadata.h"

#define STREAM_PORT 8080

// browsers send a fair few headers, Range can be near the end
#define REQUEST_BUFFER_SIZE 8192

#define FULL_PATH_SIZE 4096

#define SONGS_PREFIX "/songs/"

#define ART_PREFIX "/art/"

// the web client, baked into the binary at compile time
static const unsigned char index_html[] = {
#embed "web/index.html"
};

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

// send a full HTTP response with body, -1 on error
int send_response(
    int conn_fd, const char* status, const char* content_type,
    const char* body, size_t body_len
) {
    char header[256];

    int header_len = snprintf(
        header, sizeof(header),
        "HTTP/1.1 %s\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %zu\r\n"
        "Connection: close\r\n"
        "\r\n",
        status, content_type, body_len
    );

    if (send_all(conn_fd, header, header_len) == -1 ||
        send_all(conn_fd, body, body_len) == -1) {
        perror("send response");
        return -1;
    }
    return 0;
}

// send a plain text error like "404 Not Found"
void send_error(int conn_fd, const char* status) {
    char body[64];
    int body_len = snprintf(body, sizeof(body), "%s\n", status);

    send_response(conn_fd, status, "text/plain; charset=utf-8", body, body_len);
}

// true if name ends with ext, ignoring case
int has_extension(const char* name, const char* ext) {
    size_t name_len = strlen(name);
    size_t ext_len = strlen(ext);

    return name_len > ext_len && strcasecmp(name + name_len - ext_len, ext) == 0;
}

// Content-Type for a song file, NULL if it isn't a song
const char* song_content_type(const char* name) {
    if (has_extension(name, ".mp3")) {
        return "audio/mpeg";
    }
    if (has_extension(name, ".flac")) {
        return "audio/flac";
    }
    return NULL;
}

int hex_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// turn %XX escapes back into bytes in place, -1 if malformed
int url_decode(char* str) {
    char* out = str;

    for (char* in = str; *in; in++) {
        if (*in != '%') {
            *out++ = *in;
            continue;
        }

        int high = hex_value(in[1]);
        int low = high == -1 ? -1 : hex_value(in[2]);

        // %00 would cut the name short, treat it as malformed too
        if (low == -1 || (high == 0 && low == 0)) {
            return -1;
        }

        *out++ = (char)(high * 16 + low);
        in += 2;
    }

    *out = '\0';
    return 0;
}

int compare_names(const void* a, const void* b) {
    return strcmp(*(char* const*)a, *(char* const*)b);
}

void free_song_list(char** names, size_t count) {
    for (size_t i = 0; i < count; i++) {
        free(names[i]);
    }
    free(names);
}

// collect the song file names in dir, sorted. free with free_song_list. -1 on error
int list_songs(const char* dir, char*** names_out, size_t* count_out) {
    DIR* music_dir = opendir(dir);

    if (music_dir == NULL) {
        perror("opendir");
        return -1;
    }

    char** names = NULL;
    size_t count = 0;
    size_t capacity = 0;
    struct dirent* entry;

    while ((entry = readdir(music_dir)) != NULL) {
        const char* name = entry->d_name;

        // skip hidden files, and names that would break the one-per-line list
        if (name[0] == '.' || strchr(name, '\n') || !song_content_type(name)) {
            continue;
        }

        // a folder called something.mp3 isn't a song
        struct stat entry_stat;

        if (fstatat(dirfd(music_dir), name, &entry_stat, 0) == -1 ||
            !S_ISREG(entry_stat.st_mode)) {
            continue;
        }

        if (count == capacity) {
            capacity = capacity ? capacity * 2 : 16;
            char** grown = realloc(names, capacity * sizeof(*names));

            if (grown == NULL) {
                perror("realloc");
                free_song_list(names, count);
                closedir(music_dir);
                return -1;
            }
            names = grown;
        }

        names[count] = strdup(name);

        if (names[count] == NULL) {
            perror("strdup");
            free_song_list(names, count);
            closedir(music_dir);
            return -1;
        }
        count++;
    }

    closedir(music_dir);

    qsort(names, count, sizeof(*names), compare_names);

    *names_out = names;
    *count_out = count;
    return 0;
}

// send the songs in dir, one name per line
void send_song_list(int conn_fd, const char* dir) {
    char** names;
    size_t count;

    if (list_songs(dir, &names, &count) == -1) {
        send_error(conn_fd, "500 Internal Server Error");
        return;
    }

    // build the body in memory so we know its Content-Length
    char* body = NULL;
    size_t body_len = 0;
    FILE* body_stream = open_memstream(&body, &body_len);

    if (body_stream == NULL) {
        perror("open_memstream");
        free_song_list(names, count);
        send_error(conn_fd, "500 Internal Server Error");
        return;
    }

    for (size_t i = 0; i < count; i++) {
        fprintf(body_stream, "%s\n", names[i]);
    }

    fclose(body_stream);
    free_song_list(names, count);

    send_response(conn_fd, "200 OK", "text/plain; charset=utf-8", body, body_len);

    free(body);
}

// write str as a quoted JSON string
void json_string(FILE* out, const char* str) {
    fputc('"', out);

    for (; *str; str++) {
        unsigned char c = *str;

        if (c == '"' || c == '\\') {
            fprintf(out, "\\%c", c);
        } else if (c < 0x20) {
            fprintf(out, "\\u%04x", c);
        } else {
            fputc(c, out);
        }
    }

    fputc('"', out);
}

// send every song in dir with its tags as a JSON array
void send_song_library(int conn_fd, const char* dir) {
    char** names;
    size_t count;

    if (list_songs(dir, &names, &count) == -1) {
        send_error(conn_fd, "500 Internal Server Error");
        return;
    }

    char* body = NULL;
    size_t body_len = 0;
    FILE* body_stream = open_memstream(&body, &body_len);

    if (body_stream == NULL) {
        perror("open_memstream");
        free_song_list(names, count);
        send_error(conn_fd, "500 Internal Server Error");
        return;
    }

    fputc('[', body_stream);

    for (size_t i = 0; i < count; i++) {
        char full_path[FULL_PATH_SIZE];
        struct song_meta meta;
        struct stat song_stat = {0};

        // a song we can't read the tags of is still listed, just without them
        if (snprintf(full_path, sizeof(full_path), "%s/%s", dir, names[i]) >= (int)sizeof(full_path) ||
            read_song_meta(full_path, &meta) == -1) {
            memset(&meta, 0, sizeof(meta));
        } else {
            stat(full_path, &song_stat);
        }

        fputs(i ? ",\n{\"file\":" : "\n{\"file\":", body_stream);
        json_string(body_stream, names[i]);
        fputs(",\"title\":", body_stream);
        json_string(body_stream, meta.title);
        fputs(",\"artist\":", body_stream);
        json_string(body_stream, meta.artist);
        fputs(",\"album\":", body_stream);
        json_string(body_stream, meta.album);
        fputs(",\"year\":", body_stream);
        json_string(body_stream, meta.year);
        fputs(",\"track\":", body_stream);
        json_string(body_stream, meta.track);
        fprintf(
            body_stream, ",\"size\":%lld,\"art\":%s}",
            (long long)song_stat.st_size, meta.art_mime[0] ? "true" : "false"
        );
    }

    fputs("\n]\n", body_stream);

    fclose(body_stream);
    free_song_list(names, count);

    send_response(conn_fd, "200 OK", "application/json; charset=utf-8", body, body_len);

    free(body);
}

// find a "Range: bytes=start-end" header in request for a file of size bytes.
// 0 = no usable range (send it all), 1 = start/end filled in, -1 = out of range
int parse_range(const char* request, off_t size, off_t* start, off_t* end) {
    const char* header = strcasestr(request, "\r\nRange:");

    if (header == NULL) {
        return 0;
    }

    const char* p = header + strlen("\r\nRange:");
    while (*p == ' ') {
        p++;
    }

    // only handle a single byte range, anything else gets the whole file
    if (strncasecmp(p, "bytes=", strlen("bytes=")) != 0) {
        return 0;
    }
    p += strlen("bytes=");

    const char* line_end = strstr(p, "\r\n");
    const char* comma = strchr(p, ',');
    if (comma && (line_end == NULL || comma < line_end)) {
        return 0;
    }

    char* num_end;

    // "bytes=-500" means the last 500 bytes
    if (*p == '-') {
        long long suffix = strtoll(p + 1, &num_end, 10);

        if (num_end == p + 1 || suffix <= 0 || size == 0) {
            return -1;
        }

        *start = suffix < size ? size - suffix : 0;
        *end = size - 1;
        return 1;
    }

    long long first = strtoll(p, &num_end, 10);

    if (num_end == p || *num_end != '-' || first < 0) {
        return 0;
    }
    p = num_end + 1;

    // "bytes=100-" means from 100 to the end
    long long last = -1;

    if (*p >= '0' && *p <= '9') {
        last = strtoll(p, &num_end, 10);

        if (last < first) {
            return 0;
        }
    }

    if (first >= size) {
        return -1;
    }

    if (last == -1) {
        last = size - 1;
    }

    *start = first;
    *end = last < size ? last : size - 1;
    return 1;
}

// send bytes start to end (inclusive) of file_fd, -1 on error
int send_file_range(int conn_fd, int file_fd, off_t start, off_t end) {
    // sendfile can send less than asked, so loop until the whole range is out
    off_t offset = start;

    while (offset <= end) {
        ssize_t n = sendfile(
            conn_fd, file_fd, &offset,
            end - offset + 1
        );

        // 0 means the file got shorter while sending
        if (n <= 0) {
            if (n == -1) {
                perror("sendfile"); // usually the client disconnected
            }
            return -1;
        }
    }
    return 0;
}

// send the song at path as the response body, or an error response.
// honours a Range header in request so players can seek. -1 on error
int send_song(int conn_fd, const char* path, const char* content_type, const char* request) {
    int song_fd = open(path, O_RDONLY);

    if (song_fd == -1) {
        if (errno == ENOENT) {
            send_error(conn_fd, "404 Not Found");
        } else {
            perror("open");
            send_error(conn_fd, "500 Internal Server Error");
        }
        return -1;
    }

    // get its size for Content-Length
    struct stat song_stat;

    if (fstat(song_fd, &song_stat) == -1) {
        perror("fstat");
        send_error(conn_fd, "500 Internal Server Error");
        close(song_fd);
        return -1;
    }

    // a folder called something.mp3 isn't a song
    if (!S_ISREG(song_stat.st_mode)) {
        send_error(conn_fd, "404 Not Found");
        close(song_fd);
        return -1;
    }

    off_t start = 0;
    off_t end = song_stat.st_size - 1;
    int range = parse_range(request, song_stat.st_size, &start, &end);

    if (range == -1) {
        send_error(conn_fd, "416 Range Not Satisfiable");
        close(song_fd);
        return -1;
    }

    char header[512];
    int header_len;

    if (range == 1) {
        header_len = snprintf(
            header, sizeof(header),
            "HTTP/1.1 206 Partial Content\r\n"
            "Content-Type: %s\r\n"
            "Content-Length: %lld\r\n"
            "Content-Range: bytes %lld-%lld/%lld\r\n"
            "Accept-Ranges: bytes\r\n"
            "Connection: close\r\n"
            "\r\n",
            content_type, (long long)(end - start + 1),
            (long long)start, (long long)end, (long long)song_stat.st_size
        );
    } else {
        header_len = snprintf(
            header, sizeof(header),
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: %s\r\n"
            "Content-Length: %lld\r\n"
            "Accept-Ranges: bytes\r\n"
            "Connection: close\r\n"
            "\r\n",
            content_type, (long long)song_stat.st_size
        );
    }

    if (send_all(conn_fd, header, header_len) == -1) {
        perror("send header");
        close(song_fd);
        return -1;
    }

    int err = send_file_range(conn_fd, song_fd, start, end);

    close(song_fd);
    return err;
}

// decode and check a song name from the URL, then build its full path.
// returns its Content-Type, or NULL after sending an error response
const char* resolve_song(int conn_fd, const char* music_dir, char* name, char* full_path, size_t size) {
    // reject anything that could reach outside music_dir: slashes, "..", hidden files
    if (url_decode(name) == -1 || name[0] == '\0' ||
        name[0] == '.' || strchr(name, '/')) {
        send_error(conn_fd, "404 Not Found");
        return NULL;
    }

    const char* content_type = song_content_type(name);

    if (content_type == NULL) {
        send_error(conn_fd, "404 Not Found");
        return NULL;
    }

    int full_len = snprintf(full_path, size, "%s/%s", music_dir, name);

    if (full_len >= (int)size) {
        send_error(conn_fd, "414 URI Too Long");
        return NULL;
    }

    return content_type;
}

// serve the song named by the part of the URL after /songs/
void handle_song_request(int conn_fd, const char* music_dir, char* name, const char* request) {
    char full_path[FULL_PATH_SIZE];
    const char* content_type = resolve_song(conn_fd, music_dir, name, full_path, sizeof(full_path));

    if (content_type) {
        send_song(conn_fd, full_path, content_type, request);
    }
}

// serve the cover image inside the song named by the part of the URL after /art/
void handle_art_request(int conn_fd, const char* music_dir, char* name) {
    char full_path[FULL_PATH_SIZE];

    if (resolve_song(conn_fd, music_dir, name, full_path, sizeof(full_path)) == NULL) {
        return;
    }

    struct song_meta meta;

    if (read_song_meta(full_path, &meta) == -1 || meta.art_mime[0] == '\0') {
        send_error(conn_fd, "404 Not Found");
        return;
    }

    int song_fd = open(full_path, O_RDONLY);

    if (song_fd == -1) {
        send_error(conn_fd, "404 Not Found");
        return;
    }

    // covers rarely change, let the browser keep them for an hour
    char header[512];

    int header_len = snprintf(
        header, sizeof(header),
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %lld\r\n"
        "Cache-Control: max-age=3600\r\n"
        "Connection: close\r\n"
        "\r\n",
        meta.art_mime, (long long)meta.art_len
    );

    if (send_all(conn_fd, header, header_len) == -1) {
        perror("send header");
    } else {
        send_file_range(conn_fd, song_fd, meta.art_offset, meta.art_offset + meta.art_len - 1);
    }

    close(song_fd);
}

// serve one sonde client connection, then close it
void handle_client(int conn_fd, const char* music_dir) {
    char request_buf[REQUEST_BUFFER_SIZE];

    if (read_request(conn_fd, request_buf, sizeof(request_buf)) == -1) {
        close(conn_fd);
        return;
    }

    // first line is e.g. "GET /songs/Fever.mp3 HTTP/1.1"
    char method[8];
    char path[512];

    if (sscanf(request_buf, "%7s %511s", method, path) != 2) {
        send_error(conn_fd, "400 Bad Request");
        close(conn_fd);
        return;
    }

    printf("%s %s\n", method, path);

    if (strcmp(method, "GET") != 0) {
        send_error(conn_fd, "405 Method Not Allowed");
        close(conn_fd);
        return;
    }

    // ignore any ?query on the end
    char* query = strchr(path, '?');
    if (query) {
        *query = '\0';
    }

    if (strcmp(path, "/") == 0) {
        send_response(
            conn_fd, "200 OK", "text/html; charset=utf-8",
            (const char*)index_html, sizeof(index_html)
        );
    } else if (strcmp(path, "/list.txt") == 0) {
        send_song_list(conn_fd, music_dir);
    } else if (strcmp(path, "/api/songs") == 0) {
        send_song_library(conn_fd, music_dir);
    } else if (strncmp(path, SONGS_PREFIX, strlen(SONGS_PREFIX)) == 0) {
        handle_song_request(conn_fd, music_dir, path + strlen(SONGS_PREFIX), request_buf);
    } else if (strncmp(path, ART_PREFIX, strlen(ART_PREFIX)) == 0) {
        handle_art_request(conn_fd, music_dir, path + strlen(ART_PREFIX));
    } else {
        send_error(conn_fd, "404 Not Found");
    }

    close(conn_fd);
}

int main(int argc, char** argv) {
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <music folder>\n", argv[0]);
        return EXIT_FAILURE;
    }

    const char* music_dir = argv[1];
    struct stat music_dir_stat;

    if (stat(music_dir, &music_dir_stat) == -1) {
        perror(music_dir);
        return EXIT_FAILURE;
    }

    if (!S_ISDIR(music_dir_stat.st_mode)) {
        fprintf(stderr, "%s is not a folder\n", music_dir);
        return EXIT_FAILURE;
    }

    signal(SIGPIPE, SIG_IGN);

    // finished client processes clean themselves up instead of becoming zombies
    signal(SIGCHLD, SIG_IGN);

    // print each request line straight away, even when logging to a file
    setvbuf(stdout, NULL, _IOLBF, 0);

    int listen_fd = create_listen_socket(STREAM_PORT);

    if (listen_fd == -1) {
        return EXIT_FAILURE;
    }

    printf("Serving %s on port %d\n", music_dir, STREAM_PORT);

    while (1) {
        // sonde client socket
        int conn_fd = accept(listen_fd, NULL, NULL);
        if (conn_fd == -1) {
            perror("accept");
            continue;
        }

        // one process per client, so a browser holding a song stream open
        // doesn't block everyone else
        pid_t pid = fork();

        if (pid == -1) {
            perror("fork");
            close(conn_fd);
            continue;
        }

        if (pid == 0) {
            close(listen_fd);
            handle_client(conn_fd, music_dir);
            _exit(EXIT_SUCCESS);
        }

        close(conn_fd);
    }
    close(listen_fd);

    return EXIT_SUCCESS;
}
