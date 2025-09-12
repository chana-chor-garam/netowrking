#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <math.h>
#include <time.h>
#include "headers.h"
#include <sys/socket.h>
#include <sys/time.h>
#include <arpa/inet.h>
#include <sys/select.h>
#include <stdarg.h>

#define PAYLOAD_SIZE 1024
#define WINDOW_SIZE 4 // Max number of unacknowledged packets in flight

// Global variables for packet loss simulation
double packet_loss_rate = 0.0;
int chat_mode = 0;
FILE *log_file = NULL;

// RTO constants and variables
#define ALPHA 0.25
#define BETA 0.75
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

// Flow control state
struct flow_control {
    int last_byte_sent;
    int last_byte_acked;
    int receiver_window;
};

// Function declarations
void send_termination_sequence(int sockfd, struct sockaddr_in *server_addr, socklen_t server_len, int next_seq_num);
void send_data_chat(int sockfd, struct sockaddr_in *server_addr, socklen_t server_len);
void send_data_file(int sockfd, struct sockaddr_in *server_addr, socklen_t server_len, const char* filename);
void print_usage(const char* program_name);
int should_drop_packet(void);
void log_message(const char *format, ...);

// Function to log messages with high-precision timestamps
void log_message(const char *format, ...) {
    if (!log_file) return; // Ensure logging is active only when log_file is open

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

// Simulate packet loss
int should_drop_packet() {
    if (packet_loss_rate <= 0.0) return 0;
    double random_val = (double)rand() / RAND_MAX;
    return random_val < packet_loss_rate;
}

void send_data_chat(int sockfd, struct sockaddr_in *server_addr, socklen_t server_len) {
    int next_seq_num = 1;
    int base_seq_num = 1;
    struct sent_packet window[WINDOW_SIZE];
    int window_start = 0;
    int window_count = 0;

    struct flow_control fc;
    fc.last_byte_sent = 0;
    fc.last_byte_acked = 0;
    fc.receiver_window = 1024;

    for (int i = 0; i < WINDOW_SIZE; i++) {
        window[i].is_valid = 0;
    }

    printf("Enter chat messages (type '/quit' to exit):\n");
    if (packet_loss_rate > 0.0) {
        printf("Packet loss rate: %.2f%%\n", packet_loss_rate * 100);
    }

    while (1) {
        // Step 1: Check for incoming ACKs and timeouts
        fd_set read_fds;
        struct timeval tv, current_time;

        if (window_count > 0) {
            gettimeofday(&current_time, NULL);
            double elapsed = (current_time.tv_sec - window[window_start].sent_time.tv_sec) * 1000.0 +
                             (current_time.tv_usec - window[window_start].sent_time.tv_usec) / 1000.0;

            if (elapsed >= RTO) {
                printf("TIMEOUT! Retransmitting SEQ=%u\n", window[window_start].seq_num);
                log_message("TIMEOUT SEQ=%u\n", window[window_start].seq_num);
                ssize_t bytes_to_send = sizeof(struct sham_header) + window[window_start].data_length;
                if (!should_drop_packet()) {
                    sendto(sockfd, &window[window_start].packet, bytes_to_send, 0, (const struct sockaddr *)server_addr, server_len);
                    log_message("RETX DATA SEQ=%u LEN=%zu\n", window[window_start].seq_num, window[window_start].data_length);
                } else {
                    printf("DROPPED retransmission SEQ=%u (simulated loss)\n", window[window_start].seq_num);
                    log_message("DROP DATA SEQ=%u\n", window[window_start].seq_num);
                }
                gettimeofday(&window[window_start].sent_time, NULL);
                RTO *= 2;
                if (RTO > 5000) RTO = 5000;
                continue;
            }
            tv.tv_sec = (long)(RTO - elapsed) / 1000;
            tv.tv_usec = ((long)(RTO - elapsed) % 1000) * 1000;
        } else {
            tv.tv_sec = 1;
            tv.tv_usec = 0;
        }

        FD_ZERO(&read_fds);
        FD_SET(sockfd, &read_fds);

        int select_result = select(sockfd + 1, &read_fds, NULL, NULL, &tv);

        if (select_result > 0) {
            struct sham_header ack_header;
            recvfrom(sockfd, &ack_header, sizeof(ack_header), 0, NULL, NULL);

            if (ack_header.flags & ACK) {
                uint32_t ack_num = ntohl(ack_header.ack_num);
                fc.receiver_window = ntohs(ack_header.window_size);

                printf("Received ACK=%u, Receiver Window=%d\n", ack_num, fc.receiver_window);
                log_message("RCV ACK=%u\n", ack_num);

                // Update RTT/RTO only for non-retransmitted packets
                if (window_count > 0 && window[window_start].is_valid) {
                    gettimeofday(&current_time, NULL);
                    double SampleRTT = (current_time.tv_sec - window[window_start].sent_time.tv_sec) * 1000.0 +
                                       (current_time.tv_usec - window[window_start].sent_time.tv_usec) / 1000.0;

                    EstimatedRTT = (1 - ALPHA) * EstimatedRTT + ALPHA * SampleRTT;
                    DevRTT = (1 - BETA) * DevRTT + BETA * fabs(SampleRTT - EstimatedRTT);
                    RTO = EstimatedRTT + 4 * DevRTT;

                    if (RTO < 100) RTO = 100;
                    if (RTO > 5000) RTO = 5000;
                }

                // Slide the window
                while (window_count > 0 && (uint32_t)(window[window_start].seq_num + window[window_start].data_length) <= ack_num) {
                    printf("Packet SEQ=%u acknowledged\n", window[window_start].seq_num);
                    fc.last_byte_acked = window[window_start].seq_num + window[window_start].data_length - 1;
                    window[window_start].is_valid = 0;
                    window_start = (window_start + 1) % WINDOW_SIZE;
                    window_count--;
                }

                printf("Flow control update: Bytes in flight = %d, Receiver window = %d\n", fc.last_byte_sent - fc.last_byte_acked, fc.receiver_window);
                log_message("FLOW WIN UPDATE=%u\n", fc.receiver_window);
            }
        } else if (select_result < 0) {
            perror("select error");
            break;
        }

        // Step 2: Read input and send new packets if the window has space
        int bytes_in_flight = fc.last_byte_sent - fc.last_byte_acked;
        while (window_count < WINDOW_SIZE && bytes_in_flight < fc.receiver_window) {
            char payload[PAYLOAD_SIZE];
            printf("You: ");
            fflush(stdout);

            if (fgets(payload, PAYLOAD_SIZE, stdin) == NULL) {
                goto end_data_transfer;
            }
            if (strcmp(payload, "/quit\n") == 0) {
                goto end_data_transfer;
            }

            size_t payload_len = strlen(payload);
            if (payload_len > 0 && payload[payload_len - 1] == '\n') {
                payload[payload_len - 1] = '\0';
                payload_len--;
            }
            if (payload_len == 0) continue;

            size_t packet_data_len = payload_len + 1;
            int slot = (window_start + window_count) % WINDOW_SIZE;

            memset(&window[slot], 0, sizeof(struct sent_packet));
            window[slot].is_valid = 1;
            window[slot].seq_num = next_seq_num;
            window[slot].data_length = packet_data_len;

            window[slot].packet.header.flags = 0;
            window[slot].packet.header.seq_num = htonl(next_seq_num);
            window[slot].packet.header.ack_num = htonl(0);
            window[slot].packet.header.window_size = htons(1024);

            memcpy(window[slot].packet.payload, payload, payload_len);
            window[slot].packet.payload[payload_len] = '\0';

            ssize_t bytes_to_send = sizeof(struct sham_header) + packet_data_len;
            if (!should_drop_packet()) {
                sendto(sockfd, &window[slot].packet, bytes_to_send, 0, (const struct sockaddr *)server_addr, server_len);
                gettimeofday(&window[slot].sent_time, NULL);
                printf("SND DATA SEQ=%u, Bytes in flight: %d, Receiver window: %d\n", next_seq_num, bytes_in_flight + (int)packet_data_len, fc.receiver_window);
                log_message("SND DATA SEQ=%u LEN=%zu\n", next_seq_num, packet_data_len);
            } else {
                printf("DROPPED packet SEQ=%u (simulated loss)\n", next_seq_num);
                log_message("DROP DATA SEQ=%u\n", next_seq_num);
                gettimeofday(&window[slot].sent_time, NULL);
            }

            fc.last_byte_sent = next_seq_num + packet_data_len - 1;
            next_seq_num += packet_data_len;
            window_count++;
            bytes_in_flight = fc.last_byte_sent - fc.last_byte_acked;
        }

        if (window_count == 0 && next_seq_num > 1) {
            break; // All data sent and acknowledged
        }
    }

end_data_transfer:
    send_termination_sequence(sockfd, server_addr, server_len, next_seq_num);
}

void send_data_file(int sockfd, struct sockaddr_in *server_addr, socklen_t server_len, const char* filename) {
    FILE *input_file = fopen(filename, "rb");
    if (!input_file) {
        perror("Failed to open input file");
        return;
    }
    
    int next_seq_num = 1;
    struct sent_packet window[WINDOW_SIZE];
    int window_start = 0;
    int window_count = 0;
    int file_finished = 0;
    
    struct flow_control fc;
    fc.last_byte_sent = 0;
    fc.last_byte_acked = 0;
    fc.receiver_window = 1024;
    
    for (int i = 0; i < WINDOW_SIZE; i++) {
        window[i].is_valid = 0;
    }
    
    printf("Starting file transfer: %s\n", filename);
    if (packet_loss_rate > 0.0) {
        printf("Packet loss rate: %.2f%%\n", packet_loss_rate * 100);
    }
    
    while (!file_finished || window_count > 0) {
        fd_set read_fds;
        struct timeval tv, current_time;
        
        if (window_count > 0) {
            gettimeofday(&current_time, NULL);
            double elapsed = (current_time.tv_sec - window[window_start].sent_time.tv_sec) * 1000.0 +
                             (current_time.tv_usec - window[window_start].sent_time.tv_usec) / 1000.0;
            
            if (elapsed >= RTO) {
                printf("TIMEOUT! Retransmitting SEQ=%u\n", window[window_start].seq_num);
                log_message("TIMEOUT SEQ=%u\n", window[window_start].seq_num);
                ssize_t bytes_to_send = sizeof(struct sham_header) + window[window_start].data_length;
                if (!should_drop_packet()) {
                    sendto(sockfd, &window[window_start].packet, bytes_to_send, 0, (const struct sockaddr *)server_addr, server_len);
                    log_message("RETX DATA SEQ=%u LEN=%zu\n", window[window_start].seq_num, window[window_start].data_length);
                } else {
                    printf("DROPPED retransmission SEQ=%u (simulated loss)\n", window[window_start].seq_num);
                    log_message("DROP DATA SEQ=%u\n", window[window_start].seq_num);
                }
                gettimeofday(&window[window_start].sent_time, NULL);
                RTO *= 2;
                if (RTO > 5000) RTO = 5000;
                continue;
            }
            tv.tv_sec = (long)(RTO - elapsed) / 1000;
            tv.tv_usec = ((long)(RTO - elapsed) % 1000) * 1000;
        } else {
            tv.tv_sec = 1;
            tv.tv_usec = 0;
        }
        
        FD_ZERO(&read_fds);
        FD_SET(sockfd, &read_fds);
        
        int select_result = select(sockfd + 1, &read_fds, NULL, NULL, &tv);

        if (select_result > 0) {
            struct sham_header ack_header;
            recvfrom(sockfd, &ack_header, sizeof(ack_header), 0, NULL, NULL);
            
            if (ack_header.flags & ACK) {
                uint32_t ack_num = ntohl(ack_header.ack_num);
                fc.receiver_window = ntohs(ack_header.window_size);
                
                printf("Received ACK=%u, Receiver Window=%d\n", ack_num, fc.receiver_window);
                log_message("RCV ACK=%u\n", ack_num);
                
                if (window_count > 0 && window[window_start].is_valid) {
                     gettimeofday(&current_time, NULL);
                     double SampleRTT = (current_time.tv_sec - window[window_start].sent_time.tv_sec) * 1000.0 + 
                                       (current_time.tv_usec - window[window_start].sent_time.tv_usec) / 1000.0;
                     
                     EstimatedRTT = (1 - ALPHA) * EstimatedRTT + ALPHA * SampleRTT;
                     DevRTT = (1 - BETA) * DevRTT + BETA * fabs(SampleRTT - EstimatedRTT);
                     RTO = EstimatedRTT + 4 * DevRTT;
                     
                     if (RTO < 100) RTO = 100;
                     if (RTO > 5000) RTO = 5000;
                }
                
                while (window_count > 0 && (uint32_t)(window[window_start].seq_num + window[window_start].data_length) <= ack_num) {
                    printf("Packet SEQ=%u acknowledged\n", window[window_start].seq_num);
                    fc.last_byte_acked = window[window_start].seq_num + window[window_start].data_length - 1;
                    window[window_start].is_valid = 0;
                    window_start = (window_start + 1) % WINDOW_SIZE;
                    window_count--;
                }
                
                printf("Flow control update: Bytes in flight = %d, Receiver window = %d\n", fc.last_byte_sent - fc.last_byte_acked, fc.receiver_window);
                log_message("FLOW WIN UPDATE=%u\n", fc.receiver_window);
            }
        } else if (select_result < 0) {
            perror("select error");
            break;
        }

        int bytes_in_flight = fc.last_byte_sent - fc.last_byte_acked;
        while (window_count < WINDOW_SIZE && !file_finished && bytes_in_flight < fc.receiver_window) {
            char payload[PAYLOAD_SIZE];
            size_t bytes_read = fread(payload, 1, PAYLOAD_SIZE, input_file);
            
            if (bytes_read == 0) {
                file_finished = 1;
                break;
            }
            
            size_t packet_data_len = bytes_read;
            int slot = (window_start + window_count) % WINDOW_SIZE;
            
            memset(&window[slot], 0, sizeof(struct sent_packet));
            window[slot].is_valid = 1;
            window[slot].seq_num = next_seq_num;
            window[slot].data_length = packet_data_len;
            
            window[slot].packet.header.flags = 0;
            window[slot].packet.header.seq_num = htonl(next_seq_num);
            window[slot].packet.header.ack_num = htonl(0);
            window[slot].packet.header.window_size = htons(1024);
            
            memcpy(window[slot].packet.payload, payload, bytes_read);
            
            ssize_t bytes_to_send = sizeof(struct sham_header) + bytes_read;
            if (!should_drop_packet()) {
                sendto(sockfd, &window[slot].packet, bytes_to_send, 0, (const struct sockaddr *)server_addr, server_len);
                gettimeofday(&window[slot].sent_time, NULL);
                printf("SND DATA SEQ=%u, Size=%zu, Bytes in flight: %d, Receiver window: %d\n", next_seq_num, bytes_read, bytes_in_flight + (int)bytes_read, fc.receiver_window);
                log_message("SND DATA SEQ=%u LEN=%zu\n", next_seq_num, bytes_read);
            } else {
                printf("DROPPED packet SEQ=%u (simulated loss)\n", next_seq_num);
                log_message("DROP DATA SEQ=%u\n", next_seq_num);
                gettimeofday(&window[slot].sent_time, NULL);
            }
            
            fc.last_byte_sent = next_seq_num + bytes_read - 1;
            next_seq_num += bytes_read;
            window_count++;
            bytes_in_flight = fc.last_byte_sent - fc.last_byte_acked;
        }
    }
    
    fclose(input_file);
    printf("File transfer complete.\n");
    send_termination_sequence(sockfd, server_addr, server_len, next_seq_num);
}

void send_termination_sequence(int sockfd, struct sockaddr_in *server_addr, socklen_t server_len, int next_seq_num) {
    printf("Sending FIN to server...\n");
    struct sham_header fin_header;
    memset(&fin_header, 0, sizeof(fin_header));
    
    fin_header.flags = FIN;
    fin_header.seq_num = htonl(next_seq_num); 
    fin_header.ack_num = htonl(0);
    fin_header.window_size = htons(1024);
    
    if (!should_drop_packet()) {
        sendto(sockfd, &fin_header, sizeof(fin_header), 0, (const struct sockaddr *)server_addr, server_len);
        log_message("SND FIN SEQ=%u\n", next_seq_num);
    } else {
        printf("DROPPED FIN (simulated loss)\n");
        log_message("DROP FIN\n");
    }

    struct sham_header header;
    socklen_t temp_len = server_len;
    
    int retries = 0;
    while (retries < 5) {
        struct timeval tv = {1, 0};
        fd_set read_fds;
        FD_ZERO(&read_fds);
        FD_SET(sockfd, &read_fds);
        
        int select_result = select(sockfd + 1, &read_fds, NULL, NULL, &tv);
        if (select_result > 0) {
            recvfrom(sockfd, &header, sizeof(header), 0, (struct sockaddr *)server_addr, &temp_len);
            if (header.flags & FIN && header.flags & ACK) {
                printf("Received FIN-ACK from server. Sending final ACK.\n");
                log_message("RCV FIN-ACK SEQ=%u ACK=%u\n", ntohl(header.seq_num), ntohl(header.ack_num));
                goto send_final_ack;
            } else if (header.flags & FIN) {
                printf("Received FIN from server. Sending final ACK.\n");
                log_message("RCV FIN SEQ=%u\n", ntohl(header.seq_num));
                goto send_final_ack;
            } else if (header.flags & ACK) {
                printf("Received ACK from server. Waiting for their FIN.\n");
                log_message("RCV ACK FOR FIN\n");
            }
        } else {
            printf("Timeout waiting for server response. Re-transmitting FIN...\n");
            log_message("TIMEOUT waiting for server FIN/ACK\n");
            if (!should_drop_packet()) {
                sendto(sockfd, &fin_header, sizeof(fin_header), 0, (const struct sockaddr *)server_addr, server_len);
                log_message("RETX FIN SEQ=%u\n", next_seq_num);
            } else {
                log_message("DROP RETX FIN SEQ=%u\n", next_seq_num);
            }
            retries++;
        }
    }

    printf("Failed to close connection gracefully after multiple retries.\n");
    return;
    
send_final_ack:;
    struct sham_header final_ack_header;
    memset(&final_ack_header, 0, sizeof(final_ack_header));
    
    final_ack_header.flags = ACK;
    final_ack_header.seq_num = htonl(ntohl(header.ack_num));
    final_ack_header.ack_num = htonl(ntohl(header.seq_num) + 1);
    final_ack_header.window_size = htons(1024);
    
    if (!should_drop_packet()) {
        sendto(sockfd, &final_ack_header, sizeof(final_ack_header), 0, (const struct sockaddr *)server_addr, server_len);
        printf("Connection closed.\n");
        log_message("SND FINAL ACK\n");
    } else {
        printf("DROPPED final ACK (simulated loss)\n");
        log_message("DROP FINAL ACK\n");
    }
}

void print_usage(const char* program_name) {
    printf("Usage:\n");
    printf("  File Transfer Mode: %s <server_ip> <server_port> <input_file> <output_file_name> [loss_rate]\n", program_name);
    printf("  Chat Mode: %s <server_ip> <server_port> --chat [loss_rate]\n", program_name);
    printf("  loss_rate: Packet loss probability 0.0-1.0 (optional, default: 0.0)\n");
}

int main(int argc, char *argv[]) {
    if (argc < 3) {
        print_usage(argv[0]);
        return 1;
    }
    
    char *server_ip = argv[1];
    int server_port = atoi(argv[2]);
    if (server_port <= 0) {
        printf("Error: Invalid port number\n");
        return 1;
    }
    
    char *input_file = NULL;
    char *output_file = NULL;
    
    if (argc >= 4 && strcmp(argv[3], "--chat") == 0) {
        chat_mode = 1;
        if (argc >= 5) {
            double loss_rate = atof(argv[4]);
            if (loss_rate >= 0.0 && loss_rate <= 1.0) {
                packet_loss_rate = loss_rate;
            } else {
                printf("Warning: Invalid loss rate %s, using default 0.0\n", argv[4]);
            }
        }
    } else {
        if (argc < 5) {
            print_usage(argv[0]);
            return 1;
        }
        input_file = argv[3];
        output_file = argv[4];
        
        if (argc >= 6) {
            double loss_rate = atof(argv[5]);
            if (loss_rate >= 0.0 && loss_rate <= 1.0) {
                packet_loss_rate = loss_rate;
            } else {
                printf("Warning: Invalid loss rate %s, using default 0.0\n", argv[5]);
            }
        }
    }
    
    if (getenv("RUDP_LOG") != NULL) {
        log_file = fopen("client_log.txt", "a");
        if (!log_file) {
            perror("Failed to open log file");
            return 1;
        }
        printf("Logging enabled: writing to client_log.txt\n");
    }
    
    srand(time(NULL));
    
    int sockfd;
    struct sockaddr_in server_addr;
    socklen_t server_len = sizeof(server_addr);
    struct sham_header header;

    if ((sockfd = socket(AF_INET, SOCK_DGRAM, 0)) < 0) {
        perror("socket creation failed");
        if (log_file) fclose(log_file);
        exit(EXIT_FAILURE);
    }

    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(server_port);
    if (inet_pton(AF_INET, server_ip, &server_addr.sin_addr) <= 0) {
        printf("Error: Invalid server IP address\n");
        if (log_file) fclose(log_file);
        return 1;
    }

    printf("Connecting to server %s:%d\n", server_ip, server_port);
    printf("Mode: %s\n", chat_mode ? "Chat" : "File Transfer");
    if (!chat_mode) {
        printf("Input file: %s\n", input_file);
        printf("Output file: %s\n", output_file);
    }
    if (packet_loss_rate > 0.0) {
        printf("Packet loss rate: %.2f%%\n", packet_loss_rate * 100);
    }

    // Connection establishment
    memset(&header, 0, sizeof(header));
    header.flags = SYN;
    header.seq_num = htonl(50);
    header.ack_num = htonl(0);
    header.window_size = htons(1024);
    
    if (!should_drop_packet()) {
        sendto(sockfd, &header, sizeof(header), 0, (const struct sockaddr *)&server_addr, server_len);
        printf("Sent SYN with seq_num: %u\n", ntohl(header.seq_num));
        log_message("SND SYN SEQ=%u\n", ntohl(header.seq_num));
    } else {
        printf("DROPPED SYN (simulated loss)\n");
        log_message("DROP SYN\n");
    }

    struct timeval handshake_timeout = {3, 0}; // 3-second timeout for handshake
    fd_set read_fds;
    FD_ZERO(&read_fds);
    FD_SET(sockfd, &read_fds);
    
    if (select(sockfd + 1, &read_fds, NULL, NULL, &handshake_timeout) <= 0) {
        printf("Handshake failed: Timeout waiting for SYN-ACK.\n");
        if (log_file) fclose(log_file);
        close(sockfd);
        return 1;
    }
    
    recvfrom(sockfd, &header, sizeof(header), 0, (struct sockaddr *)&server_addr, &server_len);
    if ((header.flags & SYN) && (header.flags & ACK)) {
        uint16_t initial_window = ntohs(header.window_size);
        printf("Received SYN-ACK with seq_num: %u, ack_num: %u, window: %d\n", ntohl(header.seq_num), ntohl(header.ack_num), initial_window);
        log_message("RCV SYN-ACK SEQ=%u ACK=%u\n", ntohl(header.seq_num), ntohl(header.ack_num));

        struct sham_header final_ack_header;
        memset(&final_ack_header, 0, sizeof(final_ack_header));
        final_ack_header.flags = ACK;
        final_ack_header.seq_num = htonl(ntohl(header.ack_num));
        final_ack_header.ack_num = htonl(ntohl(header.seq_num) + 1);
        final_ack_header.window_size = htons(1024);

        if (!should_drop_packet()) {
            sendto(sockfd, &final_ack_header, sizeof(final_ack_header), 0, (const struct sockaddr *)&server_addr, server_len);
            printf("Sent final ACK. Handshake complete.\n");
            log_message("SND ACK FOR SYN\n");
        } else {
            printf("DROPPED final handshake ACK (simulated loss)\n");
            log_message("DROP ACK FOR SYN\n");
        }
    } else {
        printf("Handshake failed.\n");
        if (log_file) fclose(log_file);
        close(sockfd);
        return 1;
    }

    printf("Handshake complete. Starting data transfer.\n");
    if (chat_mode) {
        send_data_chat(sockfd, &server_addr, server_len);
    } else {
        send_data_file(sockfd, &server_addr, server_len, input_file);
    }
    
    close(sockfd);
    if (log_file) fclose(log_file);
    return 0;
}