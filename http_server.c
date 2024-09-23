#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>

#define PORT 8080
#define BUFFER_SIZE 1024

void handle_request(int client_socket) {
    char buffer[BUFFER_SIZE];
    ssize_t bytes_received = recv(client_socket, buffer, BUFFER_SIZE - 1, 0);
    if (bytes_received < 0) {
        perror("Error receiving data");
        return;
    }
    buffer[bytes_received] = '\0';

    char method[10], path[100];
    sscanf(buffer, "%s %s", method, path);

    if (strcmp(method, "GET") == 0) {
        char response[] = "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\n\r\nHello, World!";
        send(client_socket, response, strlen(response), 0);
    } else if (strcmp(method, "POST") == 0) {
        char *body = strstr(buffer, "\r\n\r\n");
        if (body) {
            body += 4;
            char response[BUFFER_SIZE];
            snprintf(response, BUFFER_SIZE, "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\n\r\nReceived POST data: %s", body);
            send(client_socket, response, strlen(response), 0);
        } else {
            char response[] = "HTTP/1.1 400 Bad Request\r\nContent-Type: text/plain\r\n\r\nInvalid POST request";
            send(client_socket, response, strlen(response), 0);
        }
    } else {
        char response[] = "HTTP/1.1 501 Not Implemented\r\nContent-Type: text/plain\r\n\r\nMethod not implemented";
        send(client_socket, response, strlen(response), 0);
    }
}

int main() {
    int server_fd, client_socket;
    struct sockaddr_in address;
    int addrlen = sizeof(address);

    if ((server_fd = socket(AF_INET, SOCK_STREAM, 0)) == 0) {
        perror("Socket failed");
        exit(EXIT_FAILURE);
    }

    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(PORT);

    if (bind(server_fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
        perror("Bind failed");
        exit(EXIT_FAILURE);
    }

    if (listen(server_fd, 3) < 0) {
        perror("Listen failed");
        exit(EXIT_FAILURE);
    }

    printf("Server listening on port %d\n", PORT);

    while (1) {
        if ((client_socket = accept(server_fd, (struct sockaddr *)&address, (socklen_t *)&addrlen)) < 0) {
            perror("Accept failed");
            continue;
        }

        handle_request(client_socket);
        close(client_socket);
    }

    return 0;
}
