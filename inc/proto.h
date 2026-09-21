#ifndef PROTO_H
#define PROTO_H

#include <stdint.h>

#pragma pack(push, 1)
typedef struct {
    uint8_t  magic;      /* 固定 0xAA */
    uint8_t  version;    /* 协议版本，当前 1 */
    uint16_t msg_type;   /* 消息类型，网络序 */
    uint32_t seq;        /* 序号，网络序 */
    uint32_t length;     /* payload 长度，网络序 */
    uint32_t crc32;      /* payload 的 CRC32，网络序；0 表示不校验 */
} protocol_header_t;
#pragma pack(pop)

#define PROTO_MAGIC   0xAA
#define PROTO_VERSION 1

#define MSG_TYPE_FILE 0x0002

#endif /* PROTO_H */