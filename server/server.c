#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/time.h>
#include <errno.h>

#define TIMEOUT_SECONDS 5
#define PORT 17017
#define MAX_BUFFER 516
#define RRQ 1
#define WRQ 2
#define DATA 3
#define ACK 4
#define ERROR 5

void send_packet(int sock, struct sockaddr_in client, int addr_len, unsigned char* buffer, int buffer_len) {
    sendto(sock, buffer, buffer_len, 0, (struct sockaddr*)&client, addr_len);
}

int receive_packet(int sock, unsigned char* buffer, struct sockaddr_in* client, int* addr_len) {
    return recvfrom(sock, buffer, MAX_BUFFER, 0, (struct sockaddr*)client, addr_len);
}

void handle_error(int sock, struct sockaddr_in client, int addr_len) {
    unsigned char buffer[MAX_BUFFER];
    buffer[0] = 0;
    buffer[1] = ERROR;
    send_packet(sock, client, addr_len, buffer, 4);
}

void write_file(int sock, struct sockaddr_in client, int addr_len, char* filename) {
    unsigned char buffer[MAX_BUFFER];
    unsigned int block_number = 0;
    FILE* file = fopen(filename, "wb");
    if(file == NULL) {
        handle_error(sock, client, addr_len);
        return;
    }
    while(1) {
        buffer[0] = 0;
        buffer[1] = ACK;
        buffer[2] = (block_number >> 8) & 0xFF;
        buffer[3] = block_number & 0xFF;
        send_packet(sock, client, addr_len, buffer, 4);
        int recv_len = receive_packet(sock, buffer, &client, &addr_len);
        if(buffer[1] != DATA) {
            handle_error(sock, client, addr_len);
            break;
        }
        unsigned int data_block_number = (buffer[2] << 8) + buffer[3];
        if (data_block_number != (block_number+1)) {
            handle_error(sock, client, addr_len);
            break;
        }
        int data_len = recv_len - 4;
        fwrite(buffer + 4, 1, data_len, file);
        block_number++;
        memset(buffer, 0, sizeof(buffer));
        if (data_len < 512) {
            break;
        }
    }
    fclose(file);
}

void send_file(int sock, struct sockaddr_in client, int addr_len, char* filename) {
    unsigned char buffer[MAX_BUFFER];
    struct timeval timeout;
    timeout.tv_sec = TIMEOUT_SECONDS;
    timeout.tv_usec = 0;
    if (setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (char *)&timeout, sizeof(timeout)) < 0) {
        perror("setsockopt failed\n");
    }
    FILE* file = fopen(filename, "rb");
    if(file == NULL) {
        perror("Failed to open file\n");
        return;
    }
    unsigned int block_number = 1;
    int bytes_read;
    while(1) {
        buffer[0] = 0;
        buffer[1] = DATA;
        buffer[2] = (block_number >> 8) & 0xFF;
        buffer[3] = block_number & 0xFF;
        bytes_read = fread(buffer + 4, 1, MAX_BUFFER - 4, file);
        if (bytes_read < 0) {
            perror("Failed to read file\n");
            break;
        }
        send_packet(sock, client, addr_len, buffer, bytes_read + 4);
        if (bytes_read < MAX_BUFFER - 4) {
            break;
        }
        while(1) {
            int recv_len = receive_packet(sock, buffer, &client, &addr_len);
            if (recv_len < 0) {
                if (errno == EWOULDBLOCK) {
                    printf("recvfrom timeout\n");
                    send_packet(sock, client, addr_len, buffer, bytes_read + 4);
                } else {
                    perror("recvfrom failed\n");
                    return;
                }
            } else if (buffer[1] != ACK) {
                printf("Received wrong opcode\n");
                return;
            } else {
                unsigned int ack_block_number = (buffer[2] << 8) + buffer[3];
                if (ack_block_number != block_number) {
                    printf("Received wrong block number\n");
                    send_packet(sock, client, addr_len, buffer, bytes_read + 4);
                } else {
                    break;
                }
            }
        }
        block_number++;
    }
    fclose(file);
}

void handle_request(int sock, struct sockaddr_in client, int addr_len) {
    char buffer[MAX_BUFFER];
    recvfrom(sock, buffer, MAX_BUFFER, 0, (struct sockaddr*)&client, &addr_len);
    if (buffer[1] == RRQ) {
        char filename[100];
        strcpy(filename, buffer + 2);
        send_file(sock, client, addr_len, filename);
    } else if (buffer[1] == WRQ) {
        char filename[100];
        strcpy(filename, buffer + 2);
        write_file(sock, client, addr_len,filename);
    } else {
        handle_error(sock, client, addr_len);
    }
}

int main() {
    int sock;
    struct sockaddr_in server, client;
    int addr_len = sizeof(struct sockaddr_in);
    if ((sock = socket(AF_INET, SOCK_DGRAM, 0)) < 0) {
        perror("socket creation failed");
        exit(EXIT_FAILURE);
    }
    server.sin_family = AF_INET;
    server.sin_port = htons(PORT);
    server.sin_addr.s_addr = INADDR_ANY;
    if (bind(sock, (struct sockaddr*)&server, sizeof(server)) < 0) {
        perror("bind failed");
        exit(EXIT_FAILURE);
    }
    while (1) {
        handle_request(sock, client, addr_len);
    }
    return 0;
}
