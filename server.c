#include "deadlock.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#ifdef _WIN32
  #include <winsock2.h>
  #include <ws2tcpip.h>
  typedef int socklen_t;
  #define CLOSE_SOCKET(s) closesocket(s)
#else
  #include <sys/types.h>
  #include <sys/socket.h>
  #include <netinet/in.h>
  #include <arpa/inet.h>
  #include <unistd.h>
  typedef int SOCKET;
  #define INVALID_SOCKET (-1)
  #define SOCKET_ERROR (-1)
  #define CLOSE_SOCKET(s) close(s)
#endif

#define DEFAULT_PORT 8080
#define BUFFER_SIZE 65536

static void send_all(SOCKET sock, const char *data, size_t len) {
    size_t total = 0;
    while (total < len) {
        int sent = send(sock, data + total, (int)(len - total), 0);
        if (sent <= 0) break;
        total += (size_t)sent;
    }
}

static void send_response(SOCKET sock, int status_code, const char *status_text,
                          const char *content_type, const char *body, size_t body_len) {
    char header[512];
    int hlen = snprintf(header, sizeof(header),
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %lu\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n"
        "Access-Control-Allow-Headers: Content-Type\r\n"
        "Connection: close\r\n\r\n",
        status_code, status_text, content_type, (unsigned long)body_len);

    if (hlen > 0) {
        send_all(sock, header, (size_t)hlen);
    }
    if (body && body_len > 0) {
        send_all(sock, body, body_len);
    }
}

static void handle_client(SOCKET client_sock) {
    char *buf = (char *)malloc(BUFFER_SIZE);
    if (!buf) {
        CLOSE_SOCKET(client_sock);
        return;
    }

    size_t total_read = 0;
    int content_length = -1;
    char *body_start = NULL;

    /* Read headers */
    while (total_read < BUFFER_SIZE - 1) {
        int r = recv(client_sock, buf + total_read, (int)(BUFFER_SIZE - 1 - total_read), 0);
        if (r <= 0) break;
        total_read += (size_t)r;
        buf[total_read] = '\0';

        char *hdr_end = strstr(buf, "\r\n\r\n");
        if (hdr_end) {
            body_start = hdr_end + 4;
            break;
        }
        hdr_end = strstr(buf, "\n\n");
        if (hdr_end) {
            body_start = hdr_end + 2;
            break;
        }
    }

    if (!body_start) {
        free(buf);
        CLOSE_SOCKET(client_sock);
        return;
    }

    /* Parse request line */
    char method[16] = {0};
    char path[256] = {0};
    sscanf(buf, "%15s %255s", method, path);

    /* Look for Content-Length */
    char *cl_ptr = strstr(buf, "Content-Length:");
    if (!cl_ptr) {
        cl_ptr = strstr(buf, "content-length:");
    }
    if (cl_ptr) {
        content_length = atoi(cl_ptr + 15);
    }

    /* If there is a body, read remaining body bytes */
    if (content_length > 0) {
        size_t current_body_len = total_read - (size_t)(body_start - buf);
        while ((int)current_body_len < content_length && total_read < BUFFER_SIZE - 1) {
            int to_read = content_length - (int)current_body_len;
            if (to_read > (int)(BUFFER_SIZE - 1 - total_read)) {
                to_read = (int)(BUFFER_SIZE - 1 - total_read);
            }
            int r = recv(client_sock, buf + total_read, to_read, 0);
            if (r <= 0) break;
            total_read += (size_t)r;
            buf[total_read] = '\0';
            current_body_len = total_read - (size_t)(body_start - buf);
        }
    }

    /* Handle CORS preflight OPTIONS */
    if (strcmp(method, "OPTIONS") == 0) {
        send_response(client_sock, 204, "No Content", "text/plain", NULL, 0);
        free(buf);
        CLOSE_SOCKET(client_sock);
        return;
    }

    /* Route: GET / or GET /index.html */
    if (strcmp(method, "GET") == 0 && (strcmp(path, "/") == 0 || strcmp(path, "/index.html") == 0)) {
        FILE *fp = fopen("frontend/index.html", "rb");
        if (!fp) {
            const char *err_msg = "Error: Could not open frontend/index.html.\n"
                                  "Please ensure the server is run from the project root folder (deadlock_recovery/).\n";
            send_response(client_sock, 500, "Internal Server Error", "text/plain; charset=utf-8",
                          err_msg, strlen(err_msg));
        } else {
            fseek(fp, 0, SEEK_END);
            long fsize = ftell(fp);
            fseek(fp, 0, SEEK_SET);

            if (fsize > 0) {
                char *html_data = (char *)malloc((size_t)fsize);
                if (html_data) {
                    size_t read_bytes = fread(html_data, 1, (size_t)fsize, fp);
                    send_response(client_sock, 200, "OK", "text/html; charset=utf-8",
                                  html_data, read_bytes);
                    free(html_data);
                } else {
                    const char *mem_err = "Internal Memory Error\n";
                    send_response(client_sock, 500, "Internal Server Error", "text/plain", mem_err, strlen(mem_err));
                }
            } else {
                send_response(client_sock, 200, "OK", "text/html; charset=utf-8", "", 0);
            }
            fclose(fp);
        }
        free(buf);
        CLOSE_SOCKET(client_sock);
        return;
    }

    /* Route: POST /api/analyze */
    if (strcmp(method, "POST") == 0 && strcmp(path, "/api/analyze") == 0) {
        char *json = analyze_json(body_start);
        size_t jlen = strlen(json);
        send_response(client_sock, 200, "OK", "application/json; charset=utf-8", json, jlen);
        free(json);
        free(buf);
        CLOSE_SOCKET(client_sock);
        return;
    }

    /* 404 Not Found */
    const char *not_found = "404 Not Found";
    send_response(client_sock, 404, "Not Found", "text/plain", not_found, strlen(not_found));
    free(buf);
    CLOSE_SOCKET(client_sock);
}

int main(int argc, char *argv[]) {
    /* CLI Mode: ./deadlock_server --cli < sample.txt */
    if (argc >= 2 && strcmp(argv[1], "--cli") == 0) {
        size_t cap = 4096;
        size_t len = 0;
        char *in_buf = (char *)malloc(cap);
        if (!in_buf) {
            fprintf(stderr, "Memory allocation error\n");
            return 1;
        }

        int c;
        while ((c = getchar()) != EOF) {
            if (len + 2 >= cap) {
                cap *= 2;
                char *new_buf = (char *)realloc(in_buf, cap);
                if (!new_buf) {
                    free(in_buf);
                    fprintf(stderr, "Memory allocation error\n");
                    return 1;
                }
                in_buf = new_buf;
            }
            in_buf[len++] = (char)c;
        }
        in_buf[len] = '\0';

        char *json = analyze_json(in_buf);
        printf("%s\n", json);
        free(json);
        free(in_buf);
        return 0;
    }

    int port = DEFAULT_PORT;
    if (argc >= 2 && argv[1][0] != '-') {
        int custom_port = atoi(argv[1]);
        if (custom_port > 0 && custom_port <= 65535) {
            port = custom_port;
        }
    }

#ifdef _WIN32
    WSADATA wsaData;
    int wsa_err = WSAStartup(MAKEWORD(2, 2), &wsaData);
    if (wsa_err != 0) {
        fprintf(stderr, "WSAStartup failed: %d\n", wsa_err);
        return 1;
    }
#endif

    SOCKET server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd == INVALID_SOCKET) {
        fprintf(stderr, "Failed to create socket\n");
#ifdef _WIN32
        WSACleanup();
#endif
        return 1;
    }

    int opt = 1;
#ifdef _WIN32
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, (const char *)&opt, sizeof(opt));
#else
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
#endif

    struct sockaddr_in address;
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = inet_addr("127.0.0.1");
    address.sin_port = htons((unsigned short)port);

    if (bind(server_fd, (struct sockaddr *)&address, sizeof(address)) == SOCKET_ERROR) {
        fprintf(stderr, "Failed to bind to 127.0.0.1:%d\n", port);
        CLOSE_SOCKET(server_fd);
#ifdef _WIN32
        WSACleanup();
#endif
        return 1;
    }

    if (listen(server_fd, 10) == SOCKET_ERROR) {
        fprintf(stderr, "Failed to listen on socket\n");
        CLOSE_SOCKET(server_fd);
#ifdef _WIN32
        WSACleanup();
#endif
        return 1;
    }

    printf("Deadlock Recovery server running at http://localhost:%d\n", port);
    fflush(stdout);

    while (1) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        SOCKET client_sock = accept(server_fd, (struct sockaddr *)&client_addr, &client_len);
        if (client_sock == INVALID_SOCKET) {
            continue;
        }
        handle_client(client_sock);
    }

    CLOSE_SOCKET(server_fd);
#ifdef _WIN32
    WSACleanup();
#endif
    return 0;
}
