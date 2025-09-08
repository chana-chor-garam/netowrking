#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "headers.h"
#include <sys/socket.h>

#define SERVER_PORT 8888
#define BUFFER_SIZE sizeof(struct sham_header)
#define MAX_BUFFER_PACKETS 10
#define RECEIVER_BUFFER_SIZE 8192  // Total receiver buffer size in bytes

// Structure to buffer out-of-order packets
struct buffered_packet {
    struct sham_packet packet;
    int seq_num;
    size_t data_length;
    int is_valid;
};

// Flow control state
struct flow_control {
    int buffer_used;           // Bytes currently buffered
    int buffer_available;      // Available buffer space
    int last_byte_received;    // Last byte successfully received and processed
    int last_byte_read;        // Last byte delivered to application
};

void recv_data(int sockfd) {
    struct sockaddr_in client_addr;
    socklen_t client_len = sizeof(client_addr);
    struct sham_packet packet;
    int expected_seq = 1;
    
    // Buffer for out-of-order packets
    struct buffered_packet buffer[MAX_BUFFER_PACKETS];
    for (int i = 0; i < MAX_BUFFER_PACKETS; i++) {
        buffer[i].is_valid = 0;
    }
    
    // Initialize flow control
    struct flow_control fc;
    fc.buffer_used = 0;
    fc.buffer_available = RECEIVER_BUFFER_SIZE;
    fc.last_byte_received = 0;
    fc.last_byte_read = 0;
    
    printf("Ready to receive data from the client...\n");
    printf("Receiver buffer size: %d bytes\n", RECEIVER_BUFFER_SIZE);
    
    while (1) {
        ssize_t bytes_received = recvfrom(sockfd, &packet, sizeof(struct sham_packet), 0, 
                                        (struct sockaddr *)&client_addr, &client_len);
        if (bytes_received < 0) {
            continue; // Keep waiting
        }
        
        // Check for the FIN flag to terminate the data transfer
        if (packet.header.flags & FIN) {
            printf("Received FIN from client. Starting connection termination.\n");
            
            // Step 1: Send ACK for client's FIN
            struct sham_header ack_header;
            memset(&ack_header, 0, sizeof(ack_header));
            
            ack_header.flags = ACK;
            ack_header.seq_num = htonl(expected_seq);
            ack_header.ack_num = htonl(ntohl(packet.header.seq_num) + 1);
            ack_header.window_size = htons(fc.buffer_available);
            sendto(sockfd, &ack_header, sizeof(ack_header), 0, (const struct sockaddr *)&client_addr, client_len);
            printf("Sent ACK for client's FIN.\n");
            
            // Step 2: Send Server's own FIN
            struct sham_header fin_header;
            memset(&fin_header, 0, sizeof(fin_header));
            
            fin_header.flags = FIN;
            fin_header.seq_num = htonl(expected_seq);
            fin_header.ack_num = htonl(0);
            fin_header.window_size = htons(fc.buffer_available);
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
        
        int received_seq = ntohl(packet.header.seq_num);
        size_t payload_length = bytes_received - sizeof(struct sham_header);
        
        // Ensure null termination for safety
        if (payload_length > 0 && payload_length < sizeof(packet.payload)) {
            packet.payload[payload_length] = '\0';
        }
        
        printf("RCV DATA SEQ=%u, Expected=%u, Length=%zu, Buffer Used=%d, Available=%d\n", 
               received_seq, expected_seq, payload_length, fc.buffer_used, fc.buffer_available);
        
        if (received_seq == expected_seq) {
            // In-order packet - process it immediately
            printf("Received: %s\n", packet.payload);
            expected_seq += payload_length;
            fc.last_byte_received = expected_seq - 1;
            fc.last_byte_read = expected_seq - 1;  // Simulate immediate processing
            
            // Check if we can now process any buffered packets
            int found_next = 1;
            while (found_next) {
                found_next = 0;
                for (int i = 0; i < MAX_BUFFER_PACKETS; i++) {
                    if (buffer[i].is_valid && buffer[i].seq_num == expected_seq) {
                        printf("Processing buffered packet SEQ=%u\n", expected_seq);
                        printf("Received: %s\n", buffer[i].packet.payload);
                        
                        // Update flow control - remove from buffer
                        fc.buffer_used -= buffer[i].data_length;
                        fc.buffer_available += buffer[i].data_length;
                        
                        expected_seq += buffer[i].data_length;
                        fc.last_byte_received = expected_seq - 1;
                        fc.last_byte_read = expected_seq - 1;  // Simulate immediate processing
                        
                        buffer[i].is_valid = 0;
                        found_next = 1;
                        break;
                    }
                }
            }
            
            // Send cumulative ACK for all processed data
            struct sham_header ack_header;
            memset(&ack_header, 0, sizeof(ack_header));
            
            ack_header.flags = ACK;
            ack_header.seq_num = htonl(expected_seq);
            ack_header.ack_num = htonl(expected_seq);
            ack_header.window_size = htons(fc.buffer_available);
            sendto(sockfd, &ack_header, sizeof(ack_header), 0, (const struct sockaddr *)&client_addr, client_len);
            printf("RCV sends: ACK=%u, Window=%d\n", expected_seq, fc.buffer_available);
            
        } else if (received_seq > expected_seq) {
            // Out-of-order packet (future packet) - check if we can buffer it
            printf("Out-of-order packet SEQ=%u (expecting %u). ", received_seq, expected_seq);
            
            // Check if we have buffer space
            if (fc.buffer_available >= (int)payload_length) {
                // Find empty slot in buffer
                int buffered = 0;
                for (int i = 0; i < MAX_BUFFER_PACKETS; i++) {
                    if (!buffer[i].is_valid) {
                        buffer[i].packet = packet;
                        buffer[i].seq_num = received_seq;
                        buffer[i].data_length = payload_length;
                        buffer[i].is_valid = 1;
                        
                        // Update flow control
                        fc.buffer_used += payload_length;
                        fc.buffer_available -= payload_length;
                        
                        buffered = 1;
                        printf("Buffering at slot %d\n", i);
                        break;
                    }
                }
                
                if (!buffered) {
                    printf("Warning: Buffer slots full, dropping packet SEQ=%u\n", received_seq);
                }
            } else {
                printf("Insufficient buffer space (%d bytes needed, %d available), dropping packet\n", 
                       (int)payload_length, fc.buffer_available);
            }
            
            // Send ACK for the last contiguous sequence we have
            struct sham_header ack_header;
            memset(&ack_header, 0, sizeof(ack_header));
            
            ack_header.flags = ACK;
            ack_header.seq_num = htonl(expected_seq);
            ack_header.ack_num = htonl(expected_seq);
            ack_header.window_size = htons(fc.buffer_available);
            sendto(sockfd, &ack_header, sizeof(ack_header), 0, (const struct sockaddr *)&client_addr, client_len);
            printf("RCV sends: ACK=%u, Window=%d (duplicate for missing data)\n", expected_seq, fc.buffer_available);
            
        } else {
            // Old/duplicate packet - just ACK it
            printf("Duplicate/old packet SEQ=%u (expecting %u). Sending ACK.\n", received_seq, expected_seq);
            
            struct sham_header ack_header;
            memset(&ack_header, 0, sizeof(ack_header));
            
            ack_header.flags = ACK;
            ack_header.seq_num = htonl(expected_seq);
            ack_header.ack_num = htonl(expected_seq);
            ack_header.window_size = htons(fc.buffer_available);
            sendto(sockfd, &ack_header, sizeof(ack_header), 0, (const struct sockaddr *)&client_addr, client_len);
            printf("RCV sends: ACK=%u, Window=%d\n", expected_seq, fc.buffer_available);
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

        // --- Handshake Step 2: Send SYN-ACK with initial window size ---
        struct sham_header syn_ack_header;
        memset(&syn_ack_header, 0, sizeof(syn_ack_header));
        
        syn_ack_header.flags = SYN | ACK;
        syn_ack_header.seq_num = htonl(100);
        syn_ack_header.ack_num = htonl(ntohl(header.seq_num) + 1);
        syn_ack_header.window_size = htons(RECEIVER_BUFFER_SIZE);  // Advertise initial window

        sendto(sockfd, &syn_ack_header, sizeof(syn_ack_header), 0, (const struct sockaddr *)&client_addr, client_len);
        printf("Sent SYN-ACK with seq_num: %u, ack_num: %u, window: %d\n", 
               ntohl(syn_ack_header.seq_num), ntohl(syn_ack_header.ack_num), RECEIVER_BUFFER_SIZE);

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
    recv_data(sockfd);
    
    printf("Server shutting down.\n");
    close(sockfd);
    return 0;
}