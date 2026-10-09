#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <sys/mount.h>
#include <sys/reboot.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/ioctl.h>
#include <errno.h>

/* --- Configuration & Constants --- */
#define MAX_SERVICES 15
#define CONF_FILE    "/etc/charon.conf"
#define SOCK_PATH    "/run/charon.sock"
#define LOG_FILE     "/var/log/charon.log"

// ANSI Colors
#define CLR_RESET    "\x1b[0m"
#define CLR_GREEN    "\x1b[32m"
#define CLR_RED      "\x1b[31m"
#define CLR_CYAN     "\x1b[36m"
#define CLR_BOLD     "\x1b[1m"

typedef enum { ONCE, RESPAWN } Action;

typedef struct {
    char path[128];
    Action action;
    pid_t pid;
} Service;

Service services[MAX_SERVICES];
int service_count = 0;
int running = 1;

/* --- Logging & UI Helpers --- */
void log_init(const char *msg) {
    int fd = open(LOG_FILE, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd >= 0) {
        dprintf(fd, "[INIT] %s\n", msg);
        close(fd);
    }
}

void print_status(const char* task, int success) {
    printf(" " CLR_BOLD "[ * ]" CLR_RESET " %-35s", task);
    if (success) printf("[" CLR_GREEN "  OK  " CLR_RESET "]\n");
    else printf("[" CLR_RED " FAIL " CLR_RESET "]\n");
}

void print_banner() {
    printf(CLR_CYAN CLR_BOLD "\n");
    printf("  Charon Init v1.0\n");
    printf("  The parent of all processes...\n");
    printf("  -----------------------------\n\n" CLR_RESET);
}

/* --- System Management --- */
void power_off_sequence(int reboot_type) {
    running = 0;
    printf("\n" CLR_BOLD "Shutting down system..." CLR_RESET "\n");
    kill(-1, SIGTERM);
    sleep(3);
    kill(-1, SIGKILL);
    sync();
    mount(NULL, "/", NULL, MS_REMOUNT | MS_RDONLY, NULL);
    reboot(reboot_type);
}

void handle_signal(int sig) {
    if (sig == SIGINT) power_off_sequence(RB_AUTOBOOT);
    if (sig == SIGPWR) power_off_sequence(RB_POWER_OFF);
}

/* --- Core Logic --- */
void parse_config() {
    FILE *fp = fopen(CONF_FILE, "r");
    if (!fp) {
        log_init("No config file found. Using emergency shell.");
        strncpy(services[0].path, "/bin/sh", 128);
        services[0].action = RESPAWN;
        service_count = 1;
        return;
    }

    char line[256];
    while (fgets(line, sizeof(line), fp) && service_count < MAX_SERVICES) {
        if (line[0] == '#' || line[0] == '\n') continue;
        char *action_str = strtok(line, ":");
        char *path_str = strtok(NULL, "\n");
        if (action_str && path_str) {
            strncpy(services[service_count].path, path_str, 128);
            services[service_count].action = (strcmp(action_str, "respawn") == 0) ? RESPAWN : ONCE;
            services[service_count].pid = -1;
            service_count++;
        }
    }
    fclose(fp);
}

void start_service(int idx) {
    pid_t pid = fork();
    if (pid == 0) {
        // Redirect child output to log
        int log_fd = open(LOG_FILE, O_WRONLY | O_CREAT | O_APPEND, 0644);
        dup2(log_fd, STDOUT_FILENO);
        dup2(log_fd, STDERR_FILENO);
        close(log_fd);

        setsid(); // New session
        execl(services[idx].path, services[idx].path, NULL);
        _exit(1);
    }
    services[idx].pid = pid;
    char log_msg[256];
    snprintf(log_msg, sizeof(log_msg), "Spawned %s (PID %d)", services[idx].path, pid);
    log_init(log_msg);
}

int main() {
    if (getpid() != 1) {
        fprintf(stderr, "Error: This must run as PID 1 (Init).\n");
        return 1;
    }

    print_banner();

    // 1. Signals & FS Setup
    signal(SIGINT, handle_signal);
    signal(SIGPWR, handle_signal);

    print_status("Mounting /proc", mount("proc", "/proc", "proc", 0, NULL) == 0);
    print_status("Mounting /sys",  mount("sysfs", "/sys", "sysfs", 0, NULL) == 0);
    print_status("Mounting /dev",  mount("devtmpfs", "/dev", "devtmpfs", 0, NULL) == 0);

    // 2. IPC Socket
    int sock_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un local = { .sun_family = AF_UNIX };
    strcpy(local.sun_path, SOCK_PATH);
    unlink(SOCK_PATH);
    bind(sock_fd, (struct sockaddr *)&local, strlen(local.sun_path) + sizeof(local.sun_family));
    listen(sock_fd, 5);
    fcntl(sock_fd, F_SETFL, O_NONBLOCK);
    print_status("IPC Control Socket", 1);

    // 3. Start Services
    parse_config();
    for (int i = 0; i < service_count; i++) {
        start_service(i);
        print_status(services[i].path, 1);
    }

    printf("\n" CLR_GREEN "Boot complete." CLR_RESET "\n");

    // 4. Main Event Loop
    while (running) {
        // Check Socket for commands
        int client_fd = accept(sock_fd, NULL, NULL);
        if (client_fd != -1) {
            char cmd[64] = {0};
            if (recv(client_fd, cmd, sizeof(cmd), 0) > 0) {
                if (strncmp(cmd, "reboot", 6) == 0) power_off_sequence(RB_AUTOBOOT);
            }
            close(client_fd);
        }

        // Reap children
        int status;
        pid_t dead_pid = waitpid(-1, &status, WNOHANG);
        if (dead_pid > 0) {
            for (int i = 0; i < service_count; i++) {
                if (services[i].pid == dead_pid && services[i].action == RESPAWN) {
                    log_init("Service died, respawning...");
                    sleep(1); 
                    start_service(i);
                }
            }
        }
        usleep(200000); // 200ms tick rate
    }

    return 0;
}
