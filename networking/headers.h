#ifndef SHAM_H
#define SHAM_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <stdint.h>
#include <arpa/inet.h>
#include <sys/select.h>
#include <sys/time.h>
#include<sys/select.h>
#include<math.h>

// S.H.A.M. Header Structure
struct sham_header {
    uint32_t seq_num;     // Sequence Number
    uint32_t ack_num;     // Acknowledgment Number
    uint16_t flags;       // Control flags (SYN, ACK, FIN)
    uint16_t window_size; // Flow control window size
};

#define PAYLOAD_SIZE 1024

// S.H.A.M. Packet Structure
struct sham_packet {
    struct sham_header header;
    char payload[PAYLOAD_SIZE];
};

// Flags
#define SYN 0x1
#define ACK 0x2
#define FIN 0x4

#endif