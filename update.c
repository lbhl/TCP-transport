#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <stdint.h>
#include <zlib.h>  // For CRC32
#include <openssl/bio.h>
#include <openssl/evp.h>

#define PORT 33333

struct message {
    int action;
    char fromname[20];
    char toname[20];
    char msg[1024];
};

struct online {
    int cfd;// 客户端文件描述符
    char name[20];
    struct online *next;// 指向下一个在线用户节点的指针
};

struct online *head = NULL;

void insert_user(struct online *new) {
    if (head == NULL) {
        new->next = NULL;
        head = new;
    } else {
        new->next = head->next;
        head->next = new;
    }
}

//查找指定用户名 toname 对应的客户端文件描述
int find_cfd(char *toname) {
    if (head == NULL) {
        return -1;
    }

    struct online *temp = head;

    while (temp != NULL) {
        if (strcmp(temp->name, toname) == 0) {
            return temp->cfd;
        }
        temp = temp->next;
    }
    return -1;
}

//将下载的数据写入到文件流中
size_t write_data(void *ptr, size_t size, size_t nmemb, FILE *stream) {
    return fwrite(ptr, size, nmemb, stream);
}

//计算数据的 CRC32 校验值
uint32_t calculate_crc32(const void *data, size_t length) {
    uint32_t crc = crc32(0L, Z_NULL, 0);
    crc = crc32(crc, data, length);
    return crc;
}

//将 Base64 编码的字符串 input 解码为原始数据，并返回解码后的数据
char *base64_decode(const char *input) {
    BIO *b64, *bmem;
    int decoded_size = 0;

    b64 = BIO_new(BIO_f_base64());
    BIO_set_flags(b64, BIO_FLAGS_BASE64_NO_NL);
    bmem = BIO_new_mem_buf((void *)input, -1);
    bmem = BIO_push(b64, bmem);

    char *buffer = (char *)malloc(strlen(input) * 3 / 4 + 1);
    decoded_size = BIO_read(bmem, buffer, strlen(input));
    buffer[decoded_size] = '\0';

    BIO_free_all(bmem);

    return buffer;
}

void *recv_message(void *arg) {
    int ret;
    int to_cfd;
    int cfd = *((int *)arg);

    struct online *new;
    struct message *msg = (struct message *)malloc(sizeof(struct message));

    while (1) {
        memset(msg, 0, sizeof(struct message));
        
        //接收从 cfd 套接字接收的消息，存储在 msg
        if ((ret = recv(cfd, msg, sizeof(struct message), 0)) < 0) {
            perror("recv error!");
            exit(EXIT_FAILURE);
        }

        if (ret == 0) {
            printf("%d is close!\n", cfd);
            pthread_exit(NULL);
        }

        switch (msg->action) {
            case 1: {
                new = (struct online *)malloc(sizeof(struct online));
                new->cfd = cfd;//将当前客户端套接字描述符 cfd 存储在新创建的在线用户结构体
                strcpy(new->name, msg->fromname);
                insert_user(new);
                msg->action = 1;
                send(cfd, msg, sizeof(struct message), 0);
                break;
            }
            case 2: {
                to_cfd = find_cfd(msg->toname);
                msg->action = 2;
                send(to_cfd, msg, sizeof(struct message), 0);

                // Record message to file
                time_t timep;
                time(&timep);
                char buff[100];
                strcpy(buff, ctime(&timep));
                buff[strlen(buff) - 1] = 0;

                char record[1024];
                sprintf(record, "%s(%s->%s):%s", buff, msg->fromname, msg->toname, msg->msg);
                printf("one record is:%s \n", record);

                FILE *fp;
                fp = fopen("a.txt", "a+");
                if (fp == NULL) {
                    printf("file open error!");
                } else {
                    fprintf(fp, "%s\n", record);
                    printf("record have written into file. \n");
                }
                fclose(fp);
                break;
            }
            case 3: {
                struct online *temp = head;

                while (temp != NULL) {
                    to_cfd = temp->cfd;
                    msg->action = 3;
                    send(to_cfd, msg, sizeof(struct message), 0);
                    temp = temp->next;
                }
                break;
            }
            case 4: {
            //将接收到的 Base64 编码数据解码为原始数据，并将解码后的数据保存
                char *decoded_data = base64_decode(msg->base64_data);
                uint32_t received_crc32 = msg->crc32_value;
                uint32_t calculated_crc32 = calculate_crc32(decoded_data, strlen(decoded_data));//计算解码后数据的 CRC32 校验值

                if (received_crc32 == calculated_crc32) {
                    printf("CRC32 verification passed.\n");

                    // 如果 CRC32 校验通过，将解码后的数据写入文件
                    FILE *fp = fopen(msg->filename, "wb");
                    fwrite(decoded_data, 1, strlen(decoded_data), fp);
                    fclose(fp);

                    printf("File saved: %s\n", msg->filename);
                } else {
                    printf("CRC32 verification failed.\n");
                }

                free(decoded_data);
                break;
            }
        }

        usleep(3);
    }

    pthread_exit(NULL);
}

int main() {
    int cfd;
    int sockfd;
    int c_len;

    char buffer[1024];

    pthread_t id;

    struct sockaddr_in s_addr;
    struct sockaddr_in c_addr;

    if ((sockfd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        perror("socket error!");
        exit(EXIT_FAILURE);
    }

    printf("socket success!\n");

    int opt = 1;
    setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    bzero(&s_addr, sizeof(struct sockaddr_in));
    s_addr.sin_family = AF_INET;
    s_addr.sin_port = htons(PORT);
    s_addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(sockfd, (struct sockaddr *)(&s_addr), sizeof(struct sockaddr_in)) < 0) {
        perror("bind error!");
        exit(EXIT_FAILURE);
    }

    printf("bind success!\n");

    if (listen(sockfd, 3) < 0) {
        perror("listen error!");
        exit(EXIT_FAILURE);
    }

    printf("listen success!\n");

    while (1) {
        memset(buffer, 0, sizeof(buffer));

        bzero(&c_addr, sizeof(struct sockaddr_in));
        c_len = sizeof(struct sockaddr_in);

        printf("accepting........!\n");

        if ((cfd = accept(sockfd, (struct sockaddr *)(&c_addr), &c_len)) < 0) {
            perror("accept error!");
            exit(EXIT_FAILURE);
        }

        printf("port = %d ip = %s\n", ntohs(c_addr.sin_port), inet_ntoa(c_addr.sin_addr));

        if (pthread_create(&id, NULL, recv_message, (void *)(&cfd)) != 0) {
            perror("pthread create error!");
            exit(EXIT_FAILURE);
        }

        usleep(3);
    }

    return 0;
}

