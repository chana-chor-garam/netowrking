#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <math.h>
#include "headers.h"
#include <sys/socket.h>
#include <sys/time.h>
#include <arpa/inet.h>

#define SERVER_IP "127.0.0.1"
#define SERVER_PORT 8888
#define BUFFER_SIZE sizeof(struct sham_header)
#define PAYLOAD_SIZE 1024
#define WINDOW_SIZE 4

// Updated RTO constants and variables
#define ALPHA 0.125
#define BETA 0.25
double EstimatedRTT = 500.0; // Initial RTT estimate in ms
double DevRTT = 0.0;
double RTO = 1000.0; // Initial RTO in ms

// Struct to hold a packet and its transmission info
struct sent_packet {
    struct sham_packet packet;
    struct timeval sent_time;
    int is_valid;
    int seq_num;
    size_t data_length;
};

void send_data(int sockfd, struct sockaddr_in *server_addr, socklen_t server_len) {
    int next_seq_num = 1;
    int base_seq_num = 1;
    struct sent_packet window[WINDOW_SIZE];
    int window_start = 0;
    int window_count = 0;
    
    // Initialize window
    for (int i = 0; i < WINDOW_SIZE; i++) {
        window[i].is_valid = 0;
    }
    
    printf("Enter data to send (type 'quit' to exit):\n");
    
    while (1) {
        // Step 1: Fill the sliding window with new packets
        while (window_count < WINDOW_SIZE) {
            char payload[PAYLOAD_SIZE];
            printf("Input: ");
            fflush(stdout);
            
            if (fgets(payload, PAYLOAD_SIZE, stdin) == NULL) {
                goto end_data_transfer;
            }
            
            if (strncmp(payload, "quit\n", 5) == 0) {
                goto end_data_transfer;
            }
            
            // Process payload
            size_t payload_len = strlen(payload);
            if (payload_len > 0 && payload[payload_len - 1] == '\n') {
                payload[payload_len - 1] = '\0';
                payload_len--;
            }

            if (payload_len == 0) {
                continue;
            }
            
            // Find next available slot in circular buffer
            int slot = (window_start + window_count) % WINDOW_SIZE;
            
            // Create and store the packet
            memset(&window[slot], 0, sizeof(struct sent_packet));
            window[slot].is_valid = 1;
            window[slot].seq_num = next_seq_num;
            window[slot].data_length = payload_len + 1; // +1 for null terminator
            
            window[slot].packet.header.flags = 0;
            window[slot].packet.header.seq_num = htonl(next_seq_num);
            window[slot].packet.header.ack_num = htonl(0);
            window[slot].packet.header.window_size = htons(1024);
            
            memcpy(window[slot].packet.payload, payload, payload_len);
            window[slot].packet.payload[payload_len] = '\0';
            
            // Send the packet
            ssize_t bytes_to_send = sizeof(struct sham_header) + payload_len + 1;
            sendto(sockfd, &window[slot].packet, bytes_to_send, 0, 
                   (const struct sockaddr *)server_addr, server_len);
            gettimeofday(&window[slot].sent_time, NULL);
            
            printf("SND DATA SEQ=%u\n", next_seq_num);
            
            next_seq_num += (payload_len + 1);
            window_count++;
        }
        
        if (window_count == 0) {
            break; // No more packets to send or wait for
        }
        
        // Step 2: Wait for ACKs or handle timeout
        fd_set read_fds;
        struct timeval tv, current_time;
        
        // Calculate timeout for the oldest unacknowledged packet
        gettimeofday(&current_time, NULL);
        
        if (!window[window_start].is_valid) {
            continue;
        }
        
        double elapsed = (current_time.tv_sec - window[window_start].sent_time.tv_sec) * 1000.0 + 
                        (current_time.tv_usec - window[window_start].sent_time.tv_usec) / 1000.0;
        double remaining_time = RTO - elapsed;
        
        if (remaining_time <= 0) {
            // Timeout - retransmit the oldest unacknowledged packet
            printf("TIMEOUT! Retransmitting SEQ=%u\n", window[window_start].seq_num);
            
            ssize_t bytes_to_send = sizeof(struct sham_header) + window[window_start].data_length;
            sendto(sockfd, &window[window_start].packet, bytes_to_send, 0, 
                   (const struct sockaddr *)server_addr, server_len);
            gettimeofday(&window[window_start].sent_time, NULL);
            
            // Double the RTO (exponential backoff)
            RTO *= 2;
            if (RTO > 5000) RTO = 5000; // Cap at 5 seconds
            
            continue;
        }

        // Set up select with remaining timeout
        tv.tv_sec = (long)remaining_time / 1000;
        tv.tv_usec = ((long)remaining_time % 1000) * 1000;

        FD_ZERO(&read_fds);
        FD_SET(sockfd, &read_fds);

        int select_result = select(sockfd + 1, &read_fds, NULL, NULL, &tv);

        if (select_result > 0) {
            // ACK received
            struct sham_header ack_header;
            recvfrom(sockfd, &ack_header, sizeof(ack_header), 0, NULL, NULL);
            
            if (ack_header.flags & ACK) {
                uint32_t ack_num = ntohl(ack_header.ack_num);
                printf("Received ACK=%u\n", ack_num);
                
                // Update RTT estimation
                gettimeofday(&current_time, NULL);
                double SampleRTT = (current_time.tv_sec - window[window_start].sent_time.tv_sec) * 1000.0 + 
                                  (current_time.tv_usec - window[window_start].sent_time.tv_usec) / 1000.0;
                
                EstimatedRTT = (1 - ALPHA) * EstimatedRTT + ALPHA * SampleRTT;
                DevRTT = (1 - BETA) * DevRTT + BETA * fabs(SampleRTT - EstimatedRTT);
                RTO = EstimatedRTT + 4 * DevRTT;
                
                if (RTO < 100) RTO = 100; // Minimum RTO
                if (RTO > 5000) RTO = 5000; // Maximum RTO
                
                // Handle cumulative ACK - slide the window
                while (window_count > 0 && window[window_start].is_valid && 
                       (uint32_t)(window[window_start].seq_num + window[window_start].data_length) <= ack_num) {
                    
                    printf("Packet SEQ=%u acknowledged\n", window[window_start].seq_num);
                    window[window_start].is_valid = 0;
                    window_start = (window_start + 1) % WINDOW_SIZE;
                    window_count--;
                    base_seq_num = ack_num;
                }
            }
        } else if (select_result == 0) {
            // Timeout occurred, will be handled in next iteration
            continue;
        } else {
            perror("select error");
            break;
        }
    }
    
end_data_transfer:
    // Wait for any remaining ACKs
    while (window_count > 0) {
        struct sham_header ack_header;
        struct timeval timeout = {1, 0}; // 1 second timeout
        
        fd_set read_fds;
        FD_ZERO(&read_fds);
        FD_SET(sockfd, &read_fds);
        
        if (select(sockfd + 1, &read_fds, NULL, NULL, &timeout) > 0) {
            recvfrom(sockfd, &ack_header, sizeof(ack_header), 0, NULL, NULL);
            if (ack_header.flags & ACK) {
                uint32_t ack_num = ntohl(ack_header.ack_num);
                printf("Final ACK received: %u\n", ack_num);
                
                // Slide window for final ACKs
                while (window_count > 0 && window[window_start].is_valid && 
                       (uint32_t)(window[window_start].seq_num + window[window_start].data_length) <= ack_num) {
                    window[window_start].is_valid = 0;
                    window_start = (window_start + 1) % WINDOW_SIZE;
                    window_count--;
                }
            }
        } else {
            break; // Timeout waiting for final ACKs
        }
    }
    
    // --- Termination logic starts here ---
    printf("Sending FIN to server...\n");
    struct sham_header fin_header;
    memset(&fin_header, 0, sizeof(fin_header));
    
    fin_header.flags = FIN;
    fin_header.seq_num = htonl(next_seq_num); 
    fin_header.ack_num = htonl(0);
    fin_header.window_size = htons(1024);
    
    sendto(sockfd, &fin_header, sizeof(fin_header), 0, (const struct sockaddr *)server_addr, server_len);

    // Wait for ACK and FIN from server
    struct sham_header header;
    socklen_t temp_len = server_len;
    
    // Wait for server's ACK
    recvfrom(sockfd, &header, sizeof(header), 0, (struct sockaddr *)server_addr, &temp_len);
    if (header.flags & ACK) {
        printf("Received ACK from server. Waiting for their FIN.\n");

        // Wait for server's FIN
        recvfrom(sockfd, &header, sizeof(header), 0, (struct sockaddr *)server_addr, &temp_len);
        if (header.flags & FIN) {
            printf("Received FIN from server. Sending final ACK.\n");

            // Send final ACK
            struct sham_header final_ack_header;
            memset(&final_ack_header, 0, sizeof(final_ack_header));
            
            final_ack_header.flags = ACK;
            final_ack_header.seq_num = htonl(ntohl(header.ack_num));
            final_ack_header.ack_num = htonl(ntohl(header.seq_num) + 1);
            final_ack_header.window_size = htons(1024);
            
            sendto(sockfd, &final_ack_header, sizeof(final_ack_header), 0, 
                   (const struct sockaddr *)server_addr, server_len);
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

    // Connection establishment
    memset(&header, 0, sizeof(header));
    header.flags = SYN;
    header.seq_num = htonl(50);
    header.ack_num = htonl(0);
    header.window_size = htons(1024);
    
    sendto(sockfd, &header, sizeof(header), 0, (const struct sockaddr *)&server_addr, server_len);
    printf("Sent SYN with seq_num: %u\n", ntohl(header.seq_num));

    recvfrom(sockfd, &header, BUFFER_SIZE, 0, (struct sockaddr *)&server_addr, &server_len);
    if ((header.flags & SYN) && (header.flags & ACK)) {
        printf("Received SYN-ACK with seq_num: %u and ack_num: %u\n", 
               ntohl(header.seq_num), ntohl(header.ack_num));

        struct sham_header final_ack_header;
        memset(&final_ack_header, 0, sizeof(final_ack_header));
        
        final_ack_header.flags = ACK;
        final_ack_header.seq_num = htonl(ntohl(header.ack_num));
        final_ack_header.ack_num = htonl(ntohl(header.seq_num) + 1);
        final_ack_header.window_size = htons(1024);

        sendto(sockfd, &final_ack_header, sizeof(final_ack_header), 0, 
               (const struct sockaddr *)&server_addr, server_len);
        printf("Sent final ACK. Handshake complete.\n");
    } else {
        printf("Handshake failed.\n");
        close(sockfd);
        return 1;
    }

    printf("Handshake complete. Starting data transfer.\n");
    send_data(sockfd, &server_addr, server_len);
    
    close(sockfd);
    return 0;
}