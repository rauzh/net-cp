#include "server.h"
#include "logger.h"

volatile sig_atomic_t server_running = true;

static void _handle_sigint_worker(int sig) {
    (void)sig;
    server_running = false;
    log_message(LOG_INFO, "\nSIGINT received. Shutting down worker...\n");
}

static void _handle_sigint_main() {
}

struct server* server_init(struct settings* server_settings) {
    struct server* srv = malloc(sizeof(struct server));
    if (srv == NULL) {
        perror("Can't allocate struct server");
        exit(EXIT_FAILURE);
    }

    // Создание сокета
    if ((srv->socket = socket(AF_INET, SOCK_STREAM, 0)) == -1) {
        perror("Can't create socket\n");
        exit(EXIT_FAILURE);
    }

    srv->addr.sin_family = AF_INET;
    srv->addr.sin_addr.s_addr = server_settings->addr;
    srv->addr.sin_port = htons(server_settings->port);

    // Привязка сокета
    if (bind(srv->socket, (struct sockaddr*)&srv->addr, sizeof(srv->addr)) == -1) {
        perror("Can't bind\n");
        exit(EXIT_FAILURE);
    }

    srv->_workers_num = server_settings->workers_num;
    srv->_max_conn = server_settings->max_conn;

    log_message(LOG_INFO, "Server with socket %d created.\n", srv->socket);

    return srv;
}

static void __set_sig_handler_worker() {
    struct sigaction sa;
    sa.sa_handler = _handle_sigint_worker;
    sa.sa_flags = 0;
    sigemptyset(&sa.sa_mask);
    
    if (sigaction(SIGINT, &sa, NULL) == -1) {
        perror("Error setting worker SIGINT handler");
        exit(EXIT_FAILURE);
    }
}

static void __set_sig_handler_main() {
    struct sigaction sa;
    sa.sa_handler = _handle_sigint_main;
    sa.sa_flags = 0;
    sigemptyset(&sa.sa_mask);
    
    if (sigaction(SIGINT, &sa, NULL) == -1) {
        perror("Error setting worker SIGINT handler");
        exit(EXIT_FAILURE);
    }
}

static void _worker(struct server* srv, int worker_number) {
    __set_sig_handler_worker();

    struct pollfd poll_fds[MAX_CLIENTS + 1]; 
    int nfds = 1;

    poll_fds[0].fd = srv->socket;
    poll_fds[0].events = POLLIN;

    for (int i = 1; i < MAX_CLIENTS + 1; i++) {
        poll_fds[i].fd = -1;
    }

    while (server_running) {
        // log_message(LOG_DEBUG, "Worker number %d waiting in poll...\n", worker_number);

        int rc = poll(poll_fds, nfds, -1);
        if (rc < 0 && server_running) {
            perror("Poll error");
            exit(EXIT_FAILURE);
        }
        if (!server_running) {
            continue;
        }

        for (int i = 0; i < nfds; ++i) {
            if (poll_fds[i].revents == 0) {
                continue;
            }

            // Новый клиент
            if (poll_fds[i].fd == srv->socket) {
                struct sockaddr_in client_addr;
                socklen_t addr_len = sizeof(client_addr);
                int client_socket = accept(srv->socket, (struct sockaddr*)&client_addr, &addr_len);
                if (client_socket < 0) {
                    perror("Accept failed");
                    continue;
                }

                // log_message(LOG_DEBUG, "New connection: socket %d, worker %d\n", client_socket, worker_number);

                // Добавление нового клиента в массив poll_fds
                if (nfds < MAX_CLIENTS + 1) {
                    poll_fds[nfds].fd = client_socket;
                    poll_fds[nfds].events = POLLIN;
                    nfds++;
                } else {
                    log_message(LOG_ERROR, "Too many clients, rejecting connection, worker %d\n", worker_number);
                    close(client_socket);
                }
                continue;
            }

            // Обработка данных клиента
            if (poll_fds[i].revents & POLLIN) {
                int handle_rc = handle_client(poll_fds[i].fd);
                close(poll_fds[i].fd);
                poll_fds[i].fd = -1;
                if (handle_rc < 0) {
                    // log_message(LOG_INBUG, "Connection closed with handle err on socket %d, worker %d\n", poll_fds[i].fd, worker_number);
                }
            }
        }

        // Сжатие массива poll_fds (удаление закрытых дескрипторов)
        for (int i = 0; i < nfds; i++) {
            if (poll_fds[i].fd == -1) {
                for (int j = i; j < nfds - 1; j++) {
                    poll_fds[j] = poll_fds[j + 1];
                }
                nfds--;
                i--;
            }
        }
    }
}

static void _prefork_serve(struct server* srv) {

    __set_sig_handler_main();

    pid_t pids[srv->_workers_num];

    for (int i = 0; i < srv->_workers_num; ++i) {
        pid_t pid = fork();

        if (pid == 0) {
            _worker(srv, i);
            exit(EXIT_SUCCESS);
        } else if (pid > 0) {
            pids[i] = pid;
            log_message(LOG_INFO, "Forked worker %d!\n", pid);
        } else {
            perror("Fork failed\n");
            exit(EXIT_FAILURE);
        }
    }

    for (int i = 0; i < srv->_workers_num; ++i) {
        int status;
        waitpid(pids[i], &status, 0); // Ждем завершения конкретного процесса
        if (WIFEXITED(status)) {
            // log_message(LOG_INBUG, "Worker %d exited with status %d\n", pids[i], WEXITSTATUS(status));
        } else if (WIFSIGNALED(status)) {
            // log_message(LOG_INBUG, "Worker %d killed by signal %d\n", pids[i], WTERMSIG(status));
        }
    }
}

void server_serve(struct server* srv) {

    if (srv == NULL) {
        perror("Can't allocate struct server");
        exit(EXIT_FAILURE);
    }

    if (listen(srv->socket, srv->_max_conn) == -1) {
        perror("Can't set socket to listening\n");
        exit(EXIT_FAILURE);
    }

    log_message(LOG_INFO, "Server listening on port %d...\n", PORT);

    _prefork_serve(srv);
}

void server_shutdown(struct server* srv) {
    log_message(LOG_INFO, "Shutting down server...\n");

    close(srv->socket);
    free(srv);
}
