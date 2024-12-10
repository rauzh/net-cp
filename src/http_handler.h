#ifndef __HTTP_HANDLER_H__
#define __HTTP_HANDLER_H__

#include <sys/socket.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <stdbool.h>
#include <stdlib.h>
#include <limits.h>
#include <unistd.h>
#include <dirent.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>

#include "logger.h"

#define MAX_PAYLOAD 1048576

#define BAD_REQUEST             403
#define FORBIDDEN               403
#define NOT_FOUND               404
#define METHOD_NOT_ALLOWED      405

#define STATIC_ROOT             "/Users/rauzh/Desktop/study/net/static"

enum req_state {
    STATE_CONNECTED, STATE_SENDFILE,
    STATE_COMPLETE, STATE_ERROR
};

struct write_buffer {
    char *data;
    uint32_t size;
    uint32_t bytes_written;
};

struct client_req {
    int      socket;
    uint8_t  state;
    struct write_buffer* write_buf;
    int      err_code;
};

void send_directory_listing(int client_socket, const char* current_path, const char* directory_path);
void read_request(struct client_req* req);
void send_response(struct client_req* req);
int handle_client(struct client_req*);
void send_file(int client_socket, const char* file_path, const char* method);
void send_error(int client_socket, int status_code);

#endif // __HTTP_HANDLER_H__
