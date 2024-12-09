#ifndef __SERVER_H__
#define __SERVER_H__

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <stdbool.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>

#include "http_handler.h"
#include "logger.h"

#define PORT            13337
#define MAX_CLIENTS     16384
#define WORKERS_NUM     8

#define _CHILD_PROCESS_PID 0

struct settings {
    uint32_t addr;
    uint16_t port;
    uint16_t workers_num;
    uint16_t max_conn;
};

struct server {
    int socket;
    struct sockaddr_in addr;
    uint8_t _workers_num;
    uint16_t _max_conn;
};

void server_serve(struct server* srv);
void server_shutdown(struct server* srv);
struct server* server_init(struct settings* server_settings);

#endif // __SERVER_H__
