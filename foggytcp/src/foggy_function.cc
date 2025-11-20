/* Copyright (C) 2024 Hong Kong University of Science and Technology

This repository is used for the Computer Networks (ELEC 3120)
course taught at Hong Kong University of Science and Technology.

No part of the project may be copied and/or distributed without
the express permission of the course staff. Everyone is prohibited
from releasing their forks in any public places. */

#include <deque>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <time.h>

#include "foggy_function.h"
#include "foggy_backend.h"

#define MIN(X, Y) (((X) < (Y)) ? (X) : (Y))
#define MAX(X, Y) (((X) > (Y)) ? (X) : (Y))

#define DEBUG_PRINT 1
#define debug_printf(fmt, ...)                   \
    do                                           \
    {                                            \
        if (DEBUG_PRINT)                         \
            fprintf(stdout, fmt, ##__VA_ARGS__); \
    } while (0)

static inline uint64_t now_ms()
{
    struct timespec ts;
    timespec_get(&ts, TIME_UTC);
    return (uint64_t)ts.tv_sec * 1000ull + (uint64_t)(ts.tv_nsec / 1000000ull);
}

// Fast retransmit the first outstanding (sent but unACKed) segment
static void fast_retransmit(foggy_socket_t *sock)
{
    while (pthread_mutex_lock(&(sock->swnd_lock)) != 0)
    {
    }
    for (auto &slot : sock->send_window)
    {
        if (!slot.is_sent)
            continue;
        foggy_tcp_header_t *hdr = (foggy_tcp_header_t *)slot.msg;
        uint32_t seq = get_seq(hdr);
        if (has_been_acked(sock, seq))
            continue;

        uint16_t plen = get_plen(hdr);
        sendto(sock->socket, slot.msg, plen, 0,
               (struct sockaddr *)&(sock->conn), sizeof(sock->conn));
        slot.last_sent_ms = now_ms();
        /* retransmission: don't use this as an RTT sample */
        slot.is_rtt_sample = 0;
        break;
    }
    pthread_mutex_unlock(&(sock->swnd_lock));
}

/**
 * Updates the socket information to represent the newly received packet.
 *
 * In the current stop-and-wait implementation, this function also sends an
 * acknowledgement for the packet.
 *
 * @param sock The socket used for handling packets received.
 * @param pkt The packet data received by the socket.
 */
void on_recv_pkt(foggy_socket_t *sock, uint8_t *pkt)
{
    debug_printf("Received packet\n");
    foggy_tcp_header_t *hdr = (foggy_tcp_header_t *)pkt;
    uint8_t flags = get_flags(hdr);

    // Handle FIN first
    if ((flags & FIN_FLAG_MASK) != 0)
    {
        sock->window.next_seq_expected += 1;

        pthread_cond_signal(&(sock->wait_cond));
        sock->peer_closed = 1;

        // Make FIN/ACK visible on stdout
        uint32_t fin_ack = sock->window.next_seq_expected;
        printf("Received FIN, sending final ACK %u\n", fin_ack);

        uint16_t adv = (uint16_t)MAX(0, (int)MAX_NETWORK_BUFFER - sock->received_len);
        uint8_t *ack_pkt = create_packet(
            sock->my_port, ntohs(sock->conn.sin_port),
            sock->window.last_byte_sent, fin_ack,
            sizeof(foggy_tcp_header_t), sizeof(foggy_tcp_header_t), ACK_FLAG_MASK,
            adv, 0, NULL, NULL, 0);
        foggy_tcp_header_t *ack_hdr = (foggy_tcp_header_t *)ack_pkt;
        sendto(sock->socket, ack_pkt, get_plen(ack_hdr), 0,
               (struct sockaddr *)&(sock->conn), sizeof(sock->conn));
        free(ack_pkt);
    }

    // Handle ACK bit (may be combined with data)
    if ((flags & ACK_FLAG_MASK) != 0)
    {
        uint32_t ack = get_ack(hdr);
        printf("Receive ACK %u\n", ack);

        // if (get_payload_len(pkt) == 0) handle_congestion_window(sock, pkt);
        sock->window.advertised_window = get_advertised_window(hdr);

        pthread_mutex_lock(&(sock->window.ack_lock));
        uint32_t prev_ack = sock->window.last_ack_received;

        if (after(ack, sock->window.last_ack_received))
        {
            uint32_t newly_acked = ack - sock->window.last_ack_received;

            /* RTT measurement: update srtt/rttvar/rto when an RTT-sampled packet is ACKed */
            while (pthread_mutex_lock(&(sock->swnd_lock)) != 0)
            {
            }
            for (auto &slot : sock->send_window)
            {
                if (!slot.is_sent)
                    continue;
                foggy_tcp_header_t *s_hdr = (foggy_tcp_header_t *)slot.msg;
                uint32_t seq = get_seq(s_hdr);
                if (slot.is_rtt_sample && after(ack, seq))
                {
                    uint64_t now = now_ms();
                    uint32_t measured_rtt = (uint32_t)(now - slot.last_sent_ms);
                    if (measured_rtt == 0)
                        measured_rtt = 1;
                    if (sock->window.srtt_ms == 0)
                    {
                        sock->window.srtt_ms = measured_rtt;
                        sock->window.rttvar_ms = measured_rtt / 2;
                    }
                    else
                    {
                        int32_t delta = (int32_t)measured_rtt - (int32_t)sock->window.srtt_ms;
                        sock->window.srtt_ms += delta / 8; /* alpha=1/8 */
                        if (delta < 0)
                            delta = -delta;
                        sock->window.rttvar_ms += (delta - (int32_t)sock->window.rttvar_ms) / 4; /* beta=1/4 */
                    }
                    uint32_t new_rto = sock->window.srtt_ms + 4 * sock->window.rttvar_ms;
                    if (new_rto < 100)
                        new_rto = 100;
                    if (new_rto > 60000)
                        new_rto = 60000;
                    sock->window.rto_ms = new_rto;
                    slot.is_rtt_sample = 0;
                    break;
                }
            }
            pthread_mutex_unlock(&(sock->swnd_lock));

            if (sock->window.reno_state == RENO_FAST_RECOVERY)
            {
                sock->window.congestion_window = sock->window.ssthresh;
                sock->window.reno_state = RENO_CONGESTION_AVOIDANCE;
            }
            else if (sock->window.reno_state == RENO_SLOW_START)
            {
                // Classic slow start: cwnd += MSS per ACK that advances ACK
                sock->window.congestion_window += MSS;
                if (sock->window.congestion_window >= sock->window.ssthresh)
                {
                    sock->window.reno_state = RENO_CONGESTION_AVOIDANCE;
                }
            }
            else if (sock->window.reno_state == RENO_CONGESTION_AVOIDANCE)
            {
                // AIMD per byte (approx): cwnd += MSS * bytes_acked / cwnd
                if (sock->window.congestion_window > 0)
                {
                    uint32_t inc = (MSS * newly_acked) /
                                   MAX(sock->window.congestion_window, (uint32_t)1);
                    if (inc == 0 && newly_acked > 0)
                        inc = 1;
                    sock->window.congestion_window += inc;
                }
            }

            sock->window.last_ack_received = ack;
            sock->window.dup_ack_count = 0;

            if (sock->type == TCP_INITIATOR &&
                !before(sock->window.last_ack_received, sock->window.last_byte_sent))
            {
                printf("All bytes ACKed (including FIN). Transmission done.\n");
            }
        }
        else if (ack == sock->window.last_ack_received)
        {
            // Duplicate ACK
            sock->window.dup_ack_count++;

            if (sock->window.reno_state != RENO_FAST_RECOVERY &&
                sock->window.dup_ack_count == 3)
            {
                // Enter Fast Recovery
                sock->window.ssthresh = MAX(sock->window.congestion_window / 2, MSS);
                sock->window.congestion_window = sock->window.ssthresh + 3 * MSS;
                sock->window.reno_state = RENO_FAST_RECOVERY;

                pthread_mutex_unlock(&(sock->window.ack_lock));
                fast_retransmit(sock);
                pthread_mutex_lock(&(sock->window.ack_lock));
            }
            else if (sock->window.reno_state == RENO_FAST_RECOVERY &&
                     sock->window.dup_ack_count > 3)
            {
                // Inflate CWND by 1 MSS per extra dup-ACK
                sock->window.congestion_window += MSS;
            }
        }
        else
        {
            // Old ACK: ignore
        }
        pthread_mutex_unlock(&(sock->window.ack_lock));

        receive_send_window(sock);
        transmit_send_window(sock);
    }

    // If payload present, handle data packet and send cumulative ACK
    if (get_payload_len(pkt) > 0)
    {
        debug_printf("Received data packet %d %d\n", get_seq(hdr),
                     get_seq(hdr) + get_payload_len(pkt));

        sock->window.advertised_window = get_advertised_window(hdr);
        // Add the packet to receive window and process receive window
        add_receive_window(sock, pkt);
        process_receive_window(sock);

        // ACK cumulative next_seq_expected with current RWND (no MSS floor)
        debug_printf("Sending ACK packet %d\n", sock->window.next_seq_expected);
        uint16_t adv = (uint16_t)MAX(0, (int)MAX_NETWORK_BUFFER - sock->received_len);
        uint8_t *ack_pkt = create_packet(
            sock->my_port, ntohs(sock->conn.sin_port),
            sock->window.last_byte_sent, sock->window.next_seq_expected,
            sizeof(foggy_tcp_header_t), sizeof(foggy_tcp_header_t), ACK_FLAG_MASK,
            adv, 0, NULL, NULL, 0);
        foggy_tcp_header_t *ack_hdr = (foggy_tcp_header_t *)ack_pkt;
        sendto(sock->socket, ack_pkt, get_plen(ack_hdr), 0,
               (struct sockaddr *)&(sock->conn), sizeof(sock->conn));
        free(ack_pkt);
    }
}

/**
 * Breaks up the data into packets and sends a single packet at a time.
 *
 * You should most certainly update this function in your implementation.
 *
 * @param sock The socket to use for sending data.
 * @param data The data to be sent.
 * @param buf_len The length of the data being sent.
 */
void send_pkts(foggy_socket_t *sock, uint8_t *data, int buf_len)
{
    uint8_t *data_offset = data;
    transmit_send_window(sock);

    if (buf_len > 0)
    {
        while (buf_len != 0)
        {
            uint16_t payload_len = MIN(buf_len, (int)MSS);

            send_window_slot_t slot;
            slot.is_sent = 0;
            slot.is_rtt_sample = 0;
            slot.timeout_interval = 0;
            slot.last_sent_ms = 0;
            slot.msg = create_packet(
                sock->my_port, ntohs(sock->conn.sin_port),
                sock->window.last_byte_sent, sock->window.next_seq_expected,
                sizeof(foggy_tcp_header_t), sizeof(foggy_tcp_header_t) + payload_len,
                ACK_FLAG_MASK,
                MAX(MAX_NETWORK_BUFFER - (uint32_t)sock->received_len, MSS), 0, NULL,
                data_offset, payload_len);

            while (pthread_mutex_lock(&(sock->swnd_lock)) != 0)
            {
            }
            sock->send_window.push_back(slot);
            pthread_mutex_unlock(&(sock->swnd_lock));

            buf_len -= payload_len;
            data_offset += payload_len;
            sock->window.last_byte_sent += payload_len;
        }
    }
    transmit_send_window(sock);
    receive_send_window(sock);
}

void add_receive_window(foggy_socket_t *sock, uint8_t *pkt)
{
    foggy_tcp_header_t *hdr = (foggy_tcp_header_t *)pkt;

    // Stop-and-wait implementation
    // Insert the packet into the first place of receive window
    receive_window_slot_t *cur_slot = &(sock->receive_window[0]);
    if (cur_slot->is_used == 0)
    {
        cur_slot->is_used = 1;
        cur_slot->msg = (uint8_t *)malloc(get_plen(hdr));
        memcpy(cur_slot->msg, pkt, get_plen(hdr));
    }
    else
    {
        // If replacing with the expected seq (duplicate/out-of-order cleanup)
        foggy_tcp_header_t *old = (foggy_tcp_header_t *)cur_slot->msg;
        if (get_seq(hdr) == sock->window.next_seq_expected &&
            get_seq(old) != sock->window.next_seq_expected)
        {
            debug_printf("Replacing out-of-order packet %d with expected %d\n",
                         get_seq(old), get_seq(hdr));
            free(cur_slot->msg);
            cur_slot->msg = (uint8_t *)malloc(get_plen(hdr));
            memcpy(cur_slot->msg, pkt, get_plen(hdr));
        }
    }
}

void process_receive_window(foggy_socket_t *sock)
{
    // Stop-and-wait implementation.
    // Only process the first packet in the window.
    receive_window_slot_t *cur_slot = &(sock->receive_window[0]);
    if (cur_slot->is_used != 0)
    {
        foggy_tcp_header_t *hdr = (foggy_tcp_header_t *)cur_slot->msg;
        // Discard unexpected packet
        if (get_seq(hdr) != sock->window.next_seq_expected)
            return;
        // Update next seq number expected
        uint16_t payload_len = get_payload_len(cur_slot->msg);

        if (payload_len > 0)
        {
            // Copy to received_buf
            sock->received_buf = (uint8_t *)
                realloc(sock->received_buf, sock->received_len + payload_len);
            memcpy(sock->received_buf + sock->received_len, get_payload(cur_slot->msg),
                   payload_len);
            sock->received_len += payload_len;

            // Wake reader promptly
            pthread_cond_signal(&(sock->wait_cond));
        }

        // Advance cumulative next expected seq
        sock->window.next_seq_expected += payload_len;

        // Free the slot
        cur_slot->is_used = 0;
        free(cur_slot->msg);
        cur_slot->msg = NULL;
    }
}

void transmit_send_window(foggy_socket_t *sock)
{
    while (1)
    {
        pthread_mutex_lock(&(sock->window.ack_lock));
        uint32_t window_limit = MIN(sock->window.congestion_window,
                                    sock->window.advertised_window);
        pthread_mutex_unlock(&(sock->window.ack_lock));

        while (pthread_mutex_lock(&(sock->swnd_lock)) != 0)
        {
        }

        uint32_t outstanding = 0;
        for (auto &slot : sock->send_window)
        {
            if (!slot.is_sent)
                continue;
            foggy_tcp_header_t *hdr = (foggy_tcp_header_t *)slot.msg;
            if (has_been_acked(sock, get_seq(hdr)))
                continue;
            outstanding += get_payload_len(slot.msg);
        }
        if (outstanding >= window_limit)
        {
            pthread_mutex_unlock(&(sock->swnd_lock));
            break;
        }

        /* Timeout detection: scan for earliest outstanding and retransmit if timed out */
        uint64_t now = now_ms();
        for (auto &slot : sock->send_window)
        {
            if (!slot.is_sent)
                continue;
            foggy_tcp_header_t *hdr = (foggy_tcp_header_t *)slot.msg;
            if (has_been_acked(sock, get_seq(hdr)))
                continue;
            if (slot.last_sent_ms == 0)
                continue;
            uint32_t elapsed = (uint32_t)(now - slot.last_sent_ms);
            if (elapsed >= sock->window.rto_ms)
            {
                /* Timeout: retransmit this packet and apply backoff */
                printf("Timeout: retransmit seq %u (elapsed %u ms, rto %u ms)\n", get_seq(hdr), elapsed, sock->window.rto_ms);
                sendto(sock->socket, slot.msg, get_plen(hdr), 0,
                       (struct sockaddr *)&(sock->conn), sizeof(sock->conn));
                slot.last_sent_ms = now;
                slot.is_rtt_sample = 0; /* retransmission shouldn't be used for RTT */
                /* Exponential backoff on RTO */
                sock->window.rto_ms = MIN(sock->window.rto_ms * 2, (uint32_t)60000);
                /* Multiplicative decrease of congestion window */
                pthread_mutex_lock(&(sock->window.ack_lock));
                sock->window.ssthresh = MAX(sock->window.congestion_window / 2, (uint32_t)MSS);
                sock->window.congestion_window = MSS;
                sock->window.reno_state = RENO_SLOW_START;
                pthread_mutex_unlock(&(sock->window.ack_lock));
                break; /* handle one timeout per call to avoid aggressive loops */
            }
        }

        bool sent_one = false;
        for (auto &slot : sock->send_window)
        {
            if (slot.is_sent)
                continue;

            foggy_tcp_header_t *hdr = (foggy_tcp_header_t *)slot.msg;
            uint16_t payload_len = get_payload_len(slot.msg);

            // Always allow control packets (e.g., FIN)
            if (payload_len == 0)
            {
                slot.is_sent = 1;
                slot.timeout_interval = sock->window.rto_ms;
                slot.last_sent_ms = now_ms();
                sendto(sock->socket, slot.msg, get_plen(hdr), 0,
                       (struct sockaddr *)&(sock->conn), sizeof(sock->conn));
                sent_one = true;
                continue;
            }

            if (outstanding + payload_len > window_limit)
                break;

            debug_printf("Sending packet %d %d\n", get_seq(hdr),
                         get_seq(hdr) + payload_len);
            slot.is_sent = 1;
            /* mark an RTT sample only if no other sample is outstanding */
            bool sample_exists = false;
            for (auto &s2 : sock->send_window)
            {
                if (&s2 == &slot)
                    break;
                if (s2.is_sent && s2.is_rtt_sample)
                {
                    sample_exists = true;
                    break;
                }
            }
            if (!sample_exists)
                slot.is_rtt_sample = 1;
            slot.timeout_interval = sock->window.rto_ms;
            slot.last_sent_ms = now_ms();
            sendto(sock->socket, slot.msg, get_plen(hdr), 0,
                   (struct sockaddr *)&(sock->conn), sizeof(sock->conn));
            outstanding += payload_len;
            sent_one = true;
        }

        pthread_mutex_unlock(&(sock->swnd_lock));
        if (!sent_one)
            break;
    }
}

void receive_send_window(foggy_socket_t *sock)
{
    // Pop out the packets that have been ACKed
    while (pthread_mutex_lock(&(sock->swnd_lock)) != 0)
    {
    }
    while (1)
    {
        if (sock->send_window.empty())
            break;

        send_window_slot_t slot = sock->send_window.front();
        foggy_tcp_header_t *hdr = (foggy_tcp_header_t *)slot.msg;

        if (slot.is_sent == 0)
            break;

        if (has_been_acked(sock, get_seq(hdr)) == 0)
            break;

        sock->send_window.pop_front();
        free(slot.msg);
    }
    pthread_mutex_unlock(&(sock->swnd_lock));

    transmit_send_window(sock);
}