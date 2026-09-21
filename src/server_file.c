#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <zlib.h>
#include <errno.h>
#include "proto.h"

#define PORT     9999
#define BACKLOG  5

ssize_t read_full(int fd, void *buf, size_t len) {
    size_t left = len;
    char *p = buf;
    while (left > 0) {
        ssize_t n = read(fd, p, left);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (n == 0) return (ssize_t)(len - left);
        p += n;
        left -= n;
    }
    return (ssize_t)len;
}

int main(void) {
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) { perror("socket"); return 1; }

    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port   = htons(PORT),
        .sin_addr.s_addr = inet_addr("127.0.0.1")
    };

    if (bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind"); close(server_fd); return 1;
    }
    if (listen(server_fd, BACKLOG) < 0) {
        perror("listen"); close(server_fd); return 1;
    }
    printf("server listening on 127.0.0.1:%d\n", PORT);

    int client_fd = accept(server_fd, NULL, NULL);
    if (client_fd < 0) { perror("accept"); close(server_fd); return 1; }
    printf("client connected\n");

    FILE *out = fopen("recv.mp4", "wb");
    if (!out) { perror("fopen recv.mp4"); close(client_fd); close(server_fd); return 1; }

    uint32_t total_bytes = 0;
    printf("sizeof(protocol_header_t) = %zu\n", sizeof(protocol_header_t));

    while (1) {
        protocol_header_t hdr;
        ssize_t r = read_full(client_fd, &hdr, sizeof(hdr));
        if (r <= 0) { printf("read header failed/closed, r=%zd\n", r); break; }

        /* 打印原始字节，和客户端输出对比 */
        printf("raw header: ");
        for (size_t i = 0; i < sizeof(hdr); i++) {
            printf("%02X ", ((uint8_t *)&hdr)[i]);
        }
        printf("\n");

        if (hdr.magic != PROTO_MAGIC) {
            printf("invalid magic: 0x%02X\n", hdr.magic);
            break;
        }
        if (hdr.version != PROTO_VERSION) {
            printf("invalid version: %u\n", hdr.version);
            break;
        }

        /* 关键：msg_type 是 2 字节 -> ntohs；其余 4 字节 -> ntohl */
        hdr.msg_type = ntohs(hdr.msg_type);
        hdr.seq      = ntohl(hdr.seq);
        hdr.length   = ntohl(hdr.length);
        hdr.crc32    = ntohl(hdr.crc32);

        printf("hdr: type=%u seq=%u len=%u crc=0x%08X\n",
               hdr.msg_type, hdr.seq, hdr.length, hdr.crc32);

        if (hdr.length == 0 || hdr.length > 100 * 1024 * 1024) {
            printf("bad length %u, drop\n", hdr.length);
            break;
        }

        char *buf = malloc(hdr.length);
        if (!buf) { perror("malloc"); break; }

        r = read_full(client_fd, buf, hdr.length);
        if (r != (ssize_t)hdr.length) {
            printf("read payload short: got %zd want %u\n", r, hdr.length);
            free(buf);
            break;
        }

        uint32_t crc = crc32(0L, Z_NULL, 0);
        crc = crc32(crc, (const Bytef *)buf, (uInt)hdr.length);
        if (crc != hdr.crc32) {
            printf("CRC error seq=%u calc=0x%08X want=0x%08X\n",
                   hdr.seq, crc, hdr.crc32);
            free(buf);
            continue;
        }

        size_t w = fwrite(buf, 1, hdr.length, out);
        if (w != hdr.length) {
            printf("fwrite short: %zu/%u\n", w, hdr.length);
            free(buf);
            break;
        }
        fflush(out);

        total_bytes += hdr.length;
        printf("recv seq=%u size=%u total=%u\n", hdr.seq, hdr.length, total_bytes);
        free(buf);
    }

    fclose(out);
    printf("done, total %u bytes\n", total_bytes);
    close(client_fd);
    close(server_fd);
    return 0;
}