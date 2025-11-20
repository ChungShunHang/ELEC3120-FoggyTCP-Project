/* Copyright (C) 2024 Hong Kong University of Science and Technology

This repository is used for the Computer Networks (ELEC 3120)
course taught at Hong Kong University of Science and Technology.

No part of the project may be copied and/or distributed without
the express permission of the course staff. Everyone is prohibited
from releasing their forks in any public places. */

/*
 * This file implements the high-level API for foggy-TCP sockets.
 */

#include "foggy_tcp.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "foggy_backend.h"
#include "foggy_packet.h"
#include "foggy_function.h"

#ifndef MIN
#define MIN(X, Y) (((X) < (Y)) ? (X) : (Y))
#endif
#ifndef MAX
#define MAX(X, Y) (((X) > (Y)) ? (X) : (Y))
#endif

void *foggy_socket(const foggy_socket_type_t socket_type,
                   const char *server_port, const char *server_ip)
{
    foggy_socket_t *sock = new foggy_socket_t;
    int sockfd, optval;
    socklen_t len;
    struct sockaddr_in conn, my_addr;
    len = sizeof(my_addr);

    sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0)
    {
        perror("ERROR opening socket");
        return NULL;
    }
    sock->socket = sockfd;

    // Buffers and locks
    sock->received_buf = NULL;
    sock->received_len = 0;
    pthread_mutex_init(&(sock->recv_lock), NULL);

    sock->sending_buf = NULL;
    sock->sending_len = 0;
    pthread_mutex_init(&(sock->send_lock), NULL);

    sock->type = socket_type;
    sock->dying = 0;
    pthread_mutex_init(&(sock->death_lock), NULL);

    // FIXME: Sequence numbers should be randomly initialized. The next expected
    // sequence number should be initialized according to the SYN packet from the
    // other side of the connection.

    sock->peer_closed = 0;

    if (pthread_cond_init(&sock->wait_cond, NULL) != 0)
    {
        perror("ERROR condition variable not set\n");
        return NULL;
    }

    sock->window.last_byte_sent = 0;
    sock->window.last_ack_received = 0;
    sock->window.dup_ack_count = 0;
    sock->window.next_seq_expected = 0;
    sock->window.ssthresh = WINDOW_INITIAL_SSTHRESH;
    sock->window.advertised_window = WINDOW_INITIAL_ADVERTISED;
    // Use a slightly larger initial congestion window to improve startup
    sock->window.congestion_window = MAX(WINDOW_INITIAL_WINDOW_SIZE, 3 * MSS);
    sock->window.reno_state = RENO_SLOW_START;
    pthread_mutex_init(&(sock->window.ack_lock), NULL);

    // Initialize RTT/RTO estimation
    sock->window.srtt_ms = 0;
    sock->window.rttvar_ms = 0;
    sock->window.rto_ms = WINDOW_INITIAL_RTT;  // initial RTO

    for (int i = 0; i < RECEIVE_WINDOW_SLOT_SIZE; ++i)
    {
        sock->receive_window[i].is_used = 0;
        sock->receive_window[i].msg = NULL;
    }

    // Protect send_window
    pthread_mutex_init(&(sock->swnd_lock), NULL);

    uint16_t portno = (uint16_t)atoi(server_port);
    switch (socket_type)
    {
    case TCP_INITIATOR:
        if (server_ip == NULL)
        {
            perror("ERROR server_ip NULL");
            return NULL;
        }
        memset(&conn, 0, sizeof(conn));

        conn.sin_family = AF_INET;
        conn.sin_addr.s_addr = inet_addr(server_ip);
        conn.sin_port = htons(portno);
        sock->conn = conn;

        memset(&my_addr, 0, sizeof(my_addr));
        my_addr.sin_family = AF_INET;
        my_addr.sin_addr.s_addr = htonl(INADDR_ANY);
        my_addr.sin_port = 0;
        if (bind(sockfd, (struct sockaddr *)&my_addr, sizeof(my_addr)) < 0)
        {
            perror("ERROR on binding");
            return NULL;
        }

        break;

    case TCP_LISTENER:
        memset(&conn, 0, sizeof(conn));
        conn.sin_family = AF_INET;
        conn.sin_addr.s_addr = htonl(INADDR_ANY);
        conn.sin_port = htons(portno);

        optval = 1;
        setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, (const void *)&optval,
                   sizeof(int));
        if (bind(sockfd, (struct sockaddr *)&conn, sizeof(conn)) < 0)
        {
            perror("ERROR on binding");
            return NULL;
        }
        sock->conn = conn;
        break;

    default:
        perror("Unknown Flag");
        return NULL;
    }
    getsockname(sockfd, (struct sockaddr *)&my_addr, &len);
    sock->my_port = ntohs(my_addr.sin_port);

    pthread_create(&(sock->thread_id), NULL, begin_backend, (void *)sock);
    return (void *)sock;
}

int foggy_close(void *in_sock)
{
    struct foggy_socket_t *sock = (struct foggy_socket_t *)in_sock;

    // Flush any pending app data into send_window
    uint8_t *pending = NULL;
    int pending_len = 0;
    while (pthread_mutex_lock(&(sock->send_lock)) != 0)
    {
    }
    if (sock->sending_len > 0 && sock->sending_buf != NULL)
    {
        pending_len = sock->sending_len;
        pending = (uint8_t *)malloc(pending_len);
        memcpy(pending, sock->sending_buf, pending_len);
        sock->sending_len = 0;
        free(sock->sending_buf);
        sock->sending_buf = NULL;
    }
    pthread_mutex_unlock(&(sock->send_lock));

    if (pending_len > 0)
    {
        send_pkts(sock, pending, pending_len);
        free(pending);
    }

    // Enqueue FIN on initiator
    if (sock->type == TCP_INITIATOR)
    {
        send_window_slot_t fin_slot;
        fin_slot.is_sent = 0;
        fin_slot.is_rtt_sample = 0;
        fin_slot.timeout_interval = 0;
        fin_slot.last_sent_ms = 0;
        fin_slot.msg = create_packet(
            sock->my_port, ntohs(sock->conn.sin_port),
            sock->window.last_byte_sent, sock->window.next_seq_expected,
            sizeof(foggy_tcp_header_t), sizeof(foggy_tcp_header_t), FIN_FLAG_MASK,
            MAX(MAX_NETWORK_BUFFER - (uint32_t)sock->received_len, MSS), 0,
            NULL, NULL, 0);
        while (pthread_mutex_lock(&(sock->swnd_lock)) != 0)
        {
        }
        sock->send_window.push_back(fin_slot);
        pthread_mutex_unlock(&(sock->swnd_lock));
        sock->window.last_byte_sent += 1;
    }

    while (pthread_mutex_lock(&(sock->death_lock)) != 0)
    {
    }
    sock->dying = 1;
    pthread_mutex_unlock(&(sock->death_lock));

    pthread_join(sock->thread_id, NULL);

    if (sock != NULL)
    {
        if (sock->received_buf != NULL)
        {
            free(sock->received_buf);
        }
        if (sock->sending_buf != NULL)
        {
            free(sock->sending_buf);
        }
    }
    else
    {
        perror("ERROR null socket\n");
        return EXIT_ERROR;
    }
    return close(sock->socket);
}

int foggy_read(void *in_sock, void *buf, int length)
{
    struct foggy_socket_t *sock = (struct foggy_socket_t *)in_sock;
    uint8_t *new_buf;
    int read_len = 0;

    if (length < 0)
    {
        perror("ERROR negative length");
        return EXIT_ERROR;
    }

    while (pthread_mutex_lock(&(sock->recv_lock)) != 0)
    {
    }

    // Wait until data arrives or peer has closed (FIN)
    while (sock->received_len == 0 && !sock->peer_closed)
    {
        pthread_cond_wait(&(sock->wait_cond), &(sock->recv_lock));
    }

    // If no data and peer closed, signal EOF
    if (sock->received_len == 0 && sock->peer_closed)
    {
        pthread_mutex_unlock(&(sock->recv_lock));
        return 0;
    }

    // Track window before read
    uint32_t old_adv = (uint32_t)MAX(0, (int)MAX_NETWORK_BUFFER - sock->received_len);

    if (sock->received_len > 0)
    {
        if (sock->received_len > length)
            read_len = length;
        else
            read_len = sock->received_len;

        memcpy(buf, sock->received_buf, read_len);
        if (read_len < sock->received_len)
        {
            new_buf = (uint8_t *)malloc(sock->received_len - read_len);
            memcpy(new_buf, sock->received_buf + read_len,
                   sock->received_len - read_len);
            free(sock->received_buf);
            sock->received_len -= read_len;
            sock->received_buf = new_buf;
        }
        else
        {
            free(sock->received_buf);
            sock->received_buf = NULL;
            sock->received_len = 0;
        }
    }

    // Compute new window after read
    uint32_t new_adv = (uint32_t)MAX(0, (int)MAX_NETWORK_BUFFER - sock->received_len);
    // Snapshot ack/seq while holding recv_lock
    uint32_t ack_num = sock->window.next_seq_expected;
    uint32_t seq_num = sock->window.last_byte_sent;

    pthread_mutex_unlock(&(sock->recv_lock));

    // If window opened, send a pure ACK advertising the new window
    if (new_adv > old_adv && read_len > 0)
    {
        uint8_t *ack_pkt = create_packet(
            sock->my_port, ntohs(sock->conn.sin_port),
            seq_num, ack_num,
            sizeof(foggy_tcp_header_t), sizeof(foggy_tcp_header_t),
            ACK_FLAG_MASK, (uint16_t)new_adv, 0, NULL, NULL, 0);
        if (ack_pkt)
        {
            foggy_tcp_header_t *ack_hdr = (foggy_tcp_header_t *)ack_pkt;
            sendto(sock->socket, ack_pkt, get_plen(ack_hdr), 0,
                   (struct sockaddr *)&(sock->conn), sizeof(sock->conn));
            free(ack_pkt);
        }
    }

    return read_len;
}

int foggy_write(void *in_sock, const void *buf, int length)
{
    struct foggy_socket_t *sock = (struct foggy_socket_t *)in_sock;
    while (pthread_mutex_lock(&(sock->send_lock)) != 0)
    {
    }
    if (sock->sending_buf == NULL)
        sock->sending_buf = (uint8_t *)malloc(length);
    else
        sock->sending_buf = (uint8_t *)realloc(sock->sending_buf, length + sock->sending_len);
    memcpy(sock->sending_buf + sock->sending_len, buf, length);
    sock->sending_len += length;

    pthread_mutex_unlock(&(sock->send_lock));
    return EXIT_SUCCESS;
}

//