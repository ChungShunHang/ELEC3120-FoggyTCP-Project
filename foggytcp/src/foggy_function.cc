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
#include <math.h>  // New: for cbrt in CUBIC

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
        break;
    }
    pthread_mutex_unlock(&(sock->swnd_lock));
}

// New: CUBIC loss handler (multiplicative decrease)
void cubic_on_loss(foggy_socket_t *sock)
{
    // Assumes ack_lock is held by caller when called on loss path
    // Save current cwnd as Wmax (classic cubic uses fast convergence tweaks; keep simple)
    sock->window.cubic_w_max = sock->window.congestion_window;

    // Multiplicative decrease
    uint32_t reduced = (uint32_t)(sock->window.congestion_window * sock->window.cubic_beta);
    if (reduced < MSS) reduced = MSS;

    sock->window.ssthresh = reduced;
    sock->window.congestion_window = reduced;

    // Reset epoch so we recompute the cubic curve after loss
    sock->window.cubic_epoch_start_ms = 0;
    sock->window.cubic_origin_point = sock->window.congestion_window;
}

// New: CUBIC growth on ACK during Congestion Avoidance
void cubic_update_on_ack(foggy_socket_t *sock, uint32_t newly_acked)
{
    // Assumes ack_lock is held by caller
    uint64_t now = now_ms();

    if (sock->window.cubic_epoch_start_ms == 0) {
        sock->window.cubic_epoch_start_ms = now;
        // Use current cwnd as new origin
        sock->window.cubic_origin_point = sock->window.congestion_window;
        // Ensure Wmax is at least current cwnd
        if (sock->window.cubic_w_max < sock->window.congestion_window)
            sock->window.cubic_w_max = sock->window.congestion_window;
    }

    double t = (double)(now - sock->window.cubic_epoch_start_ms) / 1000.0; // seconds
    double C = sock->window.cubic_C;
    double Wmax = (double)sock->window.cubic_w_max;
    double W0   = (double)sock->window.cubic_origin_point;

    // K = cbrt((Wmax - W0)/C)  [units: seconds]
    double K = 0.0;
    if (Wmax > W0 && C > 0.0) {
        double ratio = (Wmax - W0) / C;
        if (ratio > 0.0) K = cbrt(ratio);
    }

    // W_cubic(t) = C*(t - K)^3 + Wmax
    double diff = (t - K);
    double Wt = C * diff * diff * diff + Wmax;
    if (Wt < (double)MSS) Wt = (double)MSS;

    double cwndD = (double)sock->window.congestion_window;

    if (Wt > cwndD) {
        // Move cwnd toward cubic target, cap per-ACK growth by MSS for stability
        double delta = Wt - cwndD;
        uint32_t inc = (uint32_t)MIN((double)MSS, delta);
        if (inc == 0 && newly_acked > 0) inc = 1;
        sock->window.congestion_window += inc;
    } else {
        // TCP-friendly region: fall back to a Reno-like byte counting increase
        if (sock->window.congestion_window > 0) {
            uint32_t inc = (uint32_t)((double)MSS * (double)newly_acked /
                                      (double)MAX(sock->window.congestion_window, (uint32_t)1));
            if (inc == 0 && newly_acked > 0) inc = 1;
            sock->window.congestion_window += inc;
        }
    }
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

            if (sock->window.reno_state == RENO_FAST_RECOVERY)
            {
                // Recovery complete -> CA
                sock->window.congestion_window = sock->window.ssthresh;
                sock->window.reno_state = RENO_CONGESTION_AVOIDANCE;
                // Reset CUBIC epoch on recovery exit so growth follows new curve
                if (sock->window.cca_alg == CCA_CUBIC) {
                    sock->window.cubic_epoch_start_ms = 0;
                    sock->window.cubic_origin_point = sock->window.congestion_window;
                }
            }
            else if (sock->window.reno_state == RENO_SLOW_START)
            {
                // Slow start: byte counting
                sock->window.congestion_window += newly_acked;
                if (sock->window.congestion_window >= sock->window.ssthresh)
                    sock->window.reno_state = RENO_CONGESTION_AVOIDANCE;
            }
            else if (sock->window.reno_state == RENO_CONGESTION_AVOIDANCE)
            {
                if (sock->window.cca_alg == CCA_CUBIC) {
                    cubic_update_on_ack(sock, newly_acked);
                } else {
                    // Reno AIMD
                    if (sock->window.congestion_window > 0)
                    {
                        uint32_t inc = (MSS * newly_acked) /
                                       MAX(sock->window.congestion_window, (uint32_t)1);
                        if (inc == 0 && newly_acked > 0) inc = 1;
                        sock->window.congestion_window += inc;
                    }
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
                if (sock->window.cca_alg == CCA_CUBIC) {
                    // Apply CUBIC multiplicative decrease only; do NOT reinflate to ssthresh+3*MSS
                    cubic_on_loss(sock);
                    sock->window.reno_state = RENO_FAST_RECOVERY;
                    // Keep cwnd at reduced value (sock->window.congestion_window already set)
                } else {
                    sock->window.ssthresh = MAX(sock->window.congestion_window / 2, MSS);
                    sock->window.congestion_window = sock->window.ssthresh + 3 * MSS;
                    sock->window.reno_state = RENO_FAST_RECOVERY;
                }
                pthread_mutex_unlock(&(sock->window.ack_lock));
                fast_retransmit(sock);
                pthread_mutex_lock(&(sock->window.ack_lock));
            }
            else if (sock->window.reno_state == RENO_FAST_RECOVERY &&
                     sock->window.dup_ack_count > 3)
            {
                // Limited transmit / inflation while in FR
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
                slot.timeout_interval = WINDOW_INITIAL_RTT;
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
            slot.timeout_interval = WINDOW_INITIAL_RTT;
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