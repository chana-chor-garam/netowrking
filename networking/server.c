#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include "headers.h"
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/select.h>
#include <openssl/md5.h> // Include for MD5 functions
#include <stdarg.h>

#define MAX_BUFFER_PACKETS 10
#define RECEIVER_BUFFER_SIZE 8192

double packet_loss_rate = 0.0;
int chat_mode = 0;
FILE *log_file = NULL;

struct buffered_packet {
    struct sham_packet packet;
    int seq_num;
    size_t data_length;
    int is_valid;
};

struct flow_control {
    int buffer_used;
    int buffer_available;
};

int should_drop_packet(void);
void recv_data_chat(int sockfd);
void recv_data_file(int sockfd, const char* output_filename);
void print_usage(const char* program_name);
void send_ack(int sockfd, struct sockaddr_in *client_addr, socklen_t client_len, int ack_num, int window_size);
void calculate_md5_hash(const char* filename);
void log_message(const char *format, ...);

// Function to log messages with high-precision timestamps
void log_message(const char *format, ...) {
    if (!log_file) return;

    struct timeval tv;
    gettimeofday(&tv, NULL);
    time_t curtime = tv.tv_sec;
    char time_buffer[30];
    strftime(time_buffer, 30, "%Y-%m-%d %H:%M:%S", localtime(&curtime));

    fprintf(log_file, "[%s.%06ld] [LOG] ", time_buffer, tv.tv_usec);

    va_list args;
    va_start(args, format);
    vfprintf(log_file, format, args);
    va_end(args);
    
    fflush(log_file);
}

// Function to calculate and print the MD5 hash of a file
void calculate_md5_hash(const char* filename) {
    unsigned char c[MD5_DIGEST_LENGTH];
    int i;
    FILE *inFile = fopen(filename, "rb");
    MD5_CTX mdContext;
    int bytes;
    unsigned char data[1024];

    if (inFile == NULL) {
        perror("Failed to open file for MD5 calculation");
        return;
    }

    MD5_Init(&mdContext);
    while ((bytes = fread(data, 1, 1024, inFile)) != 0) {
        MD5_Update(&mdContext, data, bytes);
    }
    MD5_Final(c, &mdContext);
    
    printf("MD5: ");
    for (i = 0; i < MD5_DIGEST_LENGTH; i++) {
        printf("%02x", c[i]);
    }
    printf("\n");
    fclose(inFile);
}

int should_drop_packet() {
    if (packet_loss_rate <= 0.0) return 0;
    double random_val = (double)rand() / RAND_MAX;
    return random_val < packet_loss_rate;
}

void send_ack(int sockfd, struct sockaddr_in *client_addr, socklen_t client_len, int ack_num, int window_size) {
    struct sham_header ack_header;
    memset(&ack_header, 0, sizeof(ack_header));
    ack_header.flags = ACK;
    ack_header.ack_num = htonl(ack_num);
    ack_header.window_size = htons(window_size);
    if (!should_drop_packet()) {
        sendto(sockfd, &ack_header, sizeof(ack_header), 0, (const struct sockaddr *)client_addr, client_len);
        printf("RCV sends: ACK=%u, Window=%d\n", ack_num, window_size);
        log_message("SND ACK=%u WIN=%d\n", ack_num, window_size);
    } else {
        printf("DROPPED ACK=%u (simulated loss)\n", ack_num);
        log_message("DROP ACK=%u\n", ack_num);
    }
}

void recv_data_chat(int sockfd) {
    struct sockaddr_in client_addr;
    socklen_t client_len = sizeof(client_addr);
    struct sham_packet packet;
    int expected_seq = 1;
    
    struct buffered_packet buffer[MAX_BUFFER_PACKETS];
    for (int i = 0; i < MAX_BUFFER_PACKETS; i++) {
        buffer[i].is_valid = 0;
    }
    
    struct flow_control fc;
    fc.buffer_used = 0;
    fc.buffer_available = RECEIVER_BUFFER_SIZE;
    
    printf("Ready to receive chat messages from the client...\n");
    printf("Receiver buffer size: %d bytes\n", RECEIVER_BUFFER_SIZE);
    if (packet_loss_rate > 0.0) {
        printf("Packet loss rate: %.2f%%\n", packet_loss_rate * 100);
    }
    
    while (1) {
        ssize_t bytes_received = recvfrom(sockfd, &packet, sizeof(struct sham_packet), 0, (struct sockaddr *)&client_addr, &client_len);
        if (bytes_received < 0) {
            continue;
        }
        
        if (should_drop_packet()) {
            printf("DROPPED packet SEQ=%u (simulated loss)\n", ntohl(packet.header.seq_num));
            log_message("DROP DATA SEQ=%u\n", ntohl(packet.header.seq_num));
            continue;
        }
        
        if (packet.header.flags & FIN) {
            printf("Received FIN from client. Starting connection termination.\n");
            log_message("RCV FIN SEQ=%u\n", ntohl(packet.header.seq_num));
            send_ack(sockfd, &client_addr, client_len, ntohl(packet.header.seq_num) + 1, fc.buffer_available);
            log_message("SND ACK FOR FIN\n");
            
            struct sham_header fin_header;
            memset(&fin_header, 0, sizeof(fin_header));
            fin_header.flags = FIN;
            fin_header.seq_num = htonl(expected_seq);
            fin_header.ack_num = htonl(0);
            fin_header.window_size = htons(fc.buffer_available);
            
            int retries = 0;
            while (retries < 5) {
                if (!should_drop_packet()) {
                    sendto(sockfd, &fin_header, sizeof(fin_header), 0, (const struct sockaddr *)&client_addr, client_len);
                    printf("Sent server FIN. Waiting for final ACK.\n");
                    log_message("SND FIN SEQ=%u\n", expected_seq);
                }
                
                struct timeval tv = {1, 0};
                fd_set read_fds;
                FD_ZERO(&read_fds);
                FD_SET(sockfd, &read_fds);
                
                int select_result = select(sockfd + 1, &read_fds, NULL, NULL, &tv);
                if (select_result > 0) {
                    struct sham_header final_ack;
                    recvfrom(sockfd, &final_ack, sizeof(final_ack), 0, (struct sockaddr *)&client_addr, &client_len);
                    if (final_ack.flags & ACK) {
                        printf("Received final ACK. Connection closed gracefully.\n");
                        log_message("RCV ACK\n");
                        goto end_loop;
                    }
                }
                retries++;
            }
            printf("Failed to receive final ACK. Connection may not have closed gracefully.\n");
            log_message("FINAL ACK TIMEOUT\n");

end_loop:
            break;
        }
        
        int received_seq = ntohl(packet.header.seq_num);
        size_t payload_length = bytes_received - sizeof(struct sham_header);
        if (payload_length > 0 && payload_length < sizeof(packet.payload)) {
            packet.payload[payload_length] = '\0';
        }
        
        printf("RCV DATA SEQ=%u, Expected=%u, Length=%zu, Buffer Used=%d, Available=%d\n", received_seq, expected_seq, payload_length, fc.buffer_used, fc.buffer_available);
        log_message("RCV DATA SEQ=%u LEN=%zu\n", received_seq, payload_length);

        if (received_seq == expected_seq) {
            printf("Client: %s\n", packet.payload);
            expected_seq += payload_length;
            
            int found_next;
            do {
                found_next = 0;
                for (int i = 0; i < MAX_BUFFER_PACKETS; i++) {
                    if (buffer[i].is_valid && buffer[i].seq_num == expected_seq) {
                        printf("Processing buffered packet SEQ=%u\n", expected_seq);
                        printf("Client: %s\n", buffer[i].packet.payload);
                        fc.buffer_used -= buffer[i].data_length;
                        fc.buffer_available += buffer[i].data_length;
                        
                        expected_seq += buffer[i].data_length;
                        buffer[i].is_valid = 0;
                        found_next = 1;
                        break;
                    }
                }
            } while (found_next);
            
            send_ack(sockfd, &client_addr, client_len, expected_seq, fc.buffer_available);
        } else if (received_seq > expected_seq) {
            printf("Out-of-order packet SEQ=%u (expecting %u). ", received_seq, expected_seq);
            
            int already_buffered = 0;
            for(int i = 0; i < MAX_BUFFER_PACKETS; i++) {
                if(buffer[i].is_valid && buffer[i].seq_num == received_seq) {
                    already_buffered = 1;
                    break;
                }
            }

            if (!already_buffered && fc.buffer_available >= (int)payload_length) {
                int buffered = 0;
                for (int i = 0; i < MAX_BUFFER_PACKETS; i++) {
                    if (!buffer[i].is_valid) {
                        buffer[i].packet = packet;
                        buffer[i].seq_num = received_seq;
                        buffer[i].data_length = payload_length;
                        buffer[i].is_valid = 1;
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
                 if (already_buffered) {
                     printf("Packet already buffered.\n");
                 } else {
                     printf("Insufficient buffer space (%d bytes needed, %d available), dropping packet\n", (int)payload_length, fc.buffer_available);
                 }
            }
            send_ack(sockfd, &client_addr, client_len, expected_seq, fc.buffer_available);
        } else {
            printf("Duplicate/old packet SEQ=%u (expecting %u). Sending ACK.\n", received_seq, expected_seq);
            send_ack(sockfd, &client_addr, client_len, expected_seq, fc.buffer_available);
        }
    }
}

void recv_data_file(int sockfd, const char* output_filename) {
    struct sockaddr_in client_addr;
    socklen_t client_len = sizeof(client_addr);
    struct sham_packet packet;
    int expected_seq = 1;
    FILE *output_file = NULL;
    
    struct buffered_packet buffer[MAX_BUFFER_PACKETS];
    for (int i = 0; i < MAX_BUFFER_PACKETS; i++) {
        buffer[i].is_valid = 0;
    }
    
    struct flow_control fc;
    fc.buffer_used = 0;
    fc.buffer_available = RECEIVER_BUFFER_SIZE;
    
    printf("Ready to receive file data from the client...\n");
    printf("Receiver buffer size: %d bytes\n", RECEIVER_BUFFER_SIZE);
    if (packet_loss_rate > 0.0) {
        printf("Packet loss rate: %.2f%%\n", packet_loss_rate * 100);
    }
    
    output_file = fopen(output_filename, "wb");
    if (!output_file) {
        perror("Failed to open output file");
        return;
    }
    
    while (1) {
        ssize_t bytes_received = recvfrom(sockfd, &packet, sizeof(struct sham_packet), 0, (struct sockaddr *)&client_addr, &client_len);
        if (bytes_received < 0) {
            continue;
        }
        
        if (should_drop_packet()) {
            printf("DROPPED packet SEQ=%u (simulated loss)\n", ntohl(packet.header.seq_num));
            log_message("DROP DATA SEQ=%u\n", ntohl(packet.header.seq_num));
            continue;
        }
        
        if (packet.header.flags & FIN) {
            printf("Received FIN from client. File transfer complete.\n");
            log_message("RCV FIN SEQ=%u\n", ntohl(packet.header.seq_num));
            
            size_t total_written = 0;
            int found_next;
            do {
                found_next = 0;
                for (int i = 0; i < MAX_BUFFER_PACKETS; i++) {
                    if (buffer[i].is_valid && buffer[i].seq_num == expected_seq) {
                        fwrite(buffer[i].packet.payload, 1, buffer[i].data_length, output_file);
                        total_written += buffer[i].data_length;
                        printf("Wrote %zu buffered bytes to file\n", buffer[i].data_length);
                        fc.buffer_used -= buffer[i].data_length;
                        fc.buffer_available += buffer[i].data_length;
                        expected_seq += buffer[i].data_length;
                        buffer[i].is_valid = 0;
                        found_next = 1;
                        break;
                    }
                }
            } while (found_next);
            
            fflush(output_file);
            fclose(output_file);
            printf("File saved as: %s\n", output_filename);
            calculate_md5_hash(output_filename); // Call the MD5 function here

            send_ack(sockfd, &client_addr, client_len, ntohl(packet.header.seq_num) + 1, fc.buffer_available);
            log_message("SND ACK FOR FIN\n");
            
            struct sham_header fin_header;
            memset(&fin_header, 0, sizeof(fin_header));
            fin_header.flags = FIN;
            fin_header.seq_num = htonl(expected_seq);
            fin_header.ack_num = htonl(0);
            fin_header.window_size = htons(fc.buffer_available);
            
            int retries = 0;
            while (retries < 5) {
                 if (!should_drop_packet()) {
                    sendto(sockfd, &fin_header, sizeof(fin_header), 0, (const struct sockaddr *)&client_addr, client_len);
                    printf("Sent server FIN. Waiting for final ACK.\n");
                    log_message("SND FIN SEQ=%u\n", expected_seq);
                }
                
                struct timeval tv = {1, 0};
                fd_set read_fds;
                FD_ZERO(&read_fds);
                FD_SET(sockfd, &read_fds);
                
                int select_result = select(sockfd + 1, &read_fds, NULL, NULL, &tv);
                if (select_result > 0) {
                    struct sham_header final_ack;
                    recvfrom(sockfd, &final_ack, sizeof(final_ack), 0, (struct sockaddr *)&client_addr, &client_len);
                    if (final_ack.flags & ACK) {
                        printf("Received final ACK. Connection closed gracefully.\n");
                        log_message("RCV ACK\n");
                        goto end_loop;
                    }
                }
                retries++;
            }
            printf("Failed to receive final ACK. Connection may not have closed gracefully.\n");
            log_message("FINAL ACK TIMEOUT\n");
            
end_loop:
            break;
        }
        
        int received_seq = ntohl(packet.header.seq_num);
        size_t payload_length = bytes_received - sizeof(struct sham_header);
        
        printf("RCV DATA SEQ=%u, Expected=%u, Length=%zu, Buffer Used=%d, Available=%d\n", received_seq, expected_seq, payload_length, fc.buffer_used, fc.buffer_available);
        log_message("RCV DATA SEQ=%u LEN=%zu\n", received_seq, payload_length);
        
        if (received_seq == expected_seq) {
            fwrite(packet.payload, 1, payload_length, output_file);
            fflush(output_file);
            printf("Wrote %zu bytes to file\n", payload_length);
            expected_seq += payload_length;
            
            int found_next;
            do {
                found_next = 0;
                for (int i = 0; i < MAX_BUFFER_PACKETS; i++) {
                    if (buffer[i].is_valid && buffer[i].seq_num == expected_seq) {
                        printf("Processing buffered packet SEQ=%u\n", expected_seq);
                        fwrite(buffer[i].packet.payload, 1, buffer[i].data_length, output_file);
                        fflush(output_file);
                        printf("Wrote %zu buffered bytes to file\n", buffer[i].data_length);
                        fc.buffer_used -= buffer[i].data_length;
                        fc.buffer_available += buffer[i].data_length;
                        expected_seq += buffer[i].data_length;
                        buffer[i].is_valid = 0;
                        found_next = 1;
                        break;
                    }
                }
            } while (found_next);
            
            send_ack(sockfd, &client_addr, client_len, expected_seq, fc.buffer_available);
        } else if (received_seq > expected_seq) {
            printf("Out-of-order packet SEQ=%u (expecting %u). ", received_seq, expected_seq);
            int already_buffered = 0;
            for(int i = 0; i < MAX_BUFFER_PACKETS; i++) {
                if(buffer[i].is_valid && buffer[i].seq_num == received_seq) {
                    already_buffered = 1;
                    break;
                }
            }
            if (!already_buffered && fc.buffer_available >= (int)payload_length) {
                int buffered = 0;
                for (int i = 0; i < MAX_BUFFER_PACKETS; i++) {
                    if (!buffer[i].is_valid) {
                        buffer[i].packet = packet;
                        buffer[i].seq_num = received_seq;
                        buffer[i].data_length = payload_length;
                        buffer[i].is_valid = 1;
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
                 if (already_buffered) {
                     printf("Packet already buffered.\n");
                 } else {
                     printf("Insufficient buffer space (%d bytes needed, %d available), dropping packet\n", (int)payload_length, fc.buffer_available);
                 }
            }
            send_ack(sockfd, &client_addr, client_len, expected_seq, fc.buffer_available);
        } else {
            printf("Duplicate/old packet SEQ=%u (expecting %u). Sending ACK.\n", received_seq, expected_seq);
            send_ack(sockfd, &client_addr, client_len, expected_seq, fc.buffer_available);
        }
    }
}

void print_usage(const char* program_name) {
    printf("Usage: %s <port> [--chat] [loss_rate]\n", program_name);
    printf("  port: Port number to listen on\n");
    printf("  --chat: Enable chat mode (optional)\n");
    printf("  loss_rate: Packet loss probability 0.0-1.0 (optional, default: 0.0)\n");
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        print_usage(argv[0]);
        return 1;
    }
    
    int server_port = atoi(argv[1]);
    if (server_port <= 0) {
        printf("Error: Invalid port number\n");
        return 1;
    }
    
    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--chat") == 0) {
            chat_mode = 1;
        } else {
            double loss_rate = atof(argv[i]);
            if (loss_rate >= 0.0 && loss_rate <= 1.0) {
                packet_loss_rate = loss_rate;
            } else {
                printf("Warning: Invalid loss rate %s, using default 0.0\n", argv[i]);
            }
        }
    }
    
    if (getenv("RUDP_LOG") != NULL) {
        log_file = fopen("server_log.txt", "a");
        if (!log_file) {
            perror("Failed to open log file");
            return 1;
        }
        printf("Logging enabled: writing to server_log.txt\n");
    }
    
    srand(time(NULL));
    
    int sockfd;
    struct sockaddr_in server_addr, client_addr;
    socklen_t client_len = sizeof(client_addr);
    struct sham_header header;

    if ((sockfd = socket(AF_INET, SOCK_DGRAM, 0)) < 0) {
        perror("socket creation failed");
        if (log_file) fclose(log_file);
        exit(EXIT_FAILURE);
    }

    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(server_port);

    if (bind(sockfd, (const struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        perror("bind failed");
        if (log_file) fclose(log_file);
        exit(EXIT_FAILURE);
    }

    printf("Server listening on port %d...\n", server_port);
    printf("Mode: %s\n", chat_mode ? "Chat" : "File Transfer");
    if (packet_loss_rate > 0.0) {
        printf("Packet loss rate: %.2f%%\n", packet_loss_rate * 100);
    }

    printf("Waiting for SYN from client...\n");
    recvfrom(sockfd, &header, sizeof(header), 0, (struct sockaddr *)&client_addr, &client_len);

    if (header.flags & SYN) {
        printf("Received SYN with seq_num: %u\n", ntohl(header.seq_num));
        log_message("RCV SYN SEQ=%u\n", ntohl(header.seq_num));

        struct sham_header syn_ack_header;
        memset(&syn_ack_header, 0, sizeof(syn_ack_header));
        syn_ack_header.flags = SYN | ACK;
        syn_ack_header.seq_num = htonl(100);
        syn_ack_header.ack_num = htonl(ntohl(header.seq_num) + 1);
        syn_ack_header.window_size = htons(RECEIVER_BUFFER_SIZE);
        if (!should_drop_packet()) {
            sendto(sockfd, &syn_ack_header, sizeof(syn_ack_header), 0, (const struct sockaddr *)&client_addr, client_len);
            printf("Sent SYN-ACK with seq_num: %u, ack_num: %u, window: %d\n", ntohl(syn_ack_header.seq_num), ntohl(syn_ack_header.ack_num), RECEIVER_BUFFER_SIZE);
            log_message("SND SYN-ACK SEQ=%u ACK=%u\n", ntohl(syn_ack_header.seq_num), ntohl(syn_ack_header.ack_num));
        } else {
            printf("DROPPED SYN-ACK (simulated loss)\n");
            log_message("DROP SYN-ACK\n");
        }

        recvfrom(sockfd, &header, sizeof(header), 0, (struct sockaddr *)&client_addr, &client_len);
        if ((header.flags & ACK) && (ntohl(header.ack_num) == ntohl(syn_ack_header.seq_num) + 1)) {
            printf("Received final ACK. Handshake complete.\n");
            log_message("RCV ACK FOR SYN\n");
        } else {
            printf("Handshake failed.\n");
        }
    } else {
        printf("Expected SYN, but received different flag.\n");
    }

    printf("Handshake complete. Starting data transfer.\n");
    if (chat_mode) {
        recv_data_chat(sockfd);
    } else {
        recv_data_file(sockfd, "received_file.dat");
    }
    
    printf("Server shutting down.\n");
    close(sockfd);
    if (log_file) fclose(log_file);
    return 0;
}