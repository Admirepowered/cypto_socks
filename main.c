#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <errno.h>

#define PORT 1080          // SOCKS5代理端口
#define BUFFER_SIZE 1024   // 缓冲区大小

// 处理单个客户端连接
void handle_client(int client_socket) {
    unsigned char buffer[BUFFER_SIZE] = {0};

    // 读取SOCKS5握手请求
    int len = read(client_socket, buffer, BUFFER_SIZE);
    if (len <= 0) {
        perror("read handshake failed");
        close(client_socket);
        return;
    }

    // 检查SOCKS5版本和认证方法
    if (buffer[0] != 0x05) {  // SOCKS5协议版本
        fprintf(stderr, "Invalid SOCKS version: %d\n", buffer[0]);
        close(client_socket);
        return;
    }

    // 响应不需要认证 (NO AUTH)
    unsigned char response[] = {0x05, 0x00};
    send(client_socket, response, sizeof(response), 0);

    // 读取客户端的连接请求
    len = read(client_socket, buffer, BUFFER_SIZE);
    if (len <= 0) {
        perror("read connection request failed");
        close(client_socket);
        return;
    }

    // 解析客户端请求
    if (buffer[1] != 0x01) {  // 仅处理CONNECT请求
        fprintf(stderr, "Only CONNECT requests are supported\n");
        close(client_socket);
        return;
    }

    char target_ip[INET_ADDRSTRLEN] = {0};
    int target_port;
    struct sockaddr_in target_addr;
    memset(&target_addr, 0, sizeof(target_addr));
    target_addr.sin_family = AF_INET;

    // 判断地址类型
    if (buffer[3] == 0x01) {  // IPv4地址类型
        memcpy(&target_addr.sin_addr.s_addr, &buffer[4], 4);
        target_port = (buffer[8] << 8) | buffer[9];
        target_addr.sin_port = htons(target_port);
        inet_ntop(AF_INET, &target_addr.sin_addr, target_ip, INET_ADDRSTRLEN);
    } else if (buffer[3] == 0x03) {  // 域名地址类型
        int domain_len = buffer[4];  // 域名长度
        char domain[256] = {0};      // 最大域名长度为255
        memcpy(domain, &buffer[5], domain_len);
        domain[domain_len] = '\0';   // 添加字符串结束符
        target_port = (buffer[5 + domain_len] << 8) | buffer[6 + domain_len];

        // 解析域名为IP地址
        struct addrinfo hints, *res;
        memset(&hints, 0, sizeof(hints));
        hints.ai_family = AF_INET;   // 仅处理IPv4

        if (getaddrinfo(domain, NULL, &hints, &res) != 0) {
            perror("Failed to resolve domain");
            close(client_socket);
            return;
        }

        struct sockaddr_in *addr_in = (struct sockaddr_in *)res->ai_addr;
        target_addr.sin_addr = addr_in->sin_addr;
        target_addr.sin_port = htons(target_port);
        inet_ntop(AF_INET, &target_addr.sin_addr, target_ip, INET_ADDRSTRLEN);

        freeaddrinfo(res);  // 释放域名解析结果
    } else {
        fprintf(stderr, "Address type not supported\n");
        close(client_socket);
        return;
    }

    printf("Connecting to %s:%d...\n", target_ip, target_port);

    // 与目标服务器建立连接
    int remote_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (connect(remote_socket, (struct sockaddr *)&target_addr, sizeof(target_addr)) < 0) {
        perror("connect to target failed");
        close(client_socket);
        return;
    }

    // 响应客户端连接成功
    unsigned char success_response[] = {0x05, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    send(client_socket, success_response, sizeof(success_response), 0);

    // 数据转发: 客户端<->代理<->目标服务器
    fd_set fds;
    int max_fd = (client_socket > remote_socket) ? client_socket : remote_socket;

    while (1) {
        FD_ZERO(&fds);
        FD_SET(client_socket, &fds);
        FD_SET(remote_socket, &fds);

        if (select(max_fd + 1, &fds, NULL, NULL, NULL) < 0) {
            perror("select failed");
            break;
        }

        // 从客户端读取数据并转发到目标服务器
        if (FD_ISSET(client_socket, &fds)) {
            len = read(client_socket, buffer, BUFFER_SIZE);
            if (len <= 0) break;  // 连接断开
            send(remote_socket, buffer, len, 0);
        }

        // 从目标服务器读取数据并转发到客户端
        if (FD_ISSET(remote_socket, &fds)) {
            len = read(remote_socket, buffer, BUFFER_SIZE);
            if (len <= 0) break;  // 连接断开
            send(client_socket, buffer, len, 0);
        }
    }

    // 关闭socket
    close(remote_socket);
    close(client_socket);
}

int main() {
    int server_fd, client_socket;
    struct sockaddr_in address;
    int opt = 1;
    int addrlen = sizeof(address);

    // 创建服务器socket
    if ((server_fd = socket(AF_INET, SOCK_STREAM, 0)) == 0) {
        perror("socket failed");
        exit(EXIT_FAILURE);
    }

    // 设置端口复用
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt))) {
        perror("setsockopt failed");
        close(server_fd);
        exit(EXIT_FAILURE);
    }

    // 配置监听地址和端口
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(PORT);

    // 绑定socket到地址
    if (bind(server_fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
        perror("bind failed");
        close(server_fd);
        exit(EXIT_FAILURE);
    }

    // 监听端口
    if (listen(server_fd, 3) < 0) {
        perror("listen failed");
        close(server_fd);
        exit(EXIT_FAILURE);
    }
    printf("SOCKS5 proxy server listening on port %d...\n", PORT);

    // 循环接受客户端连接
    while (1) {
        if ((client_socket = accept(server_fd, (struct sockaddr *)&address, (socklen_t *)&addrlen)) < 0) {
            perror("accept failed");
            close(server_fd);
            exit(EXIT_FAILURE);
        }
        printf("New connection accepted.\n");

        // 使用多进程或线程可以并行处理多个客户端
        if (fork() == 0) {
            close(server_fd);  // 子进程关闭监听socket
            handle_client(client_socket);
            exit(0);
        }
        close(client_socket);  // 父进程关闭已处理的客户端socket
    }

    close(server_fd);
    return 0;
}
