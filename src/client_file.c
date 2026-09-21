// tcp发包程序；用于将文件或者流数据进行发送
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <signal.h>
#include <time.h>
#include <zlib.h>  // 编译时加 -lz
#include "proto.h"

/* 完整发送指定字节数（避免TCP半包） */
ssize_t send_full(int fd, const void *buf, size_t len) {
    const char *p = buf;
    size_t left = len;
    while (left > 0) {
        ssize_t n = send(fd, p, left, 0);
        if (n <= 0) {
            perror("send failed");
            return -1;
        }
        p += n;
        left -= n;
    }
    return (ssize_t)len;
}

/* 预扫描整个文件计算 CRC32（调试阶段用；大文件可改成流式 CRC 分段发） */
uint32_t calc_file_crc32(FILE *fp) {
    if (fseek(fp, 0, SEEK_SET) != 0) return 0;
    uint32_t crc = crc32(0L, Z_NULL, 0);
    unsigned char buf[8192];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), fp)) > 0) {
        crc = crc32(crc, buf, (uInt)n);
    }
    fseek(fp, 0, SEEK_SET);
    return crc;
}

#define SERVER_IP   "127.0.0.1"
#define SERVER_PORT 9999
#define BUF_SIZE    4096

int main(int argc, char *argv[]) {
    if (argc != 2) {
        printf("usage: %s <file_path>\n", argv[0]);
        return 1;
    }
    signal(SIGPIPE, SIG_IGN);

    FILE *fp = fopen(argv[1], "rb");
    if (!fp) { perror("fopen failed"); return 1; }
    fseek(fp, 0, SEEK_END);
    long file_size = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    printf("file size: %ld bytes\n", file_size);

    /* 先算整文件 CRC32，填进头部，服务端才能校验通过 */
    uint32_t crc = calc_file_crc32(fp);
    printf("file crc32 = 0x%08X\n", crc);

    int sock_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (sock_fd < 0) { perror("socket"); fclose(fp); return 1; }

    struct sockaddr_in server_addr = {
        .sin_family = AF_INET,
        .sin_port   = htons(SERVER_PORT)
    };
    inet_pton(AF_INET, SERVER_IP, &server_addr.sin_addr);
    if (connect(sock_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        perror("connect"); fclose(fp); close(sock_fd); return 1;
    }

    /* 关键：所有多字节字段做网络序转换 */
    protocol_header_t hdr;
    memset(&hdr, 0, sizeof(hdr));
    hdr.magic    = PROTO_MAGIC;              /* 1 字节，无需转换 */
    hdr.version  = PROTO_VERSION;            /* 1 字节，无需转换 */
    hdr.msg_type = htons(MSG_TYPE_FILE);     /* 2 字节 -> 网络序 */
    hdr.seq      = htonl(0);                 /* 4 字节 -> 网络序 */
    hdr.length   = htonl((uint32_t)file_size);
    hdr.crc32    = htonl(crc);

    /* 打印原始字节，和服务端对比 */
    printf("sizeof(protocol_header_t) = %zu\n", sizeof(hdr));
    printf("raw header: ");
    for (size_t i = 0; i < sizeof(hdr); i++) {
        printf("%02X ", ((uint8_t *)&hdr)[i]);
    }
    printf("\n");

    if (send_full(sock_fd, &hdr, sizeof(hdr)) < 0) {
        fclose(fp); close(sock_fd); return 1;
    }

    char buf[BUF_SIZE];
    size_t read_len;
    uint32_t total_sent = 0;
    while ((read_len = fread(buf, 1, BUF_SIZE, fp)) > 0) {
        if (send_full(sock_fd, buf, read_len) < 0) break;
        total_sent += (uint32_t)read_len;
        printf("sent %u/%ld bytes\r", total_sent, file_size);
    }
    printf("\nsend file done, total %u bytes\n", total_sent);

    fclose(fp);
    close(sock_fd);
    return 0;
}