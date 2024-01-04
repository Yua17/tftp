#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/time.h>
#include <errno.h>

#define RRQ 1
#define WRQ 2
#define DATA 3
#define ACK 4
#define ERROR 5
#define IPLENGTH 16
#define MAX_RETRIES 5
#define TIMEOUT 5



int create_socket() {
    int sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0) {
        perror("socket creation failed");
        exit(EXIT_FAILURE);
    }
    return sockfd;
}

void send_request(int sockfd, struct sockaddr_in servaddr, char *filename, int opcode) 
{
    char buffer[516];
    memset(buffer, 0, sizeof(buffer));

    // 设置操作码
    buffer[0] = 0;
    buffer[1] = opcode;

    // 设置文件名
    strcpy(buffer + 2, filename);

    // 设置传输模式
    strcpy(buffer + 2 + strlen(filename) + 1, "octet");

    // 发送请求
    if (sendto(sockfd, buffer, 4 + strlen(filename) + strlen("octet"), 0, \
    (struct sockaddr *)&servaddr, sizeof(servaddr)) < 0) 
    {
        perror("sendto failed");
        exit(EXIT_FAILURE);
    }
}

void handle_retries(int sockfd, struct sockaddr_in servaddr)
{
    int retries = 0;
    while (retries < MAX_RETRIES) {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(sockfd, &fds);
        struct timeval timeout;
        timeout.tv_sec = TIMEOUT;
        timeout.tv_usec = 0;

        int ret = select(sockfd + 1, &fds, NULL, NULL, &timeout);
        if (ret < 0) {
            perror("select failed");
            exit(EXIT_FAILURE);
        } else if (ret > 0) {
            // 收到服务器的响应，跳出循环
            break;
        } else {
            // 超时，增加重试次数
            retries++;
        }
    }

    if (retries == MAX_RETRIES) {
        printf("Server does not respond.\n");
        exit(EXIT_FAILURE);
    }
}


void send_rrq(int sockfd, struct sockaddr_in servaddr, char *filename) 
{
    send_request(sockfd, servaddr, filename, RRQ);
    handle_retries(sockfd, servaddr);
}

void send_wrq(int sockfd, struct sockaddr_in servaddr, char *filename) 
{
    send_request(sockfd, servaddr, filename, WRQ);
    handle_retries(sockfd, servaddr);
}

void send_ack(int sockfd, struct sockaddr_in servaddr, unsigned int block_number) 
{
    unsigned char buffer[4];
    // 设置操作码
    buffer[0] = 0;
    buffer[1] = ACK;

    // 设置块编号
    buffer[2] = (block_number >> 8) & 0xFF; // 高 8 位 放到 buffer[2]
    buffer[3] = block_number & 0xFF; // 低 8 位 放到 buffer[3]

    // 发送 ACK
    if (sendto(sockfd, buffer, 4, 0, (struct sockaddr *)&servaddr, sizeof(servaddr)) < 0) {
        perror("sendto failed");
        exit(EXIT_FAILURE);
    }
}

void handle_timeout(int sockfd, struct sockaddr_in servaddr, unsigned int block_number)
{
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
        // 超时，重新发送ACK
        send_ack(sockfd, servaddr, block_number);
    } else {
        perror("recvfrom failed");
        exit(EXIT_FAILURE);
    }
}

void receive_data_and_send_ack(int sockfd, struct sockaddr_in servaddr, char *filename)
{
    unsigned char buffer[516];
    memset(buffer, 0, sizeof(buffer));
    unsigned int block_number = 0;
    ssize_t n;
    //创建文件
    FILE *fp = fopen(filename, "wb");
    if (fp == NULL) {
        perror("fopen failed");
        exit(EXIT_FAILURE);
    }
    struct timeval timeout;
    timeout.tv_sec = TIMEOUT;
    timeout.tv_usec = 0;
    if (setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) < 0) 
    {
        perror("setsockopt failed");
        return;
    }

    while(1)
    {
        // 接收来自服务器的文件数据
        socklen_t len = sizeof(servaddr);
        memset(buffer, 0, sizeof(buffer));
        n = recvfrom(sockfd, buffer, sizeof(buffer), 0, (struct sockaddr *)&servaddr, &len);
        printf("%d\n", buffer[1]);
        printf("%d\n", buffer[0]);
        if (n < 0) {
            handle_timeout(sockfd, servaddr, block_number);
            continue;
        }

        // 通过操作码判断是否为数据包
        if (buffer[0] != 0 || buffer[1] != DATA) {
            printf("Invalid data\n");
            send_ack(sockfd, servaddr, block_number+1);
            continue;
        }

        // 将数据写入文件
        int data_size = fwrite(buffer + 4, 1, n - 4, fp);
        if (data_size < 0) {
            perror("fwrite failed");
            exit(EXIT_FAILURE);
        }

        // 自增块编号
        block_number++;

        // 发送 ACK
        send_ack(sockfd, servaddr, block_number);

        // 检查文件是否接收完毕
        if (data_size < 512) {
            break;
        }
    }
}

/**
 * @brief 向服务器发送数据并接收确认
 *
 * @param sockfd 套接字文件描述符
 * @param servaddr 服务器地址结构体
 * @param filename 文件名
 */
void send_data_and_receive_ack(int sockfd, struct sockaddr_in servaddr, char *filename)
{
    unsigned char buffer[516];
    memset(buffer, 0, sizeof(buffer));
    unsigned int block_number = 1;
    int first_block = 1;

    // 读取文件
    FILE *fp = fopen(filename, "rb");
    if (fp == NULL) {
        perror("fopen failed");
        exit(EXIT_FAILURE);
    }

    struct timeval timeout;
    timeout.tv_sec = TIMEOUT;
    timeout.tv_usec = 0;
    if (setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) < 0)
    {
        perror("setsockopt failed");
        return;
    }

    // 等待服务器的第一个ACK
    socklen_t len = sizeof(servaddr);
    ssize_t n;
    do {
        memset(buffer, 0, sizeof(buffer));
        n = recvfrom(sockfd, buffer, sizeof(buffer), 0, (struct sockaddr *)&servaddr, &len);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                // 超时，重新发送WRQ
                send_wrq(sockfd, servaddr, filename);
            } else {
                perror("recvfrom failed");
                exit(EXIT_FAILURE);
            }
        }
    } while (n < 0 || buffer[1] != ACK || (buffer[2] << 8) + buffer[3] != 0);
    printf("Received ACK 0\n");

    while(1)
    {
        if (!first_block) {
            // 接收服务器发来的 ACK
            len = sizeof(servaddr);
            memset(buffer, 0, sizeof(buffer));
            n = recvfrom(sockfd, buffer, sizeof(buffer), 0, (struct sockaddr *)&servaddr, &len);
            if (n < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    // 超时，重新发送数据
                    continue;
                } else {
                    perror("recvfrom failed");
                    exit(EXIT_FAILURE);
                }
            }

            // 检查 ACK 是否有效
            if (buffer[0] != 0 || buffer[1] != ACK) {
                printf("Invalid ACK\n");
                exit(EXIT_FAILURE);
            }

            // 检查 ACK 的块编号
            unsigned int ack_block_number = (buffer[2] << 8) + buffer[3];
            if ((ack_block_number+1) != block_number) {
                printf("Invalid block number\n");
                exit(EXIT_FAILURE);
            }
        }

        // 开始构造DATA包，设置操作码为 DATA
        buffer[0] = 0;
        buffer[1] = DATA;

        // 设置块编号
        buffer[2] = (block_number >> 8) & 0xFF;
        buffer[3] = block_number & 0xFF;

        // 从文件中读取数据并写入 buffer
        int data_size = fread(buffer + 4, 1, 512, fp);
        if (data_size < 0) {
            perror("fread failed");
            exit(EXIT_FAILURE);
        }

        // 将构造好的 DATA 包发送给服务器
        if (sendto(sockfd, buffer, 4 + data_size, 0, (struct sockaddr *)&servaddr, sizeof(servaddr)) < 0) {
            perror("sendto failed");
            exit(EXIT_FAILURE);
        }
        // 检查文件是否发送完毕
        if (data_size < 512) {
            break;
        }

        // 自增块编号
        block_number++;
        first_block = 0;
    }
}

int main() {
    char ip[IPLENGTH];
    int sockfd = create_socket();
    struct sockaddr_in servaddr;
    memset(&servaddr, 0, sizeof(servaddr));
    printf("Enter server IP: ");
    scanf("%s", ip);
    getchar();
    servaddr.sin_family = AF_INET;
    servaddr.sin_port = htons(69); // TFTP uses port 69
    servaddr.sin_addr.s_addr = inet_addr(ip);
    while(1)
    {
        printf("Enter order: ");
        char order[20];
        scanf("%s", order);
        getchar();
        if (strcmp(order, "get") == 0) {
            char filename[100];
            scanf("%s", filename);    
            send_rrq(sockfd, servaddr, filename);
            receive_data_and_send_ack(sockfd, servaddr, filename);
        } 
        else if (strcmp(order, "put") == 0) {
            char filename[100];
            scanf("%s", filename);  
            send_wrq(sockfd, servaddr, filename);
            send_data_and_receive_ack(sockfd, servaddr, filename);
        } 
        else if(strcmp(order, "exit") == 0)
            exit(EXIT_SUCCESS);
        else if(strcmp(order, "help") == 0)
            printf("get <filename> - download file from server\nput <filename> - upload file to server\nexit - exit program\n");
        else if(strcmp(order, "clear") == 0)
            system("clear");
        else{
            printf("Invalid order\n");
        }
    
    }
    return 0;
}