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
    printf("  Charon Init v1.5 (Full Service Manager)\n");
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
    snprintf(log_msg, sizeof(log_msg), "Spawned %s [PID %d] as user '%s'", 
        services[idx].name, pid, services[idx].user[0] ? services[idx].user : "root");
    log_init(log_msg);
}

/* --- IPC Command & Service Manager Handler --- */
void handle_client_command(int client_fd) {
    char cmd[128] = {0};
    if (recv(client_fd, cmd, sizeof(cmd) - 1, 0) > 0) {
        cmd[strcspn(cmd, "\r\n")] = 0;

        char *action_str = strtok(cmd, " ");
        char *target_service = strtok(NULL, " ");

        if (!action_str) {
            dprintf(client_fd, "Invalid command\n");
            return;
        }

        if (strcmp(action_str, "reboot") == 0 || strcmp(action_str, "6") == 0) {
            dprintf(client_fd, "Rebooting system...\n");
            power_off_sequence(RB_AUTOBOOT);
        } else if (strcmp(action_str, "poweroff") == 0 || strcmp(action_str, "0") == 0) {
            dprintf(client_fd, "Powering off system...\n");
            power_off_sequence(RB_POWER_OFF);
        } else if (strcmp(action_str, "status") == 0) {
            dprintf(client_fd, "CHARON SERVICE STATUS:\n");
            for (int i = 0; i < service_count; i++) {
                dprintf(client_fd, "  [%s] PID: %d | User: %s | Action: %s | Path: %s\n",
                    services[i].name[0] ? services[i].name : "unnamed",
                    services[i].pid,
                    services[i].user[0] ? services[i].user : "root",
                    services[i].action == RESPAWN ? "respawn" : "once",
                    services[i].path);
            }
        } else if (target_service) {
            int found = -1;
            for (int i = 0; i < service_count; i++) {
                if (strcmp(services[i].name, target_service) == 0) {
                    found = i;
                    break;
                }
            }

            if (found == -1) {
                dprintf(client_fd, "Service '%s' not found.\n", target_service);
                return;
            }

            if (strcmp(action_str, "stop") == 0) {
                if (services[found].pid > 0) {
                    services[found].action = ONCE; // Prevent auto-respawn
                    kill(services[found].pid, SIGTERM);
                    dprintf(client_fd, "Stopping service %s (PID %d)...\n", target_service, services[found].pid);
                } else {
                    dprintf(client_fd, "Service %s is not running.\n", target_service);
                }
            } else if (strcmp(action_str, "start") == 0) {
                if (services[found].pid > 0) {
                    dprintf(client_fd, "Service %s is already running (PID %d).\n", target_service, services[found].pid);
                } else {
                    start_service(found);
                    dprintf(client_fd, "Started service %s (PID %d).\n", target_service, services[found].pid);
                }
            } else if (strcmp(action_str, "restart") == 0) {
                if (services[found].pid > 0) {
                    kill(services[found].pid, SIGTERM);
                    usleep(100000);
                }
                start_service(found);
                dprintf(client_fd, "Restarted service %s (New PID %d).\n", target_service, services[found].pid);
            } else {
                dprintf(client_fd, "Unknown action: %s\n", action_str);
            }
        } else {
            dprintf(client_fd, "Unknown or malformed command.\n");
        }
    }
}

/* --- CLI Mode (charonctl, telinit, init) --- */
int handle_cli(int argc, char *argv[], const char *prog_name) {
    if (argc < 2 && strcmp(prog_name, "charonctl") == 0) {
        printf("Usage: charonctl <status|reboot|poweroff|start|stop|restart> [service]\n");
        return 1;
    }

    int sock_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock_fd < 0) {
        perror("Failed to create control socket");
        return 1;
    }

    struct sockaddr_un addr = { .sun_family = AF_UNIX };
    strncpy(addr.sun_path, SOCK_PATH, sizeof(addr.sun_path) - 1);

    if (connect(sock_fd, (struct sockaddr *)&addr, sizeof(struct sockaddr_un)) < 0) {
        perror("Failed to connect to Charon daemon (Is init running?)");
        close(sock_fd);
        return 1;
    }

    char buffer[128] = {0};
    if (strcmp(prog_name, "telinit") == 0 || strcmp(prog_name, "init") == 0) {
        if (argc > 1) strncpy(buffer, argv[1], sizeof(buffer) - 1);
        else {
            fprintf(stderr, "%s: missing runlevel argument\n", prog_name);
            close(sock_fd);
            return 1;
        }
    } else {
        int offset = 0;
        for (int i = 1; i < argc && offset < sizeof(buffer) - 1; i++) {
            offset += snprintf(buffer + offset, sizeof(buffer) - offset, "%s ", argv[i]);
        }
    }

    send(sock_fd, buffer, strlen(buffer), 0);

    char response[1024] = {0};
    int bytes = recv(sock_fd, response, sizeof(response) - 1, 0);
    if (bytes > 0) {
        printf("%s", response);
    }

    close(sock_fd);
    return 0;
}

/* --- Init Daemon Main (PID 1) --- */
int run_init(int argc, char *argv[]) {
    print_banner();

    struct sigaction sa = {0};
    sa.sa_handler = handle_signal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;

    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGPWR, &sa, NULL);

    print_status("Mounting /proc", mount("proc", "/proc", "proc", 0, NULL) == 0);
    print_status("Mounting /sys",  mount("sysfs", "/sys", "sysfs", 0, NULL) == 0);
    print_status("Mounting /dev",  mount("devtmpfs", "/dev", "devtmpfs", 0, NULL) == 0);

    global_sock_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (global_sock_fd >= 0) {
        struct sockaddr_un local = { .sun_family = AF_UNIX };
        strncpy(local.sun_path, SOCK_PATH, sizeof(local.sun_path) - 1);
        unlink(SOCK_PATH);
        
        if (bind(global_sock_fd, (struct sockaddr *)&local, sizeof(struct sockaddr_un)) == 0) {
            listen(global_sock_fd, 5);
            chmod(SOCK_PATH, 0666); // Open socket permissions for unprivileged users
            fcntl(global_sock_fd, F_SETFL, O_NONBLOCK);
            print_status("IPC Control Socket", 1);
        } else {
            print_status("IPC Control Socket", 0);
            close(global_sock_fd);
            global_sock_fd = -1;
        }
    } else {
        print_status("IPC Control Socket", 0);
    }

    parse_config();
    for (int i = 0; i < service_count; i++) {
        start_service(i);
        print_status(services[i].name[0] ? services[i].name : services[i].path, 1);
    }

    printf("\n" CLR_GREEN "Boot complete." CLR_RESET "\n");

    while (running) {
        if (global_sock_fd >= 0) {
            int client_fd = accept(global_sock_fd, NULL, NULL);
            if (client_fd != -1) {
                handle_client_command(client_fd);
                close(client_fd);
            }
        }

        int status;
        pid_t dead_pid;
        while ((dead_pid = waitpid(-1, &status, WNOHANG)) > 0) {
            for (int i = 0; i < service_count; i++) {
                if (services[i].pid == dead_pid) {
                    char log_msg[256];
                    snprintf(log_msg, sizeof(log_msg), "Service %s (PID %d) exited.", services[i].name, dead_pid);
                    log_init(log_msg);
                    
                    if (services[i].action == RESPAWN) {
                        log_init("Respawning service...");
                        sleep(1); 
                        start_service(i);
                    } else {
                        services[i].pid = -1;
                    }
                }
            }
        }
        usleep(200000);
    }

    if (global_sock_fd >= 0) close(global_sock_fd);
    unlink(SOCK_PATH);
    return 0;
}

int main(int argc, char *argv[]) {
    char *arg0_copy = strdup(argv[0]);
    char *prog_name = basename(arg0_copy);

    if (getpid() != 1) {
        if (strcmp(prog_name, "charonctl") == 0 || 
            strcmp(prog_name, "telinit") == 0 || 
            strcmp(prog_name, "init") == 0) {
            int ret = handle_cli(argc, argv, prog_name);
            free(arg0_copy);
            return ret;
        }

        fprintf(stderr, "Error: Charon must run as PID 1, or be invoked via charonctl/telinit.\n");
        free(arg0_copy);
        return 1;
    }

    free(arg0_copy);
    return run_init(argc, argv);
}
