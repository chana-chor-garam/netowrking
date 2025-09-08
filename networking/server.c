#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "headers.h"
#include <sys/socket.h>

#define SERVER_PORT 8888
#define BUFFER_SIZE sizeof(struct sham_header)

void recv_data(int sockfd) {
    struct sockaddr_in client_addr;
    socklen_t client_len = sizeof(client_addr);
    struct sham_packet packet;
    int expected_seq = 1;
    
    printf("Ready to receive data from the client...\n");
    
    while (1) {
        ssize_t bytes_received = recvfrom(sockfd, &packet, sizeof(struct sham_packet), 0, (struct sockaddr *)&client_addr, &client_len);
        if (bytes_received < 0) {
            continue; // Keep waiting
        }
        
        // Check for the FIN flag to terminate the data transfer
        if (packet.header.flags & FIN) {
            printf("Received FIN from client. Starting connection termination.\n");
            
            // Step 1: Send ACK for client's FIN
            struct sham_header ack_header;
            ack_header.flags = ACK;
            ack_header.seq_num = htonl(expected_seq);
            ack_header.ack_num = htonl(ntohl(packet.header.seq_num) + 1);
            sendto(sockfd, &ack_header, sizeof(ack_header), 0, (const struct sockaddr *)&client_addr, client_len);
            printf("Sent ACK for client's FIN.\n");
            
            // Step 2: Send Server's own FIN
            struct sham_header fin_header;
            fin_header.flags = FIN;
            fin_header.seq_num = htonl(expected_seq);
            fin_header.ack_num = htonl(0);
            sendto(sockfd, &fin_header, sizeof(fin_header), 0, (const struct sockaddr *)&client_addr, client_len);
            printf("Sent server FIN. Waiting for final ACK.\n");
            
            // Step 3: Wait for final ACK
            struct sham_header final_ack;
            recvfrom(sockfd, &final_ack, sizeof(final_ack), 0, (struct sockaddr *)&client_addr, &client_len);
            if (final_ack.flags & ACK) {
                printf("Received final ACK. Connection closed gracefully.\n");
            }
            
            break; // Exit the data receiving loop
        }
        
        // Check for the expected sequence number
        if (ntohl(packet.header.seq_num) == expected_seq) {
            printf("RCV DATA SEQ=%u\n", expected_seq);
            
            // Print the received data to the console instead of writing to a file
            printf("Received: %s\n", packet.payload);
            
            // Send cumulative ACK
            struct sham_header ack_header;
            ack_header.flags = ACK;
            // The acknowledgment number is the next expected sequence number
            ack_header.ack_num = htonl(expected_seq + (bytes_received - sizeof(struct sham_header)));
            sendto(sockfd, &ack_header, sizeof(ack_header), 0, (const struct sockaddr *)&client_addr, client_len);
            printf("RCV sends: ACK=%u\n", ntohl(ack_header.ack_num));
            
            expected_seq += (bytes_received - sizeof(struct sham_header));
        } else {
            // Out-of-order or duplicate packet
            printf("RCV DATA SEQ=%u (out of order). Resending ACK for %u\n", ntohl(packet.header.seq_num), expected_seq);
            struct sham_header ack_header;
            ack_header.flags = ACK;
            ack_header.ack_num = htonl(expected_seq);
            sendto(sockfd, &ack_header, sizeof(ack_header), 0, (const struct sockaddr *)&client_addr, client_len);
        }
    }
}

int main() {
    int sockfd;
    struct sockaddr_in server_addr, client_addr;
    socklen_t client_len = sizeof(client_addr);
    struct sham_header header;

    if ((sockfd = socket(AF_INET, SOCK_DGRAM, 0)) < 0) {
        perror("socket creation failed");
        exit(EXIT_FAILURE);
    }

    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(SERVER_PORT);

    if (bind(sockfd, (const struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        perror("bind failed");
        exit(EXIT_FAILURE);
    }

    printf("Server listening on port %d...\n", SERVER_PORT);

    // --- Handshake Step 1: Receive SYN ---
    printf("Waiting for SYN from client...\n");
    recvfrom(sockfd, &header, BUFFER_SIZE, 0, (struct sockaddr *)&client_addr, &client_len);

    if (header.flags & SYN) {
        printf("Received SYN with seq_num: %u\n", ntohl(header.seq_num));

        // --- Handshake Step 2: Send SYN-ACK ---
        struct sham_header syn_ack_header;
        // IMPORTANT: Initialize the entire header structure
        memset(&syn_ack_header, 0, sizeof(syn_ack_header));
        
        syn_ack_header.flags = SYN | ACK;
        syn_ack_header.seq_num = htonl(100); // Server's initial sequence number Y
        syn_ack_header.ack_num = htonl(ntohl(header.seq_num) + 1); // Acknowledges client's seq_num X+1
        syn_ack_header.window_size = htons(1024);

        sendto(sockfd, &syn_ack_header, sizeof(syn_ack_header), 0, (const struct sockaddr *)&client_addr, client_len);
        printf("Sent SYN-ACK with seq_num: %u and ack_num: %u\n", ntohl(syn_ack_header.seq_num), ntohl(syn_ack_header.ack_num));

        // --- Handshake Step 3: Wait for Final ACK ---
        recvfrom(sockfd, &header, BUFFER_SIZE, 0, (struct sockaddr *)&client_addr, &client_len);
        if ((header.flags & ACK) && (ntohl(header.ack_num) == ntohl(syn_ack_header.seq_num) + 1)) {
            printf("Received final ACK. Handshake complete.\n");
        } else {
            printf("Handshake failed.\n");
        }
    } else {
        printf("Expected SYN, but received different flag.\n");
    }

    // --- Data Transfer Step ---
    printf("Handshake complete. Starting data transfer.\n");
    recv_data(sockfd); // This function now handles the complete termination

    // REMOVED: The duplicate FIN handling code that was causing the problem
    
    printf("Server shutting down.\n");
    close(sockfd);
    return 0;
}