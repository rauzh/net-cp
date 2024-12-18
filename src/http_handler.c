#include "http_handler.h"

static bool handle_recv_error(struct client_req* req, int rcvd) {
    if (rcvd == -1) {
        req->state = STATE_ERROR;
        return true;
    }
    if (rcvd == 0) {
        req->state = STATE_COMPLETE;
        return true;
    }
    return false;
}

static bool parse_request_line(const char* buffer, char* method, char* path, char* protocol) {
    return sscanf(buffer, "%s %s %s", method, path, protocol) == 3;
}

static const char* get_content_type(const char* full_path) {
    const char* file_ext = strrchr(full_path, '.');
    if (!file_ext) return "text/plain";

    if (strcmp(file_ext, ".html") == 0) return "text/html";
    if (strcmp(file_ext, ".css") == 0) return "text/css";
    if (strcmp(file_ext, ".js") == 0) return "application/javascript";
    if (strcmp(file_ext, ".png") == 0) return "image/png";
    if (strcmp(file_ext, ".jpg") == 0 || strcmp(file_ext, ".jpeg") == 0) return "image/jpeg";
    if (strcmp(file_ext, ".gif") == 0) return "image/gif";
    return "text/plain";
}

static void prepare_headers(char* headers, size_t size, off_t file_size, const char* full_path) {
    sprintf(headers, "HTTP/1.1 200 OK\r\nContent-Length: %lld\r\n", file_size);

    const char* content_type = get_content_type(full_path);
    sprintf(headers + strlen(headers), "Content-Type: %s\r\n\r\n", content_type);
}

static bool handle_file_request(struct client_req* req, const char* method, char* full_path) {
    struct stat path_stat;
    if (stat(full_path, &path_stat) != 0 || !S_ISREG(path_stat.st_mode)) return false;

    int fd = open(full_path, O_RDONLY);
    if (fd == -1) return false;

    flock(fd, LOCK_EX);

    char* filebuff = malloc(path_stat.st_size);
    int bytesRead = read(fd, filebuff, path_stat.st_size);
    close(fd);

    char headers[1024];
    prepare_headers(headers, sizeof(headers), path_stat.st_size, full_path);

    send(req->socket, headers, strlen(headers), 0);

    if (strcmp(method, "HEAD") == 0) {
        req->state = STATE_COMPLETE;
        flock(fd, LOCK_UN);
        free(filebuff);
        return true;
    }

    req->write_buf->data = filebuff;
    req->write_buf->size = bytesRead;
    req->write_buf->bytes_written = 0;

    flock(fd, LOCK_UN);
    send_response(req);
    return true;
}

static void handle_directory_request(struct client_req* req, const char* path, const char* full_path) {
    char adjusted_path[PATH_MAX];
    strcpy(adjusted_path, path);
    if (path[strlen(path) - 1] != '/') {
        strcat(adjusted_path, "/");
    }
    send_directory_listing(req->socket, adjusted_path, full_path);
    req->state = STATE_COMPLETE;
}

static void handle_resource_request(struct client_req* req, const char* method, const char* path, char* full_path) {
    struct stat path_stat;
    if (stat(full_path, &path_stat) == 0 && S_ISDIR(path_stat.st_mode)) {
        handle_directory_request(req, path, full_path);
        return;
    }

    if (!handle_file_request(req, method, full_path)) {
        send_error(req->socket, NOT_FOUND);
        req->state = STATE_COMPLETE;
    }
}

static bool resolve_full_path(struct client_req* req, const char* path, char* full_path) {
    sprintf(full_path, "%s%s", STATIC_ROOT, path);

    char resolved_path[PATH_MAX];
    if (realpath(full_path, resolved_path) == NULL) {
        send_error(req->socket, NOT_FOUND);
        req->state = STATE_COMPLETE;
        return false;
    }
    if (strncmp(resolved_path, STATIC_ROOT, strlen(STATIC_ROOT)) != 0) {
        send_error(req->socket, FORBIDDEN);
        req->state = STATE_COMPLETE;
        return false;
    }
    return true;
}

static bool validate_method(struct client_req* req, const char* method) {
    if (strcmp(method, "GET") != 0 && strcmp(method, "HEAD") != 0) {
        send_error(req->socket, METHOD_NOT_ALLOWED);
        req->state = STATE_COMPLETE;
        return false;
    }
    return true;
}

void read_request(struct client_req* req) {

    char buffer[1024];
    int rcvd = recv(req->socket, buffer, sizeof(buffer), 0);
    if (handle_recv_error(req, rcvd)) return;

    char method[10], path[PATH_MAX], protocol[10];
    if (!parse_request_line(buffer, method, path, protocol)) {
        send_error(req->socket, BAD_REQUEST);
        req->state = STATE_COMPLETE;
        return;
    }

    if (!validate_method(req, method)) return;

    char full_path[PATH_MAX];
    if (!resolve_full_path(req, path, full_path)) return;

    handle_resource_request(req, method, path, full_path);
}

void send_response(struct client_req* req) {

    req->state = STATE_SENDFILE;

    int len = send(req->socket, req->write_buf->data + req->write_buf->bytes_written, req->write_buf->size, 0);

    if(len == -1) { // Socket Error!!!
        if(errno == EAGAIN || errno == EWOULDBLOCK) return;
        req->state = STATE_ERROR;
        free(req->write_buf->data);
        return;
    }
    if(len > 0) {
        req->write_buf->bytes_written += len;
        req->write_buf->size -= len;
        if(req->write_buf->size == 0) {
            req->state = STATE_COMPLETE;
            free(req->write_buf->data);
        }
    }
}

void send_directory_listing(int client_socket, const char* current_path, const char* directory_path) {
    DIR* directory = opendir(directory_path);
    if (directory == NULL) {
        send_error(client_socket, NOT_FOUND);
        return;
    }

    char headers[1024];
    sprintf(headers, "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\n\r\n");
    send(client_socket, headers, strlen(headers), 0);

    char buffer[1024];
    sprintf(buffer, "<html><head><title>Directory %s</title></head><body><h1>Directory %s</h1><ul>", current_path, current_path);

    struct dirent* dir_entry;
    while ((dir_entry = readdir(directory)) != NULL) {
        if (strcmp(dir_entry->d_name, ".") == 0) { continue; }

        char full_entry_path[255];
        sprintf(full_entry_path, "%s/%s", directory_path, dir_entry->d_name);

        struct stat path_stat;
        if (stat(full_entry_path, &path_stat) == 0 && S_ISDIR(path_stat.st_mode)) {
            sprintf(buffer + strlen(buffer), "<li><a href=\"%s%s/\">%s/</a></li>", current_path, dir_entry->d_name, dir_entry->d_name);
        }
        else {
            sprintf(buffer + strlen(buffer), "<li><a href=\"%s%s\">%s</a></li>", current_path, dir_entry->d_name, dir_entry->d_name);
        }
    }

    strcat(buffer, "</ul></body></html>");

    send(client_socket, buffer, strlen(buffer), 0);

    closedir(directory);
}

void send_error(int client_socket, int status_code) {
    char response[1024];
    sprintf(response, "HTTP/1.1 %d\r\nContent-Length: 0\r\nContent-Type: text/html\r\n\r\n", status_code);

    log_message(LOG_INFO, "ERROR %s\n", response);

    send(client_socket, response, strlen(response), 0);
}
