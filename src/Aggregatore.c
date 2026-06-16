#include <stdio.h>       // perror(), printf()
#include <stdlib.h>      // exit(), EXIT_FAILURE
#include <string.h>      // memset(), strlen()
#include <unistd.h>      // close()
#include <sys/socket.h>  // socket(), bind(), listen(), accept(), send(), recv()
#include <netinet/in.h>  // struct sockaddr_in, INADDR_ANY
#include <arpa/inet.h>   // htons(), inet_pton()
// Created by potta on 16/06/2026.
//
int server_fd = socket(AF_INET, SOCK_STREAM, 0);

struct sockaddr_in addr = {
    .sin_family = AF_INET,
    .sin_port = htons(8080),       // porta in ascolto
    .sin_addr.s_addr = INADDR_ANY  // accetta connessioni su qualsiasi interfaccia
};

bind(server_fd, (struct sockaddr*)&addr, sizeof(addr));
listen(server_fd, 10);  // metti in ascolto

int client_fd = accept(server_fd, NULL, NULL);  // accetta un produttore