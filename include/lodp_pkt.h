/*
 * lodp_pkt.h: LODP Packet Processing
 *
 * Copyright (c) 2013, Yawning Angel <yawning at schwanenlied dot me>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * * Redistributions of source code must retain the above copyright notice,
 *   this list of conditions and the following disclaimer.
 *
 * * Redistributions in binary form must reproduce the above copyright notice,
 *   this list of conditions and the following disclaimer in the documentation
 *   and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include <stdint.h>

#include "lodp.h"
#include "lodp_crypto.h"
#include "lodp_impl.h"

#ifndef _LODP_PKT_H_
#define _LODP_PKT_H_


/* Wire protocol format */
typedef enum {
	PKT_DATA = 0,
	PKT_INIT = 1,
	PKT_INIT_ACK = 2,
	PKT_HANDSHAKE = 3,
	PKT_HANDSHAKE_ACK = 4,
	PKT_HEARTBEAT = 5,
	PKT_HEARTBEAT_ACK = 6,
	PKT_REKEY = 7,
	PKT_REKEY_ACK = 8
} lodp_pkt_type;


typedef struct __attribute__ ((__packed__)) lodp_hdr_s {
	/* The authenticated encryption tag */
	uint8_t mac[LODP_MAC_DIGEST_LEN];
	uint8_t iv[LODP_BULK_IV_LEN];

	/* The common packet Type/Length/Value header */
	uint8_t type;
	uint8_t flags;
	uint16_t length;
} lodp_hdr;

typedef struct __attribute__ ((__packed__)) lodp_pkt_raw_s {
	lodp_hdr hdr;
	uint8_t payload[];
} lodp_pkt_raw;

typedef struct __attribute__ ((__packed__)) lodp_pkt_data_s {
	lodp_hdr hdr;
	uint8_t data[];
} lodp_pkt_data;

typedef struct __attribute__ ((__packed__)) lodp_pkt_init_s {
	lodp_hdr hdr;
	uint8_t intro_mac_key[LODP_MAC_KEY_LEN];
	uint8_t intro_bulk_key[LODP_MAC_KEY_LEN];
} lodp_pkt_init;

typedef struct __attribute__ ((__packed__)) lodp_pkt_init_ack_s {
	lodp_hdr hdr;
	uint8_t cookie[];
} lodp_pkt_init_ack;

typedef struct __attribute__ ((__packed__)) lodp_pkt_handshake_s {
	lodp_hdr hdr;
	uint8_t intro_mac_key[LODP_MAC_KEY_LEN];
	uint8_t intro_bulk_key[LODP_MAC_KEY_LEN];
	uint8_t public_key[LODP_ECDH_PUBLIC_KEY_LEN];
	uint8_t cookie[];
} lodp_pkt_handshake;

typedef struct __attribute__ ((__packed__)) lodp_pkt_handshake_ack_s {
	lodp_hdr hdr;
	uint8_t public_key[LODP_ECDH_PUBLIC_KEY_LEN];
	uint8_t digest[LODP_MAC_DIGEST_LEN];
} lodp_pkt_handshake_ack;

typedef struct __attribute__ ((__packed__)) lodp_pkt_heartbeat_s {
	lodp_hdr hdr;
	uint8_t data[];
} lodp_pkt_heartbeat;

typedef struct __attribute__ ((__packed__)) lodp_pkt_heartbeat_ack_s {
	lodp_hdr hdr;
	uint8_t data[];
} lodp_pkt_heartbeat_ack;


/*
 * Packet size constants:
 *  PKT_<TYPE>_LEN = size of full packet in the buffer
 *  PKT_<TYPE>_HDR_LEN = size in the LODP header
 *
 * Note:
 * DATA, INIT ACK and HANDSHAKE packets all need to fixup the length(s) to
 * reflect the variable length portion of the payload.
 */
#define PKT_TAG_LEN			(LODP_MAC_DIGEST_LEN + LODP_BULK_IV_LEN)
#define PKT_TLV_LEN			4
#define PKT_HDR_LEN			sizeof(lodp_hdr)

#define PKT_DATA_LEN			sizeof(lodp_pkt_data)
#define PKT_HDR_DATA_LEN		(sizeof(lodp_pkt_data) - PKT_TAG_LEN)
#define PKT_INIT_LEN			sizeof(lodp_pkt_init)
#define PKT_HDR_INIT_LEN		(sizeof(lodp_pkt_init) - PKT_TAG_LEN)
#define PKT_INIT_ACK_LEN		sizeof(lodp_pkt_init_ack)
#define PKT_HDR_INIT_ACK_LEN		(sizeof(lodp_pkt_init_ack) - PKT_TAG_LEN)
#define PKT_HANDSHAKE_LEN		sizeof(lodp_pkt_handshake)
#define PKT_HDR_HANDSHAKE_LEN		(sizeof(lodp_pkt_handshake) - PKT_TAG_LEN)
#define PKT_HANDSHAKE_ACK_LEN		sizeof(lodp_pkt_handshake_ack)
#define PKT_HDR_HANDSHAKE_ACK_LEN	(sizeof(lodp_pkt_handshake_ack) - PKT_TAG_LEN)
#define PKT_HEARTBEAT_LEN		sizeof(lodp_pkt_heartbeat)
#define PKT_HDR_HEARTBEAT_LEN		(sizeof(lodp_pkt_heartbeat) - PKT_TAG_LEN)
#define PKT_HEARTBEAT_ACK_LEN		sizeof(lodp_pkt_heartbeat_ack)
#define PKT_HDR_HEARTBEAT_ACK_LEN	(sizeof(lodp_pkt_heartbeat_ack) - PKT_TAG_LEN)


/* Cookie */
void lodp_rotate_cookie_key(lodp_endpoint *ep);

/* Incoming packets */
int lodp_on_incoming_pkt(lodp_endpoint *ep, lodp_session *session, lodp_buf
    *buf, const struct sockaddr *addr, socklen_t addr_len);

/* Outgoing packets */
int lodp_send_data_pkt(lodp_session *session, const uint8_t *buf, size_t len);
int lodp_send_init_pkt(lodp_session *session);
int lodp_send_handshake_pkt(lodp_session *session);
int lodp_send_heartbeat_pkt(lodp_session *session, const uint8_t *buf, size_t
    len);


#endif