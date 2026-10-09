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
#include <sys/stat.h>
#include <errno.h>
#include <libgen.h>
#include <pwd.h>
#include <grp.h>

/* --- Configuration & Constants --- */
#define MAX_SERVICES 15
#define MAX_ARGS     8
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
    char name[32];
    char path[128];
    char user[32];
    char raw_args[256];
    char *argv[MAX_ARGS + 2];
    Action action;
    pid_t pid;
} Service;

Service services[MAX_SERVICES];
int service_count = 0;
int running = 1;
int global_sock_fd = -1;

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
    printf("  Charon Init v1.6 (Full Service Manager)\n");
    printf("  The parent of all processes...\n");
    printf("  ---------------------------------------\n\n" CLR_RESET);
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
    if (global_sock_fd >= 0) close(global_sock_fd);
    unlink(SOCK_PATH);
    reboot(reboot_type);
}

void handle_signal(int sig) {
    if (sig == SIGINT) power_off_sequence(RB_AUTOBOOT);
    if (sig == SIGPWR) power_off_sequence(RB_POWER_OFF);
}

/* --- Config Parser --- */
void parse_config() {
    FILE *fp = fopen(CONF_FILE, "r");
    if (!fp) {
        log_init("No config file found. Using emergency shell.");
        strcpy(services[0].name, "shell");
        strcpy(services[0].path, "/bin/sh");
        services[0].argv[0] = services[0].path;
        services[0].argv[1] = NULL;
        services[0].action = RESPAWN;
        services[0].pid = -1;
        service_count = 1;
        return;
    }

    char line[256];
    int current_idx = -1;

    while (fgets(line, sizeof(line), fp) && service_count < MAX_SERVICES) {
        line[strcspn(line, "\r\n")] = 0;
        
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '#' || *p == '\0') continue;

        if (strcmp(p, "[service]") == 0) {
            current_idx = service_count++;
            memset(&services[current_idx], 0, sizeof(Service));
            services[current_idx].pid = -1;
            continue;
        }

        if (current_idx != -1) {
            char *key = strtok(p, "=");
            char *val = strtok(NULL, "");
            if (key && val) {
                while (*key == ' ') key++;
                char *end = key + strlen(key) - 1;
                while (end > key && (*end == ' ' || *end == '\t')) *end-- = '\0';

                while (*val == ' ' || *val == '\t') val++;
                end = val + strlen(val) - 1;
                while (end > val && (*end == ' ' || *end == '\t')) *end-- = '\0';

                if (strcmp(key, "name") == 0) {
                    strncpy(services[current_idx].name, val, sizeof(services[current_idx].name) - 1);
                } else if (strcmp(key, "path") == 0) {
                    strncpy(services[current_idx].path, val, sizeof(services[current_idx].path) - 1);
                    services[current_idx].argv[0] = services[current_idx].path;
                } else if (strcmp(key, "args") == 0) {
                    strncpy(services[current_idx].raw_args, val, sizeof(services[current_idx].raw_args) - 1);
                    int arg_idx = 1;
                    char *token = strtok(services[current_idx].raw_args, " ");
                    while (token && arg_idx <= MAX_ARGS) {
                        services[current_idx].argv[arg_idx++] = token;
                        token = strtok(NULL, " ");
                    }
                    services[current_idx].argv[arg_idx] = NULL;
                } else if (strcmp(key, "user") == 0) {
                    strncpy(services[current_idx].user, val, sizeof(services[current_idx].user) - 1);
                } else if (strcmp(key, "action") == 0) {
                    services[current_idx].action = (strcmp(val, "respawn") == 0) ? RESPAWN : ONCE;
                }
            }
        }
    }
    fclose(fp);
}

void start_service(int idx) {
    if (!services[idx].argv[0]) {
        services[idx].argv[0] = services[idx].path;
        services[idx].argv[1] = NULL;
    }

    pid_t pid = fork();
    if (pid < 0) {
        log_init("Failed to fork service process");
        return;
    }
    if (pid == 0) {
        int log_fd = open(LOG_FILE, O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (log_fd >= 0) {
            dup2(log_fd, STDOUT_FILENO);
            dup2(log_fd, STDERR_FILENO);
            close(log_fd);
        }

        // Drop privileges securely if a non-root user was specified
        if (services[idx].user[0] != '\0') {
            struct passwd *pw = getpwnam(services[idx].user);
            if (!pw) {
                log_init("Error: Failed to find target user for service");
                _exit(1);
            }
            if (initgroups(services[idx].user, pw->pw_gid) < 0 ||
                setgid(pw->pw_gid) < 0 ||
                setuid(pw->pw_uid) < 0) {
                log_init("Error: Failed to drop privileges");
                _exit(1);
            }
        }

        setsid();
        execv(services[idx].path, services[idx].argv);
        _exit(1);
    }
    services[idx].pid = pid;
    char log_msg[256];
    snprintf(log_msg
