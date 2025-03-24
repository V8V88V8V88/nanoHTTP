#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <pthread.h>
#include <dirent.h>
#include <openssl/ssl.h>
#include <openssl/err.h>

#define PORT 8080
#define SSL_PORT 8443
#define BUFFER_SIZE 4096
#define MAX_PATH 1024
#define THREAD_POOL_SIZE 20
#define MAX_CGI_PROCS 10

typedef struct {
    int port;
    int ssl_port;
    char root_dir[MAX_PATH];
    char default_files[5][MAX_PATH];
    size_t max_upload_size;
    int keep_alive_timeout;
    int enable_ssl;
    int directory_listing;
} ServerConfig;

typedef struct {
    int client_socket;
    SSL *ssl_handle;
    int use_ssl;
} Connection;

typedef struct {
    Connection *connections;
    int max_clients;
    int timeout;
} KeepAlivePool;

// Global configuration and SSL context
ServerConfig config;
SSL_CTX *ssl_ctx;
pthread_mutex_t lock;
KeepAlivePool keep_alive;

void init_config() {
    // Load these from config file in real implementation
    config.port = PORT;
    config.ssl_port = SSL_PORT;
    strcpy(config.root_dir, "www");
    strcpy(config.default_files[0], "index.html");
    strcpy(config.default_files[1], "index.htm");
    config.max_upload_size = 1024 * 1024; // 1MB
    config.keep_alive_timeout = 15;
    config.enable_ssl = 0;
    config.directory_listing = 0;
}

void init_ssl() {
    SSL_load_error_strings();
    OpenSSL_add_ssl_algorithms();
    ssl_ctx = SSL_CTX_new(TLS_server_method());
    SSL_CTX_use_certificate_file(ssl_ctx, "cert.pem", SSL_FILETYPE_PEM);
    SSL_CTX_use_PrivateKey_file(ssl_ctx, "key.pem", SSL_FILETYPE_PEM);
}

void log_request(const struct sockaddr_in *addr, const char *method, 
                const char *path, int status, size_t resp_size) {
    time_t now = time(NULL);
    char time_str[64];
    char client_ip[INET_ADDRSTRLEN];
    
    inet_ntop(AF_INET, &addr->sin_addr, client_ip, INET_ADDRSTRLEN);
    strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", localtime(&now));
    
    printf("[%s] %s - %s %s - %d %zu\n",
           time_str, client_ip, method, path, status, resp_size);
}

const char* get_content_type(const char *path) {
    const char *ext = strrchr(path, '.');
    if (!ext) return "text/plain";
    
    static struct {
        const char *ext;
        const char *mime;
    } mime_types[] = {
        {".html",  "text/html"},
        {".css",   "text/css"},
        {".js",    "application/javascript"},
        {".json",  "application/json"},
        {".png",   "image/png"},
        {".jpg",   "image/jpeg"},
        {".jpeg",  "image/jpeg"},
        {".gif",   "image/gif"},
        {".svg",   "image/svg+xml"},
        {".txt",   "text/plain"},
        {".pdf",   "application/pdf"},
        {".zip",   "application/zip"},
        {NULL,     "text/plain"}
    };

    for (int i = 0; mime_types[i].ext; i++) {
        if (strcmp(ext, mime_types[i].ext) == 0)
            return mime_types[i].mime;
    }
    return "text/plain";
}

void url_decode(char *dst, const char *src) {
    char a, b;
    while (*src) {
        if (*src == '%' && (a = src[1]) && (b = src[2]) && isxdigit(a) && isxdigit(b)) {
            if (a >= 'a') a -= 'a'-'A';
            if (a >= 'A') a -= ('A' - 10);
            else a -= '0';
            if (b >= 'a') b -= 'a'-'A';
            if (b >= 'A') b -= ('A' - 10);
            else b -= '0';
            *dst++ = 16*a + b;
            src += 3;
        } else {
            *dst++ = *src++;
        }
    }
    *dst = '\0';
}

void sanitize_path(char *path) {
    char decoded_path[MAX_PATH];
    url_decode(decoded_path, path);
    
    char resolved_path[MAX_PATH];
    realpath(decoded_path, resolved_path);
    
    if (strncmp(resolved_path, config.root_dir, strlen(config.root_dir)) != 0) {
        strcpy(resolved_path, config.root_dir);
        strcat(resolved_path, "/403.html");
    }
    
    strcpy(path, resolved_path);
}

void send_response(Connection *conn, const char *content, const char *content_type, 
                  int status_code, const char *status_msg) {
    char headers[BUFFER_SIZE];
    int len = snprintf(headers, BUFFER_SIZE,
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %zu\r\n"
        "Connection: %s\r\n"
        "X-Content-Type-Options: nosniff\r\n"
        "X-Frame-Options: DENY\r\n"
        "Content-Security-Policy: default-src 'self'\r\n\r\n",
        status_code, status_msg, content_type, strlen(content),
        (config.keep_alive_timeout > 0) ? "keep-alive" : "close");
    
    if (conn->use_ssl) {
        SSL_write(conn->ssl_handle, headers, len);
        SSL_write(conn->ssl_handle, content, strlen(content));
    } else {
        send(conn->client_socket, headers, len, 0);
        send(conn->client_socket, content, strlen(content), 0);
    }
}

void handle_directory_listing(Connection *conn, const char *path) {
    DIR *d = opendir(path);
    if (!d) {
        send_response(conn, "<h1>403 Forbidden</h1>", "text/html", 403, "Forbidden");
        return;
    }

    char listing[BUFFER_SIZE] = "<html><body><ul>";
    struct dirent *dir;
    while ((dir = readdir(d)) != NULL) {
        if (strcmp(dir->d_name, ".") == 0) continue;
        char item[MAX_PATH];
        snprintf(item, MAX_PATH, "<li><a href=\"%s\">%s</a></li>", dir->d_name, dir->d_name);
        strncat(listing, item, BUFFER_SIZE - strlen(listing) - 1);
    }
    strncat(listing, "</ul></body></html>", BUFFER_SIZE - strlen(listing) - 1);
    closedir(d);
    
    send_response(conn, listing, "text/html", 200, "OK");
}

void handle_request(Connection *conn, struct sockaddr_in *client_addr) {
    char buffer[BUFFER_SIZE];
    int bytes_received = conn->use_ssl ?
        SSL_read(conn->ssl_handle, buffer, BUFFER_SIZE - 1) :
        recv(conn->client_socket, buffer, BUFFER_SIZE - 1, 0);

    if (bytes_received < 1) return;
    buffer[bytes_received] = '\0';

    char method[16], path[MAX_PATH], protocol[16];
    sscanf(buffer, "%s %s %s", method, path, protocol);
    sanitize_path(path);

    // Handle different HTTP methods
    if (strcmp(method, "GET") == 0) {
        struct stat st;
        if (stat(path, &st) == -1) {
            send_response(conn, "<h1>404 Not Found</h1>", "text/html", 404, "Not Found");
            log_request(client_addr, method, path, 404, 0);
            return;
        }

        if (S_ISDIR(st.st_mode)) {
            if (config.directory_listing) {
                handle_directory_listing(conn, path);
            } else {
                send_response(conn, "<h1>403 Forbidden</h1>", "text/html", 403, "Forbidden");
            }
        } else {
            FILE *file = fopen(path, "rb");
            fseek(file, 0, SEEK_END);
            long fsize = ftell(file);
            fseek(file, 0, SEEK_SET);

            char *content = malloc(fsize + 1);
            fread(content, fsize, 1, file);
            fclose(file);

            send_response(conn, content, get_content_type(path), 200, "OK");
            free(content);
        }
    } else if (strcmp(method, "POST") == 0) {
        // Handle file uploads
        char *boundary = strstr(buffer, "boundary=");
        if (boundary) {
            // Implement multipart/form-data parsing
            send_response(conn, "{\"status\":\"upload_received\"}", 
                         "application/json", 200, "OK");
        } else {
            send_response(conn, "{\"error\":\"invalid_content\"}", 
                         "application/json", 400, "Bad Request");
        }
    } else {
        send_response(conn, "<h1>501 Not Implemented</h1>", 
                     "text/html", 501, "Not Implemented");
    }

    log_request(client_addr, method, path, 200, strlen(buffer));
}

void *connection_handler(void *arg) {
    Connection *conn = (Connection *)arg;
    struct sockaddr_in client_addr;
    socklen_t addr_len = sizeof(client_addr);
    getpeername(conn->client_socket, (struct sockaddr*)&client_addr, &addr_len);

    if (conn->use_ssl) {
        SSL_set_fd(conn->ssl_handle, conn->client_socket);
        if (SSL_accept(conn->ssl_handle) <= 0) {
            ERR_print_errors_fp(stderr);
            goto cleanup;
        }
    }

    handle_request(conn, &client_addr);

cleanup:
    if (conn->use_ssl) {
        SSL_shutdown(conn->ssl_handle);
        SSL_free(conn->ssl_handle);
    }
    close(conn->client_socket);
    free(conn);
    return NULL;
}

int main() {
    init_config();
    int server_fd, ssl_fd;
    struct sockaddr_in address;
    pthread_t threads[THREAD_POOL_SIZE];

    if (config.enable_ssl) init_ssl();

    // Create both HTTP and HTTPS servers
    int servers[2] = {config.port, config.ssl_port};
    for (int i = 0; i < (config.enable_ssl ? 2 : 1); i++) {
        int sock = socket(AF_INET, SOCK_STREAM, 0);
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = INADDR_ANY;
        address.sin_port = htons(servers[i]);

        setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &(int){1}, sizeof(int));
        if (bind(sock, (struct sockaddr*)&address, sizeof(address)) < 0) {
            perror("Bind failed");
            exit(EXIT_FAILURE);
        }
        listen(sock, 100);
        (i == 0) ? (server_fd = sock) : (ssl_fd = sock);
    }

    printf("Server running on ports %d%s\n", 
           config.port, config.enable_ssl ? " and 443 (SSL)" : "");

    while (1) {
        struct sockaddr_in client_addr;
        socklen_t addr_len = sizeof(client_addr);
        int new_socket = accept(server_fd, (struct sockaddr*)&client_addr, &addr_len);
        
        Connection *conn = malloc(sizeof(Connection));
        conn->client_socket = new_socket;
        conn->use_ssl = 0;
        conn->ssl_handle = NULL;

        pthread_t thread;
        pthread_create(&thread, NULL, connection_handler, conn);
        pthread_detach(thread);
    }

    if (config.enable_ssl) {
        SSL_CTX_free(ssl_ctx);
        EVP_cleanup();
    }
    return 0;
}