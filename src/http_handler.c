#include "http_handler.h"

void read_request(struct client_req* req) {

    req->state = STATE_READ;

    char buffer[1024];
    char method[10], path[PATH_MAX], protocol[10];

    int rcvd = recv(req->socket, buffer, sizeof(buffer), 0);
    if(rcvd == -1) {
        req->err_code = errno;
        req->state = STATE_ERROR;
        return;
    }
    if(rcvd == 0) { 
        req->state = STATE_COMPLETE;
        return;
    }

    sscanf(buffer, "%s %s %s", method, path, protocol);

    if (strcmp(method, "GET") != 0 && strcmp(method, "HEAD") != 0) {
        send_error(req->socket, METHOD_NOT_ALLOWED);
        req->state = STATE_COMPLETE;
        return;
    }

    char full_path[PATH_MAX];
    sprintf(full_path, "%s%s", STATIC_ROOT, path);

    // Защита от несанкционированного доступа
    char resolved_path[PATH_MAX];
    if (realpath(full_path, resolved_path) == NULL) {
        send_error(req->socket, NOT_FOUND);
        req->state = STATE_COMPLETE;
        return;
    }
    if (strncmp(resolved_path, STATIC_ROOT, strlen(STATIC_ROOT)) != 0) {
        send_error(req->socket, FORBIDDEN);
        req->state = STATE_COMPLETE;
        return;
    }

    if (strcmp(path, "/") == 0) {
        strcat(full_path, "index.html");
    }
    struct stat path_stat;
    // Если это директория, отправляем список файлов и поддиректорий
    if (stat(full_path, &path_stat) == 0 && S_ISDIR(path_stat.st_mode)) {
        if (path[strlen(path) - 1] != '/') { strcat(path, "/"); } // чек на слеш в конце урла
        send_directory_listing(req->socket, path, full_path);
        req->state = STATE_COMPLETE;
        return;
    }

    int fd = open(full_path, O_RDONLY);
    if (fd == -1) {
        send_error(req->socket, NOT_FOUND);
        req->state = STATE_COMPLETE;
        return;
    }

    flock(fd, LOCK_EX);

    char *filebuff;
    filebuff = malloc(path_stat.st_size);

    int bytesRead = read(fd, filebuff, path_stat.st_size);
    close(fd);

    char headers[1024];
    sprintf(headers, "HTTP/1.1 200 OK\r\nContent-Length: %lld\r\n", path_stat.st_size);

    // Определение Content-Type на основе расширения файла
    const char* content_type = "text/plain";
    const char* file_ext = strrchr(full_path, '.');
    if (file_ext != NULL) {
        if (strcmp(file_ext, ".html") == 0) {
            content_type = "text/html";
        }
        else if (strcmp(file_ext, ".css") == 0) {
            content_type = "text/css";
        }
        else if (strcmp(file_ext, ".js") == 0) {
            content_type = "application/javascript";
        }
        else if (strcmp(file_ext, ".png") == 0) {
            content_type = "image/png";
        }
        else if (strcmp(file_ext, ".jpg") == 0 || strcmp(file_ext, ".jpeg") == 0) {
            content_type = "image/jpeg";
        }
        else if (strcmp(file_ext, ".swf") == 0) {
            content_type = "application/x-shockwave-flash";
        }
        else if (strcmp(file_ext, ".gif") == 0) {
            content_type = "image/gif";
        }
    }

    sprintf(headers + strlen(headers), "Content-Type: %s\r\n\r\n", content_type);

    // Отправка заголовков
    send(req->socket, headers, strlen(headers), 0);

    if (strcmp(method, "HEAD") == 0) {
        close(fd);
        req->state = STATE_COMPLETE;
        flock(fd, LOCK_UN);
        free(filebuff);
        return;
    }

    req->write_buf->data = filebuff;
    req->write_buf->size = bytesRead;
    req->write_buf->bytes_written = 0;
    
    flock(fd, LOCK_UN);

    send_response(req);

    // send_file(req->socket, full_path, method);
}

void send_response(struct client_req* req) {

    req->state = STATE_SEND;

    int len = send(req->socket, req->write_buf->data + req->write_buf->bytes_written, req->write_buf->size, 0);

    if(len == -1) { // Socket Error!!!
        req->err_code = errno;
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
