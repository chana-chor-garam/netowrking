#include"headers.h"

void receive_and_reassemble(int sockfd, struct sockaddr_in *client_addr) {
    PacketBuffer buffer[FIXED_WINDOW_SIZE];
    uint32_t expected_seq_num = 1;
    
    for (int i = 0; i < FIXED_WINDOW_SIZE; i++) {
        buffer[i].is_valid = 0;
    }

    while (1) {
        char packet_buffer[sizeof(struct sham_header) + MAX_PAYLOAD_SIZE];
        socklen_t client_len = sizeof(*client_addr);
        int bytes_received = recvfrom(sockfd, packet_buffer, sizeof(packet_buffer), 0, (struct sockaddr *)client_addr, &client_len);

        if (bytes_received <= 0) {
            continue;
        }

        struct sham_header received_header;
        memcpy(&received_header, packet_buffer, sizeof(struct sham_header));
        uint32_t received_seq_num = ntohl(received_header.seq_num);
        int payload_len = bytes_received - sizeof(struct sham_header);
        
        printf("RCV DATA SEQ=%u\n", received_seq_num);

        // Check if the packet is the one we expect
        if (received_seq_num == expected_seq_num) {
            // Process the in-order packet
            // (e.g., write to a file or print to the console)
            printf("Processing in-order packet with SEQ=%u\n", received_seq_num);
            
            // Advance the expected sequence number
            expected_seq_num += payload_len;

            // Check buffered packets to see if they can now be processed
            for (int i = 0; i < FIXED_WINDOW_SIZE; i++) {
                if (buffer[i].is_valid && ntohl(buffer[i].header.seq_num) == expected_seq_num) {
                    printf("Processing buffered packet with SEQ=%u\n", ntohl(buffer[i].header.seq_num));
                    expected_seq_num += buffer[i].payload_len;
                    buffer[i].is_valid = 0;
                }
            }
        } 
        // Check if the packet is out-of-order but within the window
        else if (received_seq_num > expected_seq_num && received_seq_num < expected_seq_num + (MAX_PAYLOAD_SIZE * FIXED_WINDOW_SIZE)) {
            // Buffer the out-of-order packet
            int index = (received_seq_num - expected_seq_num) / MAX_PAYLOAD_SIZE;
            if (index >= 0 && index < FIXED_WINDOW_SIZE && !buffer[index].is_valid) {
                 memcpy(&buffer[index].header, &received_header, sizeof(struct sham_header));
                 memcpy(buffer[index].payload, packet_buffer + sizeof(struct sham_header), payload_len);
                 buffer[index].payload_len = payload_len;
                 buffer[index].is_valid = 1;
                 printf("Buffering out-of-order packet with SEQ=%u\n", received_seq_num);
            }
        }

        // Send a cumulative ACK for the highest in-order sequence number
        struct sham_header ack_header;
        ack_header.flags = ACK;
        ack_header.seq_num = 0;
        ack_header.ack_num = htonl(expected_seq_num);
        sendto(sockfd, &ack_header, sizeof(ack_header), 0, (struct sockaddr *)client_addr, sizeof(*client_addr));
        printf("SND ACK=%u\n", expected_seq_num);
    }
}