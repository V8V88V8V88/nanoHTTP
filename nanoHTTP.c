#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/types.h>
#include <arpa/inet.h>

#define PORT 8080
#define BUFFER_SIZE 4096

typedef struct {
    int socket;
    struct sockaddr_in address;
} Connection;

void send_response(Connection *conn, const char *content, const char *content_type, int status, const char *status_text) {
    char buffer[BUFFER_SIZE];
    int len = snprintf(buffer, sizeof(buffer), "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %ld\r\n\r\n%s", 
                        status, status_text, content_type, strlen(content), content);
    send(conn->socket, buffer, len, 0);
}

void execute_cgi(Connection *conn, const char *path) {
    char buffer[BUFFER_SIZE];
    FILE *fp = popen(path, "r");
    if (!fp) {
        send_response(conn, "<h1>500 Internal Server Error</h1>", "text/html", 500, "Internal Server Error");
        return;
    }

    size_t n = fread(buffer, 1, sizeof(buffer) - 1, fp);
    buffer[n] = '\0';
    pclose(fp);

    send_response(conn, buffer, "text/html", 200, "OK");
}

void handle_request(Connection *conn) {
    char buffer[BUFFER_SIZE];
    recv(conn->socket, buffer, sizeof(buffer) - 1, 0);
    buffer[sizeof(buffer) - 1] = '\0';
    
    char method[16], path[256];
    sscanf(buffer, "%15s %255s", method, path);

    if (strcmp(path, "/") == 0) {
        strcpy(path, "index.html");
    } else {
        memmove(path, path + 1, strlen(path));
    }

    if (strstr(path, ".cgi")) {
        execute_cgi(conn, path);
    } else {
        FILE *file = fopen(path, "r");
        if (!file) {
            send_response(conn, "<h1>404 Not Found</h1>", "text/html", 404, "Not Found");
            return;
        }

        char content[BUFFER_SIZE];
        size_t n = fread(content, 1, sizeof(content) - 1, file);
        content[n] = '\0';
        fclose(file);

        send_response(conn, content, "text/html", 200, "OK");
    }
}

int main() {
    int server_socket = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in server_address = {0};
    server_address.sin_family = AF_INET;
    server_address.sin_addr.s_addr = INADDR_ANY;
    server_address.sin_port = htons(PORT);

    bind(server_socket, (struct sockaddr *)&server_address, sizeof(server_address));
    listen(server_socket, 10);

    printf("nanoHTTP server running on port number%d...\n", PORT);

    while (1) {
        Connection conn;
        socklen_t addr_len = sizeof(conn.address);
        conn.socket = accept(server_socket, (struct sockaddr *)&conn.address, &addr_len);
        if (conn.socket < 0) continue;

        handle_request(&conn);
        close(conn.socket);
    }

    close(server_socket);
    return 0;
}
