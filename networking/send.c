#include"headers.h"

void send_file_with_sliding_window(int sockfd, struct sockaddr_in *server_addr, const char* filename) {
    FILE* fp = fopen(filename, "r");
    if (!fp) {
        perror("File open failed");
        return;
    }

    InFlightPacket window[FIXED_WINDOW_SIZE];
    int window_start = 0; // Index of the first unacknowledged packet
    int window_end = 0;   // Index of the next available slot

    uint32_t next_seq_num = 1; // Start with 1, as the handshake uses X+1

    while (1) {
        // --- Send new packets as long as the window isn't full ---
        while (window_end - window_start < FIXED_WINDOW_SIZE) {
            char payload_buffer[MAX_PAYLOAD_SIZE];
            int bytes_read = fread(payload_buffer, 1, MAX_PAYLOAD_SIZE, fp);
            if (bytes_read <= 0) {
                break; // End of file
            }

            // Create and store the new packet
            InFlightPacket* packet = &window[window_end % FIXED_WINDOW_SIZE];
            packet->header.seq_num = htonl(next_seq_num);
            packet->header.ack_num = 0;
            packet->header.flags = 0;
            packet->header.window_size = 0;
            memcpy(packet->payload, payload_buffer, bytes_read);
            packet->payload_len = bytes_read;
            packet->is_acked = 0;

            // Send the packet and record its sent time
            sendto(sockfd, (const char*)&packet->header, sizeof(struct sham_header) + packet->payload_len, 0, (struct sockaddr*)server_addr, sizeof(*server_addr));
            gettimeofday(&packet->sent_time, NULL);
            
            printf("SND DATA SEQ=%u\n", next_seq_num);
            
            next_seq_num += bytes_read;
            window_end++;
        }

        // --- Check for ACKs and Retransmissions ---
        fd_set read_fds;
        struct timeval timeout;
        
        FD_ZERO(&read_fds);
        FD_SET(sockfd, &read_fds);
        timeout.tv_sec = 0;
        timeout.tv_usec = RTO_MS * 1000;

        int ret = select(sockfd + 1, &read_fds, NULL, NULL, &timeout);

        if (ret > 0 && FD_ISSET(sockfd, &read_fds)) {
            // Received an ACK, process it
            struct sham_header ack_header;
            recvfrom(sockfd, &ack_header, sizeof(ack_header), 0, NULL, NULL);

            if (ack_header.flags & ACK) {
                uint32_t ack_num = ntohl(ack_header.ack_num);
                printf("RCV ACK=%u\n", ack_num);

                // Slide the window forward based on the cumulative ACK
                while (window_start < window_end && ntohl(window[window_start % FIXED_WINDOW_SIZE].header.seq_num) + window[window_start % FIXED_WINDOW_SIZE].payload_len <= ack_num) {
                    window[window_start % FIXED_WINDOW_SIZE].is_acked = 1;
                    window_start++;
                }
            }
        } else if (ret == 0) {
            // Timeout, check for packets to retransmit
            for (int i = window_start; i < window_end; i++) {
                InFlightPacket* packet = &window[i % FIXED_WINDOW_SIZE];
                struct timeval now;
                gettimeofday(&now, NULL);
                
                long elapsed_ms = (now.tv_sec - packet->sent_time.tv_sec) * 1000 + (now.tv_usec - packet->sent_time.tv_usec) / 1000;
                
                if (!packet->is_acked && elapsed_ms > RTO_MS) {
                    printf("TIMEOUT SEQ=%u\n", ntohl(packet->header.seq_num));
                    sendto(sockfd, (const char*)&packet->header, sizeof(struct sham_header) + packet->payload_len, 0, (struct sockaddr*)server_addr, sizeof(*server_addr));
                    gettimeofday(&packet->sent_time, NULL); // Restart the timer
                    printf("RETX DATA SEQ=%u\n", ntohl(packet->header.seq_num));
                }
            }
        }

        // Break if all data is sent and acknowledged
        if (feof(fp) && window_start == window_end) {
            break;
        }
    }

    fclose(fp);
}