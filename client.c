#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <curl/curl.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <zlib.h>        
#include <openssl/bio.h>
#include <openssl/evp.h>
#include <openssl/buffer.h>  

#define PORT 33333
#define VERSION_URL "http://test.sh.fangk.top/server_version.txt"
#define UPDATE_URL "http://test.sh.fangk.top/client_new"
#define LOCAL_VERSION "version.txt"
#define CLIENT_PATH "./client"
#define TEMP_CLIENT_PATH "./client_new_temp"
#define UPDATE_FLAG "update_complete.flag"

#define CMD_REG "reg"
#define CMD_SEND "send"
#define CMD_ALL "all"
#define CMD_FILE_TRANSFER "file"

// 写入回调函数，用于下载文件
size_t write_data(void *ptr, size_t size, size_t nmemb, FILE *stream) {
    return fwrite(ptr, size, nmemb, stream);
}

// Base64编码函数
char *base64_encode(const unsigned char *input, int length) {
    BIO *b64, *bio;
    BUF_MEM *bptr = NULL;//存储内存缓冲区
    char *buff = NULL;//存储最终的 Base64 编码字符串
    
    //创建一个处理 Base64 编码的 BIO
    b64 = BIO_new(BIO_f_base64());
    if (!b64) {
        fprintf(stderr, "Error creating BIO\n");
        return NULL;
    }
    
    // 创建一个内存 BIO
    bio = BIO_new(BIO_s_mem());
    if (!bio) {
        BIO_free(b64);
        fprintf(stderr, "Error creating BIO\n");
        return NULL;
    }
    
    //数据首先被编码为 Base64，然后被写入内存
    bio = BIO_push(b64, bio);
    BIO_set_flags(bio, BIO_FLAGS_BASE64_NO_NL);//不添加新行标志
    BIO_write(bio, input, length);
    BIO_flush(bio);
    
    // 获取内存 BIO 中的缓冲区指针 bptr
    BIO_get_mem_ptr(bio, &bptr);

    buff = (char *)malloc(bptr->length + 1);
    if (!buff) {
        BIO_free_all(bio);
        return NULL;
    }
    
    // 将编码后的数据从 bptr->data 复制到 buff 中
    memcpy(buff, bptr->data, bptr->length);
    buff[bptr->length] = '\0';

    BIO_free_all(bio);

    return buff;
}

// 计算数据的CRC32校验值
uint32_t calculate_crc32(const void *data, size_t length) {
    uint32_t crc = crc32(0L, Z_NULL, 0);
    crc = crc32(crc, data, length);
    return crc;
}

// 发送文件函数，包含Base64编码和CRC32校验
int send_file(int sockfd, const char *filename) {
    FILE *fp = fopen(filename, "rb");
    if (fp == NULL) {
        perror("File open error");
        return -1;
    }

    // 获取文件大小
    fseek(fp, 0L, SEEK_END);
    long file_size = ftell(fp);
    rewind(fp);

    // 读取文件内容
    char *file_content = (char *)malloc(file_size);
    fread(file_content, 1, file_size, fp);
    fclose(fp);

    // Base64编码文件内容
    char *base64_data = base64_encode((unsigned char *)file_content, file_size);

    // 计算CRC32校验值
    uint32_t crc32_value = calculate_crc32(file_content, file_size);

    // 构造消息
    struct message {
        int action;
        char filename[100];
        char base64_data[1024]; // 假设Base64编码后的数据不超过1024字节
        uint32_t crc32_value;
    } msg;

    msg.action = 4; // 文件下载请求
    strcpy(msg.filename, filename);
    strcpy(msg.base64_data, base64_data);
    msg.crc32_value = crc32_value;

    // 发送消息
    int bytes_sent = send(sockfd, &msg, sizeof(msg), 0);

    free(file_content);
    free(base64_data);

    return bytes_sent;
}

// 检查并更新客户端
void check_and_update(int sockfd) {
    CURL *curl;
    FILE *fp;
    CURLcode res;//用于存储 curl 操作的返回状态
    char server_version[64];
    char local_version[64];

    // 初始化 libcurl
    curl_global_init(CURL_GLOBAL_DEFAULT);//全局设置
    curl = curl_easy_init();//用于具体的网络操作

    if (curl) {
        // 下载服务器版本号文件并保存为本地的server_ version.txt
        fp = fopen(LOCAL_VERSION, "w");
        curl_easy_setopt(curl, CURLOPT_URL, VERSION_URL);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, fp);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_data);
        res = curl_easy_perform(curl);//下载服务器版本号文件并写入 fp 指向的文件中
        fclose(fp);

        if (res == CURLE_OK) {
            // 读取本地版本号
            fp = fopen(LOCAL_VERSION, "r");
            if (fp != NULL) {
                fscanf(fp, "%s", local_version);//读取文件内容到 local_version
                fclose(fp);

                // 比较版本号
                if (access("server_version.txt", F_OK) != -1) {
                    // 删除旧的 server_version.txt 文件
                    remove("server_version.txt");
                }

                // 重命名下载的版本号文件为 server_version.txt
                if (rename(LOCAL_VERSION, "server_version.txt") != 0) {
                    perror("Failed to rename version file");
                    exit(EXIT_FAILURE);
                }

                // 读取服务器版本号
                fp = fopen("server_version.txt", "r");
                fscanf(fp, "%s", server_version);
                fclose(fp);

                // 比较本地和服务器版本号
                if (strcmp(server_version, local_version) != 0) {
                    // 版本不同，下载新版本
                    printf("Downloading new version...\n");
                    fp = fopen(TEMP_CLIENT_PATH, "wb");
                    curl_easy_setopt(curl, CURLOPT_URL, UPDATE_URL);
                    curl_easy_setopt(curl, CURLOPT_WRITEDATA, fp);
                    res = curl_easy_perform(curl);
                    fclose(fp);

                    if (res == CURLE_OK) {
                        printf("Update downloaded.\n");

                        // 验证下载文件的大小
                        struct stat st;
                        if (stat(TEMP_CLIENT_PATH, &st) == 0 && st.st_size > 0) {
                            // 创建标志文件
                            int fd = open(UPDATE_FLAG, O_CREAT | O_WRONLY, 0644);
                            if (fd != -1) {
                                close(fd);
                            }

                            // 发送文件到服务器
                            if (send_file(sockfd, TEMP_CLIENT_PATH) > 0) {
                                printf("File sent successfully.\n");
                            } else {
                                printf("Failed to send file.\n");
                            }
                        } else {
                            printf("Downloaded file is invalid.\n");
                        }
                    } else {
                        printf("Failed to download update.\n");
                    }

                    // 退出当前客户端
                    exit(EXIT_FAILURE);
                } else {
                    printf("Client is up to date.\n");
                }
            } else {
                // 如果本地版本文件不存在，创建并写入服务器版本号
                printf("Local version not found, creating new one.\n");
                fp = fopen(LOCAL_VERSION, "w");
                fprintf(fp, "%s", server_version);
                fclose(fp);
            }
        } else {
            printf("Failed to fetch server version.\n");
        }

        curl_easy_cleanup(curl);
    } else {
        printf("Failed to initialize libcurl.\n");
    }

    curl_global_cleanup();
}

struct message {
    int action;
    char fromname[20];
    char toname[20];
    char msg[1024];
};

void *recv_message(void *arg) {
    time_t t;
    char buf[1024];
    time(&t);
    ctime_r(&t, buf);

    int ret;
    int cfd = *((int *)arg);

    struct message *msg = (struct message *)malloc(sizeof(struct message));

    while (1) {
        memset(msg, 0, sizeof(struct message));

        if ((ret = recv(cfd, msg, sizeof(struct message), 0)) < 0) {
            perror("recv error!");
            exit(EXIT_FAILURE);
        }

        if (ret == 0) {
            printf("%d is close!\n", cfd);
            pthread_exit(NULL);
        }

        switch (msg->action) {
            case 1:
                printf("reg success!\n");
                break;
            case 2:
                printf("time:%s recv:%s\n", buf, msg->msg);
                break;
            case 3:
                printf("time:%s all recv:%s\n", buf, msg->msg);
                break;
        }
        usleep(3);
    }

    pthread_exit(NULL);
}

volatile sig_atomic_t update_in_progress = 0;

void sig_handler(int signum) {
    if (signum == SIGINT || signum == SIGTERM) {
        update_in_progress = 1;
    }
}

int main() {
    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);

    printf("Checking for updates...\n");

    int sockfd;
    pthread_t id;
    struct sockaddr_in s_addr;
    
    //套接字创建和连接
    if ((sockfd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        perror("socket error!");
        exit(EXIT_FAILURE);
    }

    printf("client socket success!\n");
    
    //套接字配置
    bzero(&s_addr, sizeof(struct sockaddr_in));//清空结构体
    s_addr.sin_family = AF_INET;
    s_addr.sin_port = htons(PORT);//设置端口号
    s_addr.sin_addr.s_addr = inet_addr("101.133.168.114");//设置服务器的 IP 地址

    if (connect(sockfd, (struct sockaddr *)(&s_addr), sizeof(struct sockaddr_in)) < 0) {
        perror("connect error!");
        exit(EXIT_FAILURE);
    }

    printf("connect success!\n");
    
    //创建了一个新的线程，用来执行 recv_message 函数
    if (pthread_create(&id, NULL, recv_message, (void *)(&sockfd)) != 0) {
        perror("pthread create error!");
        exit(EXIT_FAILURE);
    }

    check_and_update(sockfd);

    char cmd[20];
    char name[20];
    char toname[20];
    char message[1024];
    char file_path[256];

    struct message *msg = (struct message *)malloc(sizeof(struct message));

    while (1) {
        printf("Please input cmd (reg/send/all/file): ");
        scanf("%s", cmd);

        if (strcmp(cmd, CMD_REG) == 0) {
            printf("Please input reg name: ");
            scanf("%s", name);

            msg->action = 1;
            strcpy(msg->fromname, name);

            if (send(sockfd, msg, sizeof(struct message), 0) < 0) {
                perror("send error reg!\n");
                exit(EXIT_FAILURE);
            }
        } else if (strcmp(cmd, CMD_SEND) == 0) {
            printf("Please input send to name: ");
            scanf("%s", toname);

            printf("Please input send message: ");
            scanf("%s", message);

            msg->action = 2;
            strcpy(msg->toname, toname);
            strcpy(msg->msg, message);

            if (send(sockfd, msg, sizeof(struct message), 0) < 0) {
                perror("send error send!\n");
                exit(EXIT_FAILURE);
            }
        } else if (strcmp(cmd, CMD_ALL) == 0) {
            printf("Please input all message: ");
            scanf("%s", message);

            msg->action = 3;
            strcpy(msg->msg, message);

            if (send(sockfd, msg, sizeof(struct message), 0) < 0) {
                perror("send error all!\n");
                exit(EXIT_FAILURE);
            }
        } else if (strcmp(cmd, CMD_FILE_TRANSFER) == 0) {
            printf("Please input file path: ");
            scanf("%s", file_path);

            // 发送文件到服务器
            if (send_file(sockfd, file_path) > 0) {
                printf("File sent successfully.\n");
            } else {
                printf("Failed to send file.\n");
            }
        }

        if (update_in_progress) {
            printf("Interrupt signal received. Exiting...\n");
            break;
        }
    }

    shutdown(sockfd, SHUT_RDWR);

    return 0;
}
