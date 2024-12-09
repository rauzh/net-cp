#include "server.h"
#include "http_handler.h"
#include "logger.h"
#include <sys/socket.h>

volatile sig_atomic_t server_running = true;

static void _handle_sigint_worker(int sig) {
    (void)sig;
    server_running = false;
    log_message(LOG_INFO, "\nSIGINT received. Shutting down worker...\n");
}

static void _handle_sigint_main() {
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

static struct pollfd* __worker_init_poll_fds(struct server*  srv) {
    struct pollfd *poll_fds = calloc((srv->_max_conn), sizeof(struct pollfd));
    poll_fds[0].fd = srv->socket;
    poll_fds[0].events = POLLIN;

    for (int i = 1; i < srv->_max_conn + 1; i++) {
        poll_fds[i].fd = -1;
    }

    return poll_fds;
}

static struct client_req* __worker_init_reqs(struct server* srv) {
    struct client_req* reqs = calloc(srv->_max_conn / srv->_workers_num, sizeof(struct client_req));
    for(int i = 0; i < srv->_max_conn; i++) {
        reqs[i].write_buf = malloc(sizeof(struct write_buffer));
        reqs[i].socket = -1;
    }
    return reqs;
}

static void _worker(struct server* srv, int worker_number) {
    __set_sig_handler_worker();

    struct client_req* reqs = __worker_init_reqs(srv);

    struct pollfd *poll_fds = __worker_init_poll_fds(srv);

    int cur_req_idx = 0;
    int nfds = 1;

    while (server_running) {
        log_message(LOG_DEBUG, "Worker number %d waiting in poll...\n", worker_number);

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

                log_message(LOG_DEBUG, "New connection: socket %d, worker %d\n", client_socket, worker_number);

                // Добавление нового клиента в массив poll_fds и в массив reqss
                if (nfds < srv->_max_conn / srv->_workers_num) {
                    poll_fds[nfds].fd = client_socket;
                    poll_fds[nfds].events = POLLIN | POLLOUT;
                    nfds++;

                    reqs[cur_req_idx].socket = client_socket;
                    reqs[cur_req_idx].state = STATE_CONNECT;

                    while (reqs[cur_req_idx].socket != -1) cur_req_idx = (cur_req_idx + 1) % (srv->_max_conn / srv->_workers_num);
                } else {
                    log_message(LOG_ERROR, "Too many clients, rejecting connection, worker %d\n", worker_number);
                    close(client_socket);
                }
                continue;
            }

            struct client_req* req = NULL; // поиск нужного req в массиве reqs по соответствию сокета
            for (int j = 0; j < srv->_max_conn / srv->_workers_num; j++) {
                if (reqs[j].socket == poll_fds[i].fd) {
                    req = &reqs[j];
                }
            }
            if (req == NULL) {
                continue;
            }

            switch (req->state) { // свитч по состоянию
                case STATE_CONNECT:
                    read_request(req);
                    break;
                case STATE_READ:
                    read_request(req);
                    break;
                case STATE_SEND:
                    send_response(req);
                    break;
            }
            if(req->state == STATE_COMPLETE || req->state == STATE_ERROR) { // закрываем соединение только в случае завершения работы
                close(poll_fds[i].fd);
                poll_fds[i].fd = -1;
                shutdown(req->socket, SHUT_RDWR);
                req->socket = -1;      
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

    free(reqs);
    free(poll_fds);
}

static void _init_workers(struct server* srv, pid_t pids[]) {

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

}

static void _wait_workers(struct server* srv, pid_t pids[]) {

    for (int i = 0; i < srv->_workers_num; ++i) {
        int status;
        waitpid(pids[i], &status, 0); // Ждем завершения конкретного процесса
        if (WIFEXITED(status)) {
            log_message(LOG_DEBUG, "Worker %d exited with status %d\n", pids[i], WEXITSTATUS(status));
        } else if (WIFSIGNALED(status)) {
            log_message(LOG_DEBUG, "Worker %d killed by signal %d\n", pids[i], WTERMSIG(status));
        }
    }

}

static void prefork_serve(struct server* srv) {

    __set_sig_handler_main();

    pid_t *pids = calloc(srv->_workers_num, sizeof(pid_t));

    _init_workers(srv, pids);

    _wait_workers(srv, pids);

    free(pids);
}


struct server* server_init(struct settings* server_settings) {
    struct server* srv = malloc(sizeof(struct server));
    if (srv == NULL) {
        perror("Can't allocate struct server");
        exit(EXIT_FAILURE);
    }

    if ((srv->socket = socket(AF_INET, SOCK_STREAM, 0)) == -1) {
        perror("Can't create socket\n");
        exit(EXIT_FAILURE);
    }

    srv->addr.sin_family = AF_INET;
    srv->addr.sin_addr.s_addr = server_settings->addr;
    srv->addr.sin_port = htons(server_settings->port);

    if (bind(srv->socket, (struct sockaddr*)&srv->addr, sizeof(srv->addr)) == -1) {
        perror("Can't bind\n");
        exit(EXIT_FAILURE);
    }

    srv->_workers_num = server_settings->workers_num;
    srv->_max_conn = server_settings->max_conn;

    log_message(LOG_INFO, "Server with socket %d created.\n", srv->socket);

    return srv;
}

void server_serve(struct server* srv) {

    if (srv == NULL) {
        perror("Can't serve NULL struct server");
        exit(EXIT_FAILURE);
    }

    if (listen(srv->socket, srv->_max_conn) == -1) {
        perror("Can't set socket to listening\n");
        exit(EXIT_FAILURE);
    }

    log_message(LOG_INFO, "Server listening on port %d...\n", PORT);

    prefork_serve(srv);
}

void server_shutdown(struct server* srv) {
    log_message(LOG_INFO, "Shutting down server...\n");

    close(srv->socket);
    free(srv);
}
