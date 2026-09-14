#include "aesdsocket.h"
#include "sys/queue.h"
#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#define PORT 9000
#define BACKLOG 5
#define BUFFER_SIZE 1024
#define LOG_FILE "/var/tmp/aesdsocketdata"
#define LOG_FILE_MODE (S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH | S_IWOTH)
#define LOG_FILE_FLAGS (O_CREAT | O_WRONLY | O_APPEND)

typedef struct thread_entry {
    pthread_t thread_id;
    int client_socket;
    volatile bool is_active;
    SLIST_ENTRY(thread_entry) next_entry;
} thread_entry_t;

SLIST_HEAD(listhead, thread_entry);
static struct listhead head;

volatile sig_atomic_t exit_flag = 0;
static int server_socket = -1;
static pthread_mutex_t log_file_mutex = PTHREAD_MUTEX_INITIALIZER;

static timer_t timerid;
static struct sigevent sev;
static struct itimerspec its;

static int run_as_daemon = 0;

static int setup_server_socket(void);
static int handle_server_loop(void);
static int handle_storage_and_response(int client_socket);
static int append_to_log_file(const char *data, ssize_t data_len);
static int send_log_file_content(int client_socket);
static int create_threaded_connection(int client_socket);
static void reap_inactive_threads(void);
static void join_all_threads(void);
static void cleanup(void);
static void signal_handler(int signum);
static void daemonize(void);
static void initialize_timer(void);
static void parse_arguments(int argc, char *argv[]);
static void *client_handler(void *arg);

void timer_handler(union sigval sv)
{
    time_t now = time(NULL);
    struct tm *tm_info = localtime(&now);
    char timestamp[100];

    if (tm_info == NULL) {
        return;
    }

    strftime(timestamp, sizeof(timestamp), "timestamp:%a, %d %b %Y %H:%M:%S %z\n", tm_info);

    pthread_mutex_lock(&log_file_mutex);
    FILE *fp = fopen(LOG_FILE, "a");
    if (fp) {
        fputs(timestamp, fp);
        fclose(fp);
    }
    pthread_mutex_unlock(&log_file_mutex);
}

void daemonize(void)
{
    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        exit(EXIT_FAILURE);
    }

    if (pid > 0) {
        exit(EXIT_SUCCESS);
    }

    if (setsid() < 0) {
        perror("setsid");
        exit(EXIT_FAILURE);
    }

    close(STDIN_FILENO);
    close(STDOUT_FILENO);
    close(STDERR_FILENO);

    int fd = open("/dev/null", O_RDWR);
    if (fd >= 0) {
        dup2(fd, STDIN_FILENO);
        dup2(fd, STDOUT_FILENO);
        dup2(fd, STDERR_FILENO);
        if (fd > STDERR_FILENO) {
            close(fd);
        }
    }
}

int main(int argc, char *argv[])
{
    parse_arguments(argc, argv);

    openlog("aesdsocket", LOG_PID | LOG_CONS, LOG_USER);
    unlink(LOG_FILE);

    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    server_socket = setup_server_socket();
    if (server_socket < 0) {
        syslog(LOG_ERR, "Failed to set up server socket");
        exit(EXIT_FAILURE);
    }

    if (run_as_daemon) {
        daemonize();
    }

    initialize_timer();

    SLIST_INIT(&head);
    syslog(LOG_INFO, "Server started on port %d", PORT);

    handle_server_loop();

    cleanup();
    return 0;
}

static void parse_arguments(int argc, char *argv[])
{
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-d") == 0) {
            run_as_daemon = 1;
        }
    }
}

static void initialize_timer(void)
{
    sev.sigev_notify = SIGEV_THREAD;
    sev.sigev_notify_function = timer_handler;
    sev.sigev_notify_attributes = NULL;

    if (timer_create(CLOCK_REALTIME, &sev, &timerid) == -1) {
        syslog(LOG_ERR, "Failed to create timer");
        exit(EXIT_FAILURE);
    }

    its.it_value.tv_sec = 10;
    its.it_value.tv_nsec = 0;
    its.it_interval.tv_sec = 10;
    its.it_interval.tv_nsec = 0;

    if (timer_settime(timerid, 0, &its, NULL) == -1) {
        syslog(LOG_ERR, "Failed to set timer");
        exit(EXIT_FAILURE);
    }
}

static int setup_server_socket(void)
{
    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) {
        return -1;
    }

    int optval = 1;
    setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval));

    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(PORT);

    if (bind(sockfd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        close(sockfd);
        return -1;
    }

    if (listen(sockfd, BACKLOG) < 0) {
        close(sockfd);
        return -1;
    }

    return sockfd;
}

static int handle_server_loop(void)
{
    struct sockaddr_in client_addr;
    socklen_t client_addr_len = sizeof(client_addr);
    char client_ip[INET_ADDRSTRLEN];

    while (!exit_flag) {
        int client_socket = accept(server_socket, (struct sockaddr *)&client_addr, &client_addr_len);
        if (client_socket < 0) {
            if (errno == EINTR) {
                continue;
            }
            syslog(LOG_ERR, "Failed to accept connection: %s", strerror(errno));
            break;
        }

        inet_ntop(AF_INET, &client_addr.sin_addr, client_ip, sizeof(client_ip));
        syslog(LOG_INFO, "Accepted connection from %s", client_ip);

        if (create_threaded_connection(client_socket) != 0) {
            syslog(LOG_ERR, "Failed to create thread for client connection");
            close(client_socket);
            continue;
        }

        reap_inactive_threads();
    }

    syslog(LOG_INFO, "Server is shutting down");
    return 0;
}

static int create_threaded_connection(int client_socket)
{
    thread_entry_t *new_entry = malloc(sizeof(thread_entry_t));
    if (new_entry == NULL) {
        syslog(LOG_ERR, "Failed to allocate memory for thread entry: %s", strerror(errno));
        return -1;
    }

    new_entry->client_socket = client_socket;
    new_entry->is_active = true;

    if (pthread_create(&new_entry->thread_id, NULL, client_handler, new_entry) != 0) {
        syslog(LOG_ERR, "Failed to create thread: %s", strerror(errno));
        free(new_entry);
        return -1;
    }

    SLIST_INSERT_HEAD(&head, new_entry, next_entry);
    return 0;
}

static void reap_inactive_threads(void)
{
    thread_entry_t *entry;
  
    SLIST_FOREACH(entry, &head, next_entry) {
        if (!entry->is_active) {
            pthread_join(entry->thread_id, NULL);
            SLIST_REMOVE(&head, entry, thread_entry, next_entry);
            free(entry);
        }
    }
}

static void join_all_threads(void)
{
    while (!SLIST_EMPTY(&head)) {
        thread_entry_t *entry = SLIST_FIRST(&head);
        pthread_join(entry->thread_id, NULL);
        SLIST_REMOVE_HEAD(&head, next_entry);
        free(entry);
    }
}

static void signal_handler(int signum)
{
    if (signum == SIGINT || signum == SIGTERM) {
        exit_flag = 1;
        if (server_socket >= 0) {
            shutdown(server_socket, SHUT_RDWR);
        }
    }
}

static void cleanup(void)
{
    if (server_socket >= 0) {
        close(server_socket);
    }

    if (timerid != (timer_t)0) {
        timer_delete(timerid);
    }

    join_all_threads();
    pthread_mutex_destroy(&log_file_mutex);
    unlink(LOG_FILE);
    syslog(LOG_INFO, "Server shutting down");
    closelog();
}

void *client_handler(void *arg)
{
    thread_entry_t *entry = (thread_entry_t *)arg;
    int client_socket = entry->client_socket;

    if (handle_storage_and_response(client_socket) != 0) {
        syslog(LOG_ERR, "Failed to process client data");
    }

    entry->is_active = false;
    close(client_socket);
    return NULL;
}

static int handle_storage_and_response(int client_socket)
{
    char *packet = NULL;
    size_t packet_size = 0;
    char buffer[BUFFER_SIZE];
    ssize_t bytes_received;
    bool newline_found = false;

    while (!newline_found && (bytes_received = recv(client_socket, buffer, sizeof(buffer), 0)) > 0) {
        syslog(LOG_INFO, "Received data from client");

        char *new_packet = realloc(packet, packet_size + bytes_received);
        if (new_packet == NULL) {
            syslog(LOG_ERR, "Memory allocation failed: %s", strerror(errno));
            free(packet);
            return -1;
        }

        packet = new_packet;
        memcpy(packet + packet_size, buffer, bytes_received);
        packet_size += bytes_received;

        if (memchr(buffer, '\n', bytes_received) != NULL) {
            newline_found = true;
        }
    }

    if (bytes_received < 0) {
        syslog(LOG_ERR, "Failed to receive data: %s", strerror(errno));
        free(packet);
        return -1;
    }

    if (newline_found) {
        syslog(LOG_INFO, "Newline received; appending and sending log content");

        if (append_to_log_file(packet, packet_size) < 0) {
            free(packet);
            return -1;
        }

        if (send_log_file_content(client_socket) < 0) {
            free(packet);
            return -1;
        }
    }

    free(packet);
    return 0;
}

static int append_to_log_file(const char *data, ssize_t data_len)
{
    pthread_mutex_lock(&log_file_mutex);

    int log_fd = open(LOG_FILE, LOG_FILE_FLAGS, LOG_FILE_MODE);
    if (log_fd < 0) {
        syslog(LOG_ERR, "Failed to open log file for appending: %s", strerror(errno));
        pthread_mutex_unlock(&log_file_mutex);
        return -1;
    }

    if (write(log_fd, data, data_len) < 0) {
        syslog(LOG_ERR, "Failed to write to log file: %s", strerror(errno));
        close(log_fd);
        pthread_mutex_unlock(&log_file_mutex);
        return -1;
    }

    close(log_fd);
    pthread_mutex_unlock(&log_file_mutex);
    return 0;
}

static int send_log_file_content(int client_socket)
{
    pthread_mutex_lock(&log_file_mutex);

    int log_fd = open(LOG_FILE, O_RDONLY);
    if (log_fd < 0) {
        syslog(LOG_ERR, "Failed to open log file for reading: %s", strerror(errno));
        pthread_mutex_unlock(&log_file_mutex);
        return -1;
    }

    char buffer[BUFFER_SIZE];
    ssize_t bytes_read;

    while ((bytes_read = read(log_fd, buffer, sizeof(buffer))) > 0) {
        ssize_t total_sent = 0;

        while (total_sent < bytes_read) {
            ssize_t sent = send(client_socket, buffer + total_sent, bytes_read - total_sent, 0);
            if (sent <= 0) {
                syslog(LOG_ERR, "Failed to send log file content: %s", strerror(errno));
                close(log_fd);
                pthread_mutex_unlock(&log_file_mutex);
                return -1;
            }
            total_sent += sent;
        }
    }

    close(log_fd);
    pthread_mutex_unlock(&log_file_mutex);
    return 0;
}
