#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "headers.h"
#include <sys/socket.h>

#define SERVER_IP "127.0.0.1"
#define SERVER_PORT 8888
#define BUFFER_SIZE sizeof(struct sham_header)
#define PAYLOAD_SIZE 1024

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "headers.h"
#include <sys/socket.h>

#define PAYLOAD_SIZE 1024

void send_data(int sockfd, struct sockaddr_in *server_addr, socklen_t server_len) {
    int seq_num = 1;
    char payload[PAYLOAD_SIZE];
    
    printf("Enter data to send (type 'quit' to exit):\n");
    
    while (1) {
        memset(payload, 0, PAYLOAD_SIZE);
        if (fgets(payload, PAYLOAD_SIZE, stdin) == NULL) {
            break;
        }
        
        if (strncmp(payload, "quit\n", 5) == 0) {
            printf("Quitting data transfer.\n");
            break;
        }
        
        struct sham_packet packet;
        packet.header.flags = 0; 
        packet.header.seq_num = htonl(seq_num);
        memcpy(packet.payload, payload, strlen(payload));
        
        ssize_t bytes_to_send = sizeof(struct sham_header) + strlen(payload);
        sendto(sockfd, &packet, bytes_to_send, 0, (const struct sockaddr *)server_addr, server_len);
        printf("SND DATA SEQ=%u\n", seq_num);
        seq_num += strlen(payload);
        
        struct sham_header ack_header;
        socklen_t temp_len = server_len;
        recvfrom(sockfd, &ack_header, sizeof(ack_header), 0, (struct sockaddr *)server_addr, &temp_len);
        if (ack_header.flags & ACK) {
            printf("Received ACK=%u\n", ntohl(ack_header.ack_num));
        }
    }
    
    // --- Termination logic starts here ---
    printf("Sending FIN to server...\n");
    struct sham_header fin_header;
    fin_header.flags = FIN;
    // Set a meaningful sequence number for the FIN packet
    fin_header.seq_num = htonl(seq_num); 
    fin_header.ack_num = htonl(0);
    sendto(sockfd, &fin_header, sizeof(fin_header), 0, (const struct sockaddr *)server_addr, server_len);

    // Step 2: Wait for ACK
    struct sham_header header;
    socklen_t temp_len = server_len;
    recvfrom(sockfd, &header, sizeof(header), 0, (struct sockaddr *)server_addr, &temp_len);
    if (header.flags & ACK) {
        printf("Received ACK from server. Waiting for their FIN.\n");

        // Step 3: Wait for server's FIN
        recvfrom(sockfd, &header, sizeof(header), 0, (struct sockaddr *)server_addr, &temp_len);
        if (header.flags & FIN) {
            printf("Received FIN from server. Sending final ACK.\n");

            // Step 4: Send final ACK
            struct sham_header final_ack_header;
            final_ack_header.flags = ACK;
            final_ack_header.seq_num = htonl(ntohl(header.ack_num));
            final_ack_header.ack_num = htonl(ntohl(header.seq_num) + 1);
            sendto(sockfd, &final_ack_header, sizeof(final_ack_header), 0, (const struct sockaddr *)server_addr, server_len);
            printf("Connection closed.\n");
        }
    }
}

int main() {
    int sockfd;
    struct sockaddr_in server_addr;
    socklen_t server_len = sizeof(server_addr);
    struct sham_header header;

    if ((sockfd = socket(AF_INET, SOCK_DGRAM, 0)) < 0) {
        perror("socket creation failed");
        exit(EXIT_FAILURE);
    }

    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(SERVER_PORT);
    inet_pton(AF_INET, SERVER_IP, &server_addr.sin_addr);

    // --- Handshake Step 1: Send SYN ---
    header.flags = SYN;
    header.seq_num = htonl(50); // Client's initial sequence number X
    sendto(sockfd, &header, sizeof(header), 0, (const struct sockaddr *)&server_addr, server_len);
    printf("Sent SYN with seq_num: %u\n", ntohl(header.seq_num));

    // --- Handshake Step 2: Wait for SYN-ACK ---
    recvfrom(sockfd, &header, BUFFER_SIZE, 0, (struct sockaddr *)&server_addr, &server_len);
    if ((header.flags & SYN) && (header.flags & ACK)) {
        printf("Received SYN-ACK with seq_num: %u and ack_num: %u\n", ntohl(header.seq_num), ntohl(header.ack_num));

        // --- Handshake Step 3: Send Final ACK ---
        struct sham_header final_ack_header;
        final_ack_header.flags = ACK;
        final_ack_header.seq_num = htonl(ntohl(header.ack_num));
        final_ack_header.ack_num = htonl(ntohl(header.seq_num) + 1);

        sendto(sockfd, &final_ack_header, sizeof(final_ack_header), 0, (const struct sockaddr *)&server_addr, server_len);
        printf("Sent final ACK. Handshake complete.\n");
    } else {
        printf("Handshake failed.\n");
    }

    // --- Data Transfer Step ---
    // The send_data function now handles both data transfer AND termination
    printf("Handshake complete. Starting data transfer.\n");
    send_data(sockfd, &server_addr, server_len);
    
    close(sockfd);
    return 0;
}