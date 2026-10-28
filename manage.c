#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <errno.h>
#include <time.h>
#include <sys/stat.h>

#include "platform.h"
#include "manage.h"
#include "version.h"

#ifdef _WIN32
#include <direct.h>
#else
#include <limits.h>
#include <spawn.h>
#include <sys/wait.h>
#endif

#define PATH_SIZE 4096

// a folder path with a file name added on the end
#define FILE_PATH_SIZE (PATH_SIZE + 64)

#define STREAM_PORT_TEXT "8080"

#ifdef _WIN32
#define ASSET "sonde-windows-x86_64.zip"
#define PACKAGE_DIR "sonde-windows-x86_64"
#define EXE ".exe"
#define NULL_DEVICE "NUL"
#else
#define ASSET "sonde-linux-x86_64.tar.gz"
#define PACKAGE_DIR "sonde-linux-x86_64"
#define EXE ""
#define NULL_DEVICE "/dev/null"
#endif

// ---------- running other programs ----------

#ifdef _WIN32

// add arg to a Windows command line, quoted the way programs split them back up
static void append_arg(char* cmd, size_t size, const char* arg) {
    size_t len = strlen(cmd);
    int quote = arg[0] == '\0' || strpbrk(arg, " \t") != NULL;

    if (len && len < size - 1) {
        cmd[len++] = ' ';
    }
    if (quote && len < size - 1) {
        cmd[len++] = '"';
    }
    for (const char* c = arg; *c && len < size - 3; c++) {
        cmd[len++] = *c;
    }
    // a backslash right before the closing quote would escape it, so double it
    if (quote && len && cmd[len - 1] == '\\' && len < size - 2) {
        cmd[len++] = '\\';
    }
    if (quote && len < size - 1) {
        cmd[len++] = '"';
    }
    cmd[len] = '\0';
}

static void join_args(char* cmd, size_t size, const char* const argv[]) {
    cmd[0] = '\0';
    for (size_t i = 0; argv[i]; i++) {
        append_arg(cmd, size, argv[i]);
    }
}

// run a program and wait for it. quiet hides its output. returns its exit code, -1 if it couldn't start
static int run(const char* const argv[], int quiet) {
    fflush(stdout); // our messages first, then the program's
    char cmd[PATH_SIZE * 2];
    join_args(cmd, sizeof(cmd), argv);

    STARTUPINFOA startup = { .cb = sizeof(startup) };
    PROCESS_INFORMATION process;
    HANDLE null_out = INVALID_HANDLE_VALUE;

    if (quiet) {
        SECURITY_ATTRIBUTES inherit = { sizeof(inherit), NULL, TRUE };
        null_out = CreateFileA("NUL", GENERIC_WRITE, FILE_SHARE_WRITE, &inherit, OPEN_EXISTING, 0, NULL);
        startup.dwFlags = STARTF_USESTDHANDLES;
        startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
        startup.hStdOutput = null_out;
        startup.hStdError = null_out;
    }

    if (!CreateProcessA(NULL, cmd, NULL, NULL, quiet, 0, NULL, NULL, &startup, &process)) {
        if (null_out != INVALID_HANDLE_VALUE) {
            CloseHandle(null_out);
        }
        return -1;
    }

    DWORD code = 1;
    WaitForSingleObject(process.hProcess, INFINITE);
    GetExitCodeProcess(process.hProcess, &code);
    CloseHandle(process.hProcess);
    CloseHandle(process.hThread);
    if (null_out != INVALID_HANDLE_VALUE) {
        CloseHandle(null_out);
    }
    return (int)code;
}

#else

extern char** environ;

static int run(const char* const argv[], int quiet) {
    fflush(stdout); // our messages first, then the program's
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);

    if (quiet) {
        posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, NULL_DEVICE, O_WRONLY, 0);
        posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, NULL_DEVICE, O_WRONLY, 0);
    }

    pid_t pid;
    int err = posix_spawnp(&pid, argv[0], &actions, NULL, (char* const*)argv, environ);
    posix_spawn_file_actions_destroy(&actions);

    if (err != 0) {
        return -1;
    }

    int status;
    if (waitpid(pid, &status, 0) == -1) {
        return -1;
    }
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

#endif

// run a shell command and keep the first line it prints, without the newline. -1 on error
static int run_capture(const char* command, char* out, size_t size) {
    FILE* pipe = popen(command, "r");

    if (pipe == NULL) {
        return -1;
    }

    out[0] = '\0';
    if (fgets(out, (int)size, pipe) == NULL) {
        out[0] = '\0';
    }
    // drain the rest so the program isn't left writing into a closed pipe
    char rest[512];
    while (fgets(rest, sizeof(rest), pipe)) {
    }

    int status = pclose(pipe);
    out[strcspn(out, "\r\n")] = '\0';
    return status == 0 ? 0 : -1;
}

// ---------- paths ----------

// where this sonded binary is, -1 if it can't tell
static int self_path(char* out, size_t size) {
#ifdef _WIN32
    DWORD len = GetModuleFileNameA(NULL, out, (DWORD)size);
    return len == 0 || len >= size ? -1 : 0;
#else
    ssize_t len = readlink("/proc/self/exe", out, size - 1);
    if (len <= 0) {
        return -1;
    }
    out[len] = '\0';
    return 0;
#endif
}

// cut a path down to its folder, in place
static void strip_to_dir(char* path) {
    char* slash = strrchr(path, '/');
#ifdef _WIN32
    char* backslash = strrchr(path, '\\');
    if (backslash && (!slash || backslash > slash)) {
        slash = backslash;
    }
#endif
    if (slash) {
        *slash = '\0';
    }
}

static int absolute_path(const char* path, char* out, size_t size) {
#ifdef _WIN32
    return _fullpath(out, path, size) ? 0 : -1;
#else
    char resolved[PATH_MAX];
    if (realpath(path, resolved) == NULL || strlen(resolved) >= size) {
        return -1;
    }
    strcpy(out, resolved);
    return 0;
#endif
}

static int is_dir(const char* path) {
    struct stat info;
    return stat(path, &info) == 0 && S_ISDIR(info.st_mode);
}

// make path and any missing folders above it
static int make_dirs(const char* path) {
    char partial[PATH_SIZE];

    if (strlen(path) >= sizeof(partial)) {
        return -1;
    }
    strcpy(partial, path);

    for (char* c = partial + 1; *c; c++) {
        if (*c == '/' || *c == '\\') {
            char saved = *c;
            *c = '\0';
            if (!is_dir(partial)) {
#ifdef _WIN32
                _mkdir(partial);
#else
                mkdir(partial, 0755);
#endif
            }
            *c = saved;
        }
    }
    if (!is_dir(partial)) {
#ifdef _WIN32
        return _mkdir(partial);
#else
        return mkdir(partial, 0755);
#endif
    }
    return 0;
}

static int copy_file(const char* from, const char* to) {
    FILE* in = fopen(from, "rb");
    if (in == NULL) {
        return -1;
    }
    FILE* out = fopen(to, "wb");
    if (out == NULL) {
        fclose(in);
        return -1;
    }

    char buf[64 * 1024];
    size_t n;
    int err = 0;

    while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
        if (fwrite(buf, 1, n, out) != n) {
            err = -1;
            break;
        }
    }
    if (ferror(in)) {
        err = -1;
    }
    fclose(in);
    if (fclose(out) != 0) {
        err = -1;
    }
    return err;
}

// ---------- is something already serving ----------

// true if something answers on the sonded port on this machine
static int port_in_use(void) {
    if (net_init() == -1) {
        return 0;
    }
    sock_t fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd == SOCK_INVALID) {
        return 0;
    }

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(atoi(STREAM_PORT_TEXT));
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    int in_use = connect(fd, (struct sockaddr*)&addr, sizeof(addr)) == 0;
    sock_close(fd);
    return in_use;
}

static void wait_ms(int ms) {
#ifdef _WIN32
    Sleep(ms);
#else
    struct timespec pause = { ms / 1000, (ms % 1000) * 1000000L };
    nanosleep(&pause, NULL);
#endif
}

// ---------- the background server ----------

#ifdef _WIN32

#define RUN_KEY "Software\\Microsoft\\Windows\\CurrentVersion\\Run"

// %LOCALAPPDATA%\Sonde, where the background server keeps its log and pid
static int state_dir(char* out, size_t size) {
    const char* base = getenv("LOCALAPPDATA");
    if (base == NULL || snprintf(out, size, "%s\\Sonde", base) >= (int)size) {
        return -1;
    }
    return make_dirs(out);
}

// the pid of the background sonded, 0 if there isn't one
static DWORD read_background_pid(void) {
    char dir[PATH_SIZE];
    char path[FILE_PATH_SIZE];
    if (state_dir(dir, sizeof(dir)) == -1) {
        return 0;
    }
    snprintf(path, sizeof(path), "%s\\sonded.pid", dir);

    FILE* f = fopen(path, "r");
    unsigned long pid = 0;
    if (f) {
        if (fscanf(f, "%lu", &pid) != 1) {
            pid = 0;
        }
        fclose(f);
    }
    return (DWORD)pid;
}

static void write_background_pid(DWORD pid) {
    char dir[PATH_SIZE];
    char path[FILE_PATH_SIZE];
    if (state_dir(dir, sizeof(dir)) == -1) {
        return;
    }
    snprintf(path, sizeof(path), "%s\\sonded.pid", dir);

    FILE* f = fopen(path, "w");
    if (f) {
        fprintf(f, "%lu\n", (unsigned long)pid);
        fclose(f);
    }
}

// stop the background sonded if it's running. checks the pid really is
// a sonded first, since Windows hands old pids out again
static void stop_background(void) {
    DWORD pid = read_background_pid();
    if (pid == 0) {
        return;
    }

    HANDLE process = OpenProcess(PROCESS_TERMINATE | PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, pid);
    if (process) {
        char image[PATH_SIZE];
        DWORD len = sizeof(image);

        if (QueryFullProcessImageNameA(process, 0, image, &len)) {
            const char* name = strrchr(image, '\\');
            name = name ? name + 1 : image;
            if (strncasecmp(name, "sonded", strlen("sonded")) == 0) {
                TerminateProcess(process, 0);
                WaitForSingleObject(process, 3000);
            }
        }
        CloseHandle(process);
    }
    write_background_pid(0);
}

// start sonded for folder with no window, output going to the log
static int start_background(const char* folder) {
    char self[PATH_SIZE];
    char dir[PATH_SIZE];
    char log_path[FILE_PATH_SIZE];

    if (self_path(self, sizeof(self)) == -1 || state_dir(dir, sizeof(dir)) == -1) {
        fprintf(stderr, "Couldn't find where sonded is installed\n");
        return -1;
    }
    snprintf(log_path, sizeof(log_path), "%s\\sonded.log", dir);

    SECURITY_ATTRIBUTES inherit = { sizeof(inherit), NULL, TRUE };
    HANDLE log = CreateFileA(
        log_path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
        &inherit, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL
    );

    STARTUPINFOA startup = { .cb = sizeof(startup) };
    if (log != INVALID_HANDLE_VALUE) {
        startup.dwFlags = STARTF_USESTDHANDLES;
        startup.hStdOutput = log;
        startup.hStdError = log;
    }

    const char* const argv[] = { self, folder, NULL };
    char cmd[PATH_SIZE * 2];
    join_args(cmd, sizeof(cmd), argv);

    PROCESS_INFORMATION process;
    BOOL started = CreateProcessA(
        self, cmd, NULL, NULL, TRUE,
        CREATE_NO_WINDOW | CREATE_NEW_PROCESS_GROUP, NULL, NULL, &startup, &process
    );

    if (log != INVALID_HANDLE_VALUE) {
        CloseHandle(log);
    }
    if (!started) {
        fprintf(stderr, "Couldn't start sonded in the background: error %lu\n", GetLastError());
        return -1;
    }

    write_background_pid(process.dwProcessId);
    CloseHandle(process.hProcess);
    CloseHandle(process.hThread);

    // it exits straight away if it can't use the port or the folder
    wait_ms(1000);
    DWORD code = STILL_ACTIVE;
    HANDLE check = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, process.dwProcessId);
    if (check) {
        GetExitCodeProcess(check, &code);
        CloseHandle(check);
    }
    if (code != STILL_ACTIVE) {
        fprintf(stderr, "sonded stopped straight after starting. See %s\n", log_path);
        return -1;
    }
    return 0;
}

// start at login: Windows runs "sonded start <folder>" which starts it hidden
static int set_autostart(const char* folder) {
    char self[PATH_SIZE];
    if (self_path(self, sizeof(self)) == -1) {
        return -1;
    }

    const char* const argv[] = { self, "start", folder, NULL };
    char cmd[PATH_SIZE * 2];
    join_args(cmd, sizeof(cmd), argv);

    return RegSetKeyValueA(HKEY_CURRENT_USER, RUN_KEY, "Sonde", REG_SZ, cmd, (DWORD)strlen(cmd) + 1) == ERROR_SUCCESS ? 0 : -1;
}

static int background_set_up(void) {
    return RegGetValueA(HKEY_CURRENT_USER, RUN_KEY, "Sonde", RRF_RT_REG_SZ, NULL, NULL, NULL) == ERROR_SUCCESS;
}

// the folder the login entry serves, for restarting after an update
static int autostart_folder(char* out, size_t size) {
    char cmd[PATH_SIZE * 2];
    DWORD len = sizeof(cmd);

    if (RegGetValueA(HKEY_CURRENT_USER, RUN_KEY, "Sonde", RRF_RT_REG_SZ, NULL, cmd, &len) != ERROR_SUCCESS) {
        return -1;
    }
    // it reads: "...\sonded.exe" start "C:\Music", so take the last quoted or bare word
    char* start = strstr(cmd, " start ");
    if (start == NULL) {
        return -1;
    }
    start += strlen(" start ");
    if (*start == '"') {
        start++;
        char* end = strrchr(start, '"');
        if (end) {
            *end = '\0';
        }
        // undo the doubled backslash before the closing quote
        size_t n = strlen(start);
        if (n >= 2 && start[n - 1] == '\\' && start[n - 2] == '\\') {
            start[n - 1] = '\0';
        }
    }
    if (strlen(start) >= size) {
        return -1;
    }
    strcpy(out, start);
    return 0;
}

static int setup_background(const char* folder) {
    stop_background();

    if (port_in_use()) {
        fprintf(stderr, "Another sonded is already running on port " STREAM_PORT_TEXT ". Close its window, then run this again.\n");
        return -1;
    }
    if (start_background(folder) == -1) {
        return -1;
    }
    if (set_autostart(folder) == -1) {
        fprintf(stderr, "Started, but couldn't set it to start when you log in\n");
    }
    printf("sonded is running in the background and starts when you log in\n");
    return 0;
}

static void remove_background(void) {
    stop_background();
    RegDeleteKeyValueA(HKEY_CURRENT_USER, RUN_KEY, "Sonde");
    printf("Stopped the background sonded and removed it from login\n");
}

static void restart_background(void) {
    char folder[PATH_SIZE];

    if (!background_set_up() || autostart_folder(folder, sizeof(folder)) == -1) {
        printf("Restart sonded to use the new version\n");
        return;
    }
    stop_background();
    if (start_background(folder) == 0) {
        printf("Restarted the background sonded\n");
    }
}

void remove_old_binary(void) {
    char self[PATH_SIZE];
    char old[FILE_PATH_SIZE];

    if (self_path(self, sizeof(self)) == -1) {
        return;
    }
    strip_to_dir(self);
    for (size_t i = 0; i < 2; i++) {
        snprintf(old, sizeof(old), "%s\\%s.old.exe", self, i ? "sonde" : "sonded");
        DeleteFileA(old); // fails quietly while the old one is still running
    }
}

#else

// ~/.config/systemd/user/sonded.service
static int unit_path(char* out, size_t size, int make) {
    char dir[PATH_SIZE];
    const char* config = getenv("XDG_CONFIG_HOME");
    const char* home = getenv("HOME");

    if (config && *config) {
        snprintf(dir, sizeof(dir), "%s/systemd/user", config);
    } else if (home) {
        snprintf(dir, sizeof(dir), "%s/.config/systemd/user", home);
    } else {
        return -1;
    }
    if (make && make_dirs(dir) == -1) {
        return -1;
    }
    return snprintf(out, size, "%s/sonded.service", dir) >= (int)size ? -1 : 0;
}

// write arg for an ExecStart line: in quotes, with \, " and % escaped
static void write_unit_arg(FILE* f, const char* arg) {
    fputc('"', f);
    for (const char* c = arg; *c; c++) {
        if (*c == '"' || *c == '\\') {
            fputc('\\', f);
        } else if (*c == '%') {
            fputc('%', f);
        }
        fputc(*c, f);
    }
    fputc('"', f);
}

static int service_active(void) {
    const char* const argv[] = { "systemctl", "--user", "is-active", "--quiet", "sonded", NULL };
    return run(argv, 1) == 0;
}

static int background_set_up(void) {
    const char* const argv[] = { "systemctl", "--user", "is-enabled", "--quiet", "sonded", NULL };
    return run(argv, 1) == 0;
}

static int setup_background(const char* folder) {
    char self[PATH_SIZE];
    char unit[PATH_SIZE];

    if (self_path(self, sizeof(self)) == -1 || unit_path(unit, sizeof(unit), 1) == -1) {
        fprintf(stderr, "Couldn't work out where to put the service\n");
        return -1;
    }

    // a sonded started by hand would hold the port and the service couldn't start
    if (port_in_use() && !service_active()) {
        fprintf(stderr, "Another sonded is already running on port " STREAM_PORT_TEXT ". Stop it (Ctrl+C in its terminal), then run this again.\n");
        return -1;
    }

    FILE* f = fopen(unit, "w");
    if (f == NULL) {
        perror(unit);
        return -1;
    }
    fprintf(f,
        "# written by sonded ts, undo with sonded ts off\n"
        "[Unit]\n"
        "Description=Sonde music server\n"
        "After=network-online.target\n"
        "\n"
        "[Service]\n"
        "ExecStart=");
    write_unit_arg(f, self);
    fputc(' ', f);
    write_unit_arg(f, folder);
    fprintf(f,
        "\n"
        "Restart=on-failure\n"
        "\n"
        "[Install]\n"
        "WantedBy=default.target\n");
    if (fclose(f) != 0) {
        perror(unit);
        return -1;
    }

    const char* const reload[] = { "systemctl", "--user", "daemon-reload", NULL };
    const char* const enable[] = { "systemctl", "--user", "enable", "--quiet", "sonded", NULL };
    const char* const restart[] = { "systemctl", "--user", "restart", "sonded", NULL };

    if (run(reload, 0) != 0 || run(enable, 0) != 0 || run(restart, 0) != 0) {
        fprintf(stderr, "Couldn't start the sonded service\n");
        return -1;
    }

    wait_ms(1000);
    if (!service_active()) {
        fprintf(stderr, "The sonded service stopped straight after starting. See: journalctl --user -u sonded\n");
        return -1;
    }

    // keep the service running while you're logged out, and start it at boot
    const char* const linger[] = { "loginctl", "enable-linger", NULL };
    if (run(linger, 1) != 0) {
        printf("To keep it running when you're logged out, run: sudo loginctl enable-linger %s\n", getenv("USER") ? getenv("USER") : "$USER");
    }

    printf("sonded is running in the background as a service (journalctl --user -u sonded for its log)\n");
    return 0;
}

static void remove_background(void) {
    char unit[PATH_SIZE];
    const char* const disable[] = { "systemctl", "--user", "disable", "--now", "--quiet", "sonded", NULL };
    const char* const reload[] = { "systemctl", "--user", "daemon-reload", NULL };

    run(disable, 1);
    if (unit_path(unit, sizeof(unit), 0) == 0) {
        remove(unit);
    }
    run(reload, 1);
    printf("Stopped the sonded service and removed it\n");
}

static void restart_background(void) {
    if (!background_set_up()) {
        printf("Restart sonded to use the new version\n");
        return;
    }
    const char* const restart[] = { "systemctl", "--user", "restart", "sonded", NULL };
    if (run(restart, 0) == 0) {
        printf("Restarted the sonded service\n");
    }
}

void remove_old_binary(void) {
}

#endif

// ---------- sonded ts ----------

// the machine's name on the tailnet, like archlinux.tail20f949.ts.net. -1 if unknown
static int tailnet_name(char* out, size_t size) {
    FILE* pipe = popen("tailscale status --json", "r");
    if (pipe == NULL) {
        return -1;
    }

    // only the "Self" part is about this machine, the peers come after it
    char line[1024];
    int in_self = 0;
    int found = -1;

    while (fgets(line, sizeof(line), pipe)) {
        if (strstr(line, "\"Self\"")) {
            in_self = 1;
        }
        const char* key = strstr(line, "\"DNSName\": \"");
        if (in_self && key && found == -1) {
            key += strlen("\"DNSName\": \"");
            size_t len = strcspn(key, "\"");
            if (len && key[len - 1] == '.') {
                len--; // drop the root dot
            }
            if (len < size) {
                memcpy(out, key, len);
                out[len] = '\0';
                found = 0;
            }
        }
    }
    pclose(pipe);
    return found;
}

static int command_ts(const char* folder_arg) {
    char folder[PATH_SIZE];

    if (absolute_path(folder_arg, folder, sizeof(folder)) == -1 || !is_dir(folder)) {
        fprintf(stderr, "%s is not a folder\n", folder_arg);
        return EXIT_FAILURE;
    }

    const char* const version[] = { "tailscale", "version", NULL };
    if (run(version, 1) != 0) {
        fprintf(stderr, "Tailscale isn't installed, or isn't on the PATH. Get it from https://tailscale.com/download\n");
        return EXIT_FAILURE;
    }

    if (setup_background(folder) == -1) {
        return EXIT_FAILURE;
    }

    // share the port on the tailnet over https. --bg keeps it after reboots
    const char* const serve[] = { "tailscale", "serve", "--bg", STREAM_PORT_TEXT, NULL };
    if (run(serve, 1) != 0) {
        fprintf(stderr, "Couldn't set up tailscale serve.\n");
#ifdef _WIN32
        fprintf(stderr, "Run this from a Command Prompt opened as administrator: tailscale serve --bg " STREAM_PORT_TEXT "\n");
#else
        fprintf(stderr, "Let your user manage Tailscale once with: sudo tailscale set --operator=$USER\nthen run sonded ts again.\n");
#endif
        return EXIT_FAILURE;
    }

    char name[256];
    if (tailnet_name(name, sizeof(name)) == 0) {
        printf("Open https://%s on any device on your tailnet\n", name);
    } else {
        printf("Shared on your tailnet. tailscale serve status shows the address\n");
    }
    return EXIT_SUCCESS;
}

static int command_ts_off(void) {
    remove_background();

    const char* const serve_off[] = { "tailscale", "serve", "--https=443", "off", NULL };
    if (run(serve_off, 1) == 0) {
        printf("Stopped sharing on the tailnet\n");
    }
    return EXIT_SUCCESS;
}

// ---------- sonded update ----------

// the newest release tag, like "v0.2.0", from where GitHub's latest link points
static int latest_tag(char* out, size_t size) {
    char url[512];
    const char* command =
        "curl -fsS -o " NULL_DEVICE " -w \"%{redirect_url}\" https://github.com/" SONDE_REPO "/releases/latest";

    if (run_capture(command, url, sizeof(url)) == -1) {
        return -1;
    }
    const char* tag = strstr(url, "/tag/");
    if (tag == NULL || strlen(tag + 5) >= size || tag[5] == '\0') {
        return -1;
    }
    strcpy(out, tag + 5);
    return 0;
}

// a folder of its own under the system temp folder
static int make_temp_dir(char* out, size_t size) {
#ifdef _WIN32
    char base[MAX_PATH];
    if (GetTempPathA(sizeof(base), base) == 0) {
        return -1;
    }
    snprintf(out, size, "%ssonde-update-%lu", base, GetCurrentProcessId());
    return make_dirs(out);
#else
    const char* base = getenv("TMPDIR");
    if (snprintf(out, size, "%s/sonde-update-XXXXXX", base && *base ? base : "/tmp") >= (int)size) {
        return -1;
    }
    return mkdtemp(out) ? 0 : -1;
#endif
}

static void remove_temp_dir(const char* dir) {
#ifdef _WIN32
    const char* const argv[] = { "cmd", "/c", "rmdir", "/s", "/q", dir, NULL };
#else
    const char* const argv[] = { "rm", "-rf", dir, NULL };
#endif
    run(argv, 1);
}

// put the new copy of name over the one in install_dir, even while it's running
static int install_binary(const char* package, const char* install_dir, const char* name) {
    char from[FILE_PATH_SIZE + 64];
    char to[FILE_PATH_SIZE];
    char staged[FILE_PATH_SIZE];

    snprintf(from, sizeof(from), "%s/%s" EXE, package, name);
    snprintf(to, sizeof(to), "%s/%s" EXE, install_dir, name);
    snprintf(staged, sizeof(staged), "%s/%s.new" EXE, install_dir, name);

    // copy next to the old one first, so the swap below is a rename on one disk
    if (copy_file(from, staged) == -1) {
        fprintf(stderr, "Couldn't write %s: %s\n", staged, strerror(errno));
        return -1;
    }

#ifdef _WIN32
    // Windows won't replace a running .exe, but will rename it out of the way
    char old[FILE_PATH_SIZE];
    snprintf(old, sizeof(old), "%s/%s.old" EXE, install_dir, name);
    DeleteFileA(old);
    MoveFileExA(to, old, MOVEFILE_REPLACE_EXISTING);
    if (!MoveFileExA(staged, to, MOVEFILE_REPLACE_EXISTING)) {
        fprintf(stderr, "Couldn't replace %s: error %lu\n", to, GetLastError());
        MoveFileExA(old, to, MOVEFILE_REPLACE_EXISTING);
        return -1;
    }
#else
    // a running program keeps its old file open, so renaming over it is safe
    chmod(staged, 0755);
    if (rename(staged, to) == -1) {
        fprintf(stderr, "Couldn't replace %s: %s\n", to, strerror(errno));
        remove(staged);
        return -1;
    }
#endif
    return 0;
}

// compare versions like "0.10.2" number by number. <0, 0 or >0 like strcmp
static int compare_versions(const char* a, const char* b) {
    while (*a || *b) {
        long x = strtol(a, (char**)&a, 10);
        long y = strtol(b, (char**)&b, 10);

        if (x != y) {
            return x < y ? -1 : 1;
        }
        if (*a == '.') {
            a++;
        }
        if (*b == '.') {
            b++;
        }
        // anything else, like "-dev", ends the comparison
        if ((*a && (*a < '0' || *a > '9')) || (*b && (*b < '0' || *b > '9'))) {
            break;
        }
    }
    return 0;
}

static int command_update(void) {
#if !defined(__x86_64__) && !defined(_M_X64)
    fprintf(stderr, "Releases are only built for x86-64. Build this one from source with make.\n");
    return EXIT_FAILURE;
#endif

    char tag[64];
    printf("Sonde %s, checking for a newer release\n", SONDE_VERSION);

    if (latest_tag(tag, sizeof(tag)) == -1) {
        fprintf(stderr, "Couldn't reach GitHub. Check the internet connection, and that curl is installed.\n");
        return EXIT_FAILURE;
    }

    const char* latest = tag[0] == 'v' ? tag + 1 : tag;
    int order = compare_versions(latest, SONDE_VERSION);

    if (order == 0) {
        printf("Already up to date\n");
        return EXIT_SUCCESS;
    }
    if (order < 0) {
        printf("This is newer than the latest release (%s), nothing to do\n", latest);
        return EXIT_SUCCESS;
    }

    char install_dir[PATH_SIZE];
    char temp[PATH_SIZE];
    char archive[FILE_PATH_SIZE];
    char package[FILE_PATH_SIZE];
    char url[512];

    if (self_path(install_dir, sizeof(install_dir)) == -1 || make_temp_dir(temp, sizeof(temp)) == -1) {
        fprintf(stderr, "Couldn't set up the download\n");
        return EXIT_FAILURE;
    }
    strip_to_dir(install_dir);
    snprintf(archive, sizeof(archive), "%s/" ASSET, temp);
    snprintf(package, sizeof(package), "%s/" PACKAGE_DIR, temp);
    snprintf(url, sizeof(url), "https://github.com/" SONDE_REPO "/releases/download/%s/" ASSET, tag);

    printf("Downloading Sonde %s\n", latest);

    const char* const download[] = { "curl", "-fL", "--progress-bar", "-o", archive, url, NULL };
#ifdef _WIN32
    const char* const unpack[] = { "tar", "-xf", archive, "-C", temp, NULL };
#else
    const char* const unpack[] = { "tar", "-xzf", archive, "-C", temp, NULL };
#endif

    if (run(download, 0) != 0) {
        fprintf(stderr, "Download failed\n");
        remove_temp_dir(temp);
        return EXIT_FAILURE;
    }
    if (run(unpack, 1) != 0) {
        fprintf(stderr, "Couldn't unpack %s\n", archive);
        remove_temp_dir(temp);
        return EXIT_FAILURE;
    }

    int err = install_binary(package, install_dir, "sonded");
    if (err == 0) {
        install_binary(package, install_dir, "sonde");
    }
    remove_temp_dir(temp);

    if (err) {
#ifndef _WIN32
        fprintf(stderr, "If sonded is in a system folder, run the update with sudo.\n");
#endif
        return EXIT_FAILURE;
    }

    printf("Updated to Sonde %s in %s\n", latest, install_dir);
    restart_background();
    return EXIT_SUCCESS;
}

// ---------- dispatch ----------

int manage_command(int argc, char** argv) {
    const char* command = argv[1];

    if (strcmp(command, "version") == 0 || strcmp(command, "--version") == 0) {
        printf("Sonde %s\n", SONDE_VERSION);
        return EXIT_SUCCESS;
    }
    if (strcmp(command, "update") == 0 && argc == 2) {
        return command_update();
    }
    if (strcmp(command, "ts") == 0) {
        if (argc == 3 && strcmp(argv[2], "off") == 0) {
            return command_ts_off();
        }
        if (argc == 3) {
            return command_ts(argv[2]);
        }
        fprintf(stderr, "Usage: %s ts <music folder>   or   %s ts off\n", argv[0], argv[0]);
        return EXIT_FAILURE;
    }
#ifdef _WIN32
    // what Windows runs at login after sonded ts: start hidden and return
    if (strcmp(command, "start") == 0 && argc == 3) {
        return start_background(argv[2]) == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
    }
#endif
    return -1;
}
