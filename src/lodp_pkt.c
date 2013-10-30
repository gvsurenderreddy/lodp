/*
 * lodp_pkt.c: LODP Packet Processing
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

#include <stdlib.h>
#include <time.h>
#include <assert.h>

#include "lodp.h"
#include "lodp_crypto.h"
#include "lodp_impl.h"
#include "lodp_pkt.h"


#define COOKIE_LEN			LODP_MAC_DIGEST_LEN
#define COOKIE_ROTATE_INTERVAL		30
#define COOKIE_GRACE_WINDOW		15

typedef struct {
	uint8_t bytes[COOKIE_LEN];
} lodp_cookie;


/* Packet/session related crypto */
static int encrypt_then_mac(lodp_endpoint *ep, lodp_symmetric_key *keys,
    lodp_buf *buf);
static int mac_then_decrypt(lodp_symmetric_key *keys, lodp_buf *buf);
static int generate_cookie(lodp_cookie *cookie, int prev_key, lodp_endpoint *ep,
    const lodp_pkt_raw *pkt, const struct sockaddr *addr, socklen_t addr_len);
static int ntor_handshake(lodp_session *session, lodp_ecdh_public_key *pub_key);
static void scrub_handshake_material(lodp_session *session);


/* Packet type specific handler routines */
static int on_init_pkt(lodp_endpoint *ep, const lodp_pkt_init *init_pkt,
    const struct sockaddr *addr, socklen_t addr_len);
static int on_handshake_pkt(lodp_endpoint *ep, lodp_session *session,
    const lodp_pkt_handshake *hs_pkt, const struct sockaddr *addr,
    socklen_t addr_len);
static int on_data_pkt(lodp_session *session, const lodp_pkt_data *pkt);
static int on_init_ack_pkt(lodp_session *session, const lodp_pkt_init_ack *pkt);
static int on_handshake_ack_pkt(lodp_session *session, const
    lodp_pkt_handshake_ack *pkt);
static int on_heartbeat_pkt(lodp_session *session, const lodp_pkt_heartbeat
    *hb_pkt);
static int on_heartbeat_ack_pkt(lodp_session *session, const
    lodp_pkt_heartbeat_ack *pkt);


int
lodp_on_incoming_pkt(lodp_endpoint *ep, lodp_session *session, lodp_buf *buf,
    const struct sockaddr *addr, socklen_t addr_len)
{
	lodp_hdr *hdr;
	int used_session_keys = 0;
	int ret;

	assert(NULL != ep);
	assert(NULL != buf);
	assert(NULL != addr);
	assert(addr_len > 0);

	/*
	 * Validate the MAC and Decrypt
	 *
	 * Note:
	 * Before copying the data from the user buffer (received off the wire)
	 * to the lodp_buf, we validated the buffer length to ensure that at a
	 * minimum, the IV/MAC and 4 byte common packet Type/Flags/Length header
	 * is present.
	 */

	if (NULL != session) {
		/* Try the session keys first */
		ret = mac_then_decrypt(&session->rx_key, buf);
		if (!ret) {
			used_session_keys = 1;
			goto mac_then_decrypt_ok;
		} else if (LODP_ERR_INVALID_MAC != ret)
			return (ret);

		/*
		 * Invalid MAC, this could be a retransmited HANDSHAKE packet,
		 * so try the endpoint keys before giving up.
		 */
	}
	if (!ep->has_intro_keys)
		return (LODP_ERR_NOT_RESPONDER);

	ret = mac_then_decrypt(&ep->intro_sym_keys, buf);
	if (ret)
		return (ret);

mac_then_decrypt_ok:

	/*
	 * Do the remaining packet type agnostic sanity checking
	 *
	 * Note:
	 * All that needs to be done here is to fixup pkt->length to host byte
	 * order and ensure that pkt->length >= 4 (TLV header is *always*
	 * included in the length) and pkt->length <= buf->len -
	 * PKT_TAG_LEN (The buffer we received is actually has all of
	 * the payload).
	 *
	 * While not a strict requirement, none of the packets actually use the
	 * flag field yet either, so check that here.  Whenever flags are
	 * actually defined, this check will need to be moved into each of the
	 * individual packet handlers.
	 */

	hdr = (lodp_hdr *)buf->plaintext;
	hdr->length = ntohs(hdr->length);
	if (hdr->length < PKT_TLV_LEN)
		return (LODP_ERR_BAD_PACKET);   /* Undersized */

	if (hdr->length > buf->len - PKT_TAG_LEN)
		return (LODP_ERR_BAD_PACKET);   /* Oversized */

	if (0 != hdr->flags)
		return (LODP_ERR_BAD_PACKET);   /* Flags not defined yet */

	/*
	 * Actually handle the packet
	 *
	 * At this point, the packet is "tenatively" valid in that it had a
	 * valid MAC, was encrypted with a key that we understand, and the
	 * length is "valid" (May be incorrect for the specific packet type,
	 * but at least that much data is actually present).
	 */

	if (NULL != session) {
		/*
		 * It's possible to get HANDSHAKE packets even though a session
		 * already exists if the HANDSHAKE ACK got lost.  This is only
		 * valid if said packet was encrypted/MACed with the endpoint
		 * keys.
		 */

		if (!used_session_keys) {
			if (PKT_HANDSHAKE != hdr->type)
				return (LODP_ERR_BAD_PACKET);

			if (session->is_initiator)
				return (LODP_ERR_NOT_RESPONDER);

			return (on_handshake_pkt(ep, session, (lodp_pkt_handshake *)hdr,
			       addr, addr_len));
		}

		/* Packets for an existing session */
		switch (hdr->type)
		{
		case PKT_DATA:
			return (on_data_pkt(session, (lodp_pkt_data *)hdr));

		case PKT_INIT_ACK:
			return (on_init_ack_pkt(session, (lodp_pkt_init_ack *)hdr));

		case PKT_HANDSHAKE_ACK:
			return (on_handshake_ack_pkt(session, (lodp_pkt_handshake_ack *)hdr));

		case PKT_HEARTBEAT:
			return (on_heartbeat_pkt(session, (lodp_pkt_heartbeat *)hdr));

		case PKT_HEARTBEAT_ACK:
			return (on_heartbeat_ack_pkt(session, (lodp_pkt_heartbeat_ack *)hdr));

		/* TODO: Implement these */
		case PKT_REKEY:
		case PKT_REKEY_ACK:
		default:
			break;
		}
	} else {
		/* Responder handshake related packets */
		assert(ep->has_intro_keys);
		switch (hdr->type)
		{
		case PKT_INIT:
			return (on_init_pkt(ep, (lodp_pkt_init *)hdr, addr, addr_len));

		case PKT_HANDSHAKE:
			return (on_handshake_pkt(ep, session, (lodp_pkt_handshake *)hdr,
			       addr, addr_len));

		default:
			break;
		}
	}

	/* It's not like I decrypted that packet for you or anything... baka. */
	return (LODP_ERR_BAD_PACKET);
}


int
lodp_send_data_pkt(lodp_session *session, const uint8_t *payload, size_t len)
{
	lodp_pkt_data *pkt;
	lodp_buf *buf;
	int ret = 0;

	assert(NULL != session);
	assert(STATE_ESTABLISHED == session->state);

	if (PKT_DATA_LEN + len > LODP_MSS)
		return (LODP_ERR_MSGSIZE);

	buf = lodp_buf_alloc();
	if (NULL == buf)
		return (LODP_ERR_NOBUFS);

	buf->len = PKT_DATA_LEN + len;
	assert(buf->len < LODP_MSS);

	pkt = (lodp_pkt_data *)buf->plaintext;
	pkt->hdr.type = PKT_DATA;
	pkt->hdr.flags = 0;
	pkt->hdr.length = htons(PKT_HDR_DATA_LEN + len);
	memcpy(pkt->data, payload, len);

	ret = encrypt_then_mac(session->ep, &session->tx_key, buf);
	if (ret)
		goto out;
	ret = session->ep->callbacks.sendto_fn(session->ep, session->ep->ctxt,
		buf->ciphertext, buf->len, (struct sockaddr *)
		&session->peer_addr, session->peer_addr_len);

out:
	lodp_buf_free(buf);
	return (ret);
}


int
lodp_send_init_pkt(lodp_session *session)
{
	lodp_pkt_init *pkt;
	lodp_buf *buf;
	int ret = 0;

	assert(NULL != session);
	assert(session->is_initiator);
	assert(STATE_INIT == session->state);

	buf = lodp_buf_alloc();
	if (NULL == buf)
		return (LODP_ERR_NOBUFS);

	buf->len = PKT_INIT_LEN;
	assert(buf->len < LODP_MSS);

	pkt = (lodp_pkt_init *)buf->plaintext;
	pkt->hdr.type = PKT_INIT;
	pkt->hdr.flags = 0;
	pkt->hdr.length = htons(PKT_HDR_INIT_LEN);
	memcpy(pkt->intro_mac_key, session->rx_key.mac_key.mac_key,
	    sizeof(pkt->intro_mac_key));
	memcpy(pkt->intro_bulk_key, session->rx_key.bulk_key.bulk_key,
	    sizeof(pkt->intro_bulk_key));

	ret = encrypt_then_mac(session->ep, &session->tx_key, buf);
	if (ret)
		goto out;
	ret = session->ep->callbacks.sendto_fn(session->ep, session->ep->ctxt,
		buf->ciphertext, buf->len, (struct sockaddr *)
		&session->peer_addr, session->peer_addr_len);

out:
	lodp_buf_free(buf);
	return (ret);
}


int
lodp_send_handshake_pkt(lodp_session *session)
{
	lodp_pkt_handshake *pkt;
	lodp_buf *buf;
	int ret;

	assert(NULL != session);
	assert(session->is_initiator);
	assert(STATE_HANDSHAKE == session->state);

	buf = lodp_buf_alloc();
	if (NULL == buf)
		return (LODP_ERR_NOBUFS);

	buf->len = PKT_HANDSHAKE_LEN + session->cookie_len;
	assert(buf->len < LODP_MSS);

	pkt = (lodp_pkt_handshake *)buf->plaintext;
	pkt->hdr.type = PKT_HANDSHAKE;
	pkt->hdr.flags = 0;
	pkt->hdr.length = htons(PKT_HDR_HANDSHAKE_LEN + session->cookie_len);
	memcpy(pkt->intro_mac_key, session->rx_key.mac_key.mac_key,
	    sizeof(pkt->intro_mac_key));
	memcpy(pkt->intro_bulk_key, session->rx_key.bulk_key.bulk_key,
	    sizeof(pkt->intro_bulk_key));
	memcpy(pkt->public_key,
	    session->session_ecdh_keypair.public_key.public_key,
	    sizeof(pkt->public_key));
	memcpy(pkt->cookie, session->cookie, session->cookie_len);

	ret = encrypt_then_mac(session->ep, &session->tx_key, buf);
	if (ret)
		goto out;
	ret = session->ep->callbacks.sendto_fn(session->ep, session->ep->ctxt,
		buf->ciphertext, buf->len, (struct sockaddr *)
		&session->peer_addr, session->peer_addr_len);

out:
	lodp_buf_free(buf);
	return (0);
}


int
lodp_send_heartbeat_pkt(lodp_session *session, const uint8_t *payload, size_t len)
{
	lodp_pkt_heartbeat *pkt;
	lodp_buf *buf;
	int ret = 0;

	assert(NULL != session);
	assert(STATE_ESTABLISHED == session->state);

	if (PKT_HEARTBEAT_LEN + len > LODP_MSS)
		return (LODP_ERR_MSGSIZE);

	buf = lodp_buf_alloc();
	if (NULL == buf)
		return (LODP_ERR_NOBUFS);

	buf->len = PKT_HEARTBEAT_LEN + len;
	assert(buf->len < LODP_MSS);

	pkt = (lodp_pkt_heartbeat *)buf->plaintext;
	pkt->hdr.type = PKT_HEARTBEAT;
	pkt->hdr.flags = 0;
	pkt->hdr.length = htons(PKT_HDR_HEARTBEAT_LEN + len);
	memcpy(pkt->data, payload, len);

	ret = encrypt_then_mac(session->ep, &session->tx_key, buf);
	if (ret)
		goto out;
	ret = session->ep->callbacks.sendto_fn(session->ep, session->ep->ctxt,
		buf->ciphertext, buf->len, (struct sockaddr *)
		&session->peer_addr, session->peer_addr_len);

out:
	lodp_buf_free(buf);
	return (ret);
}


void
lodp_rotate_cookie_key(lodp_endpoint *ep)
{
	time_t now;

	assert(NULL != ep);

	now = time(NULL);
	memcpy(&ep->prev_cookie_key, &ep->cookie_key,
	    sizeof(ep->prev_cookie_key));
	lodp_rand_bytes(&ep->cookie_key, sizeof(ep->cookie_key));

	ep->cookie_rotate_time = now;
	ep->cookie_expire_time = now + COOKIE_GRACE_WINDOW;
}


static int
encrypt_then_mac(lodp_endpoint *ep, lodp_symmetric_key *keys, lodp_buf *buf)
{
	lodp_hdr *pt_hdr, *ct_hdr;
	int ret;

	assert(NULL != ep);
	assert(NULL != keys);
	assert(NULL != buf);
	assert(buf->len > 0);
	assert(buf->len <= LODP_MSS);

	pt_hdr = (lodp_hdr *)buf->plaintext;
	ct_hdr = (lodp_hdr *)buf->ciphertext;

	/*
	 * Optionally allow the user to insert randomized padding here with a
	 * with a callback.
	 */

	if (NULL != ep->callbacks.pre_encrypt_fn) {
		ret = ep->callbacks.pre_encrypt_fn(ep, ep->ctxt, buf->len,
		    LODP_MSS);
		if (ret > 0) {
			lodp_log(ep, LODP_LOG_DEBUG, "%d bytes of padding, %d",
			    ret, buf->len);
			if (ret + buf->len > LODP_MSS)
				ret = LODP_MSS - buf->len;
			lodp_rand_bytes(((void *)pt_hdr) + buf->len, ret);
			buf->len += ret;
		}
	}

	/* Encrypt */
	lodp_rand_bytes(ct_hdr->iv, sizeof(ct_hdr->iv)); /* Random IV */
	ret = lodp_encrypt(ct_hdr->iv + sizeof(ct_hdr->iv), &keys->bulk_key,
		ct_hdr->iv, pt_hdr->iv + sizeof(pt_hdr->iv), buf->len -
		PKT_TAG_LEN);
	if (ret)
		return (ret);

	/* MAC */
	ret = lodp_mac(ct_hdr->mac, ct_hdr->iv, &keys->mac_key, sizeof(ct_hdr->mac),
		buf->len - sizeof(ct_hdr->mac));
	return (ret);
}


static int
mac_then_decrypt(lodp_symmetric_key *keys, lodp_buf *buf)
{
	uint8_t digest[LODP_MAC_DIGEST_LEN];
	lodp_hdr *pt_hdr, *ct_hdr;
	int ret;

	assert(NULL != keys);
	assert(NULL != buf);
	assert(buf->len > 0);

	pt_hdr = (lodp_hdr *)buf->plaintext;
	ct_hdr = (lodp_hdr *)buf->ciphertext;

	/* MAC */
	ret = lodp_mac(digest, ct_hdr->iv, &keys->mac_key, sizeof(ct_hdr->mac),
		buf->len - sizeof(ct_hdr->mac));
	if (ret)
		return (ret);

	if (lodp_memcmp(digest, ct_hdr->mac, sizeof(digest)))
		return (LODP_ERR_INVALID_MAC);

	/* Decrypt */
	ret = lodp_decrypt(pt_hdr->iv + sizeof(pt_hdr->iv), &keys->bulk_key,
		ct_hdr->iv, ct_hdr->iv + sizeof(ct_hdr->iv), buf->len -
		PKT_TAG_LEN);
	return (ret);
}


static int
generate_cookie(lodp_cookie *cookie, int prev_key, lodp_endpoint *ep,
    const lodp_pkt_raw *pkt, const struct sockaddr *addr,
    socklen_t addr_len)
{
	uint8_t blob[16 + 2 + LODP_MAC_KEY_LEN + LODP_BULK_KEY_LEN];
	uint8_t *p;
	time_t now;
	int ret;

	assert(NULL != cookie);
	assert(NULL != ep);
	assert(NULL != pkt);
	assert(NULL != addr);

	/* If the cookie key rotation time is up, rotate the key */
	now = time(NULL);
	if (now > ep->cookie_rotate_time + COOKIE_ROTATE_INTERVAL)
		lodp_rotate_cookie_key(ep);

	if ((PKT_INIT != pkt->hdr.type) && (PKT_HANDSHAKE != pkt->hdr.type))
		return (LODP_ERR_BAD_PACKET);

	/*
	 * Generate a cookie - OM NOM NOM
	 *
	 * This is swiped shamelessly from the DTLS RFC.  Cookies are a hash of
	 * the peer's source IP/Port combined with the immutable contents of
	 * the INIT packet.  Replay attacks are mitigated by rotating the hash
	 * key once every 30 seconds.
	 *
	 * Note:
	 * Checking for cookie reuse would be a good idea, though care must be
	 * taken to only consider cookies as "used" for connections that we
	 * have seen positive proof of the fact that the peer has completed a
	 * handshake.
	 *
	 * blob = Peer IP | Peer Port |  Peer Intro MAC Key |
	 *        Peer Intro Bulk Key |
	 * cookie = BLAKE2s(endpoint_cookie_key, blob);
	 */

	p = blob;
	if (AF_INET == addr->sa_family) {
		struct sockaddr_in *addr_v4 = (struct sockaddr_in *)addr;
		memcpy(p, &addr_v4->sin_addr.s_addr, 4);
		p += 4;
		memcpy(p, &addr_v4->sin_port, 2);
		p += 2;
	} else if (AF_INET6 == addr->sa_family) {
		struct sockaddr_in6 *addr_v6 = (struct sockaddr_in6 *)addr;
		memcpy(p, &addr_v6->sin6_addr.s6_addr, 16);
		p += 16;
		memcpy(p, &addr_v6->sin6_port, 2);
		p += 2;
	} else
		return (LODP_ERR_AFNOTSUPPORT);

	/* Both the INIT and HANDSHAKE packets put the keys in the same place */
	if (pkt->hdr.length < 4 + LODP_MAC_KEY_LEN + LODP_BULK_KEY_LEN)
		return (LODP_ERR_BAD_PACKET);

	memcpy(p, pkt->payload, LODP_MAC_KEY_LEN + LODP_BULK_KEY_LEN);
	p += LODP_MAC_KEY_LEN + LODP_BULK_KEY_LEN;

	ret = lodp_mac(cookie->bytes, blob, &ep->cookie_key, COOKIE_LEN,
		p - blob);
	lodp_memwipe(cookie, sizeof(cookie));
	return (ret);
}


static int
ntor_handshake(lodp_session *session, lodp_ecdh_public_key *pub_key)
{
	static const uint8_t PROTOID[] = {
		'l', 'o', 'd', 'p', '-', 'n', 't', 'o', 'r', '-', '1'
	};
	static const uint8_t RESPONDER[] = {
		'R', 'e', 's', 'p', 'o', 'n', 'd', 'e', 'r'
	};
	static const lodp_mac_key ss_key = {
		{
			'l', 'o', 'd', 'p', '-', 'n', 't', 'o', 'r', '-',
			'1', ':', 'k', 'e', 'y', '_', 'e', 'x', 't', 'r',
			'a', 'c', 't', 0
		}
	};
	static const lodp_mac_key verify_key = {
		{
			'l', 'o', 'd', 'p', '-', 'n', 't', 'o', 'r', '-',
			'1', ':', 'k', 'e', 'y', '_', 'e', 'x', 'p', 'a',
			'n', 'd', 0
		}
	};
	static const lodp_mac_key auth_key = {
		{
			'l', 'o', 'd', 'p', '-', 'n', 't', 'o', 'r', '-',
			'1', ':', 'm', 'a', 'c', 0
		}
	};

	struct __attribute__ ((__packed__)) {
		uint8_t secret_1[LODP_ECDH_SECRET_LEN];
		uint8_t secret_2[LODP_ECDH_SECRET_LEN];
		uint8_t B[LODP_ECDH_PUBLIC_KEY_LEN];
		uint8_t X[LODP_ECDH_PUBLIC_KEY_LEN];
		uint8_t Y[LODP_ECDH_PUBLIC_KEY_LEN];
		uint8_t id[sizeof(PROTOID)];
	} secret_input;
	struct __attribute__ ((__packed__)) {
		uint8_t verify[LODP_ECDH_SECRET_LEN];
		uint8_t B[LODP_ECDH_PUBLIC_KEY_LEN];
		uint8_t X[LODP_ECDH_PUBLIC_KEY_LEN];
		uint8_t Y[LODP_ECDH_PUBLIC_KEY_LEN];
		uint8_t id[sizeof(PROTOID)];
		uint8_t responder[sizeof(RESPONDER)];
	} auth_input;
	uint8_t verify[LODP_MAC_DIGEST_LEN];
	lodp_ecdh_shared_secret secret;
	const lodp_ecdh_public_key *X;
	const lodp_ecdh_public_key *Y;
	const lodp_ecdh_private_key *y;
	const lodp_ecdh_public_key *B;
	int ret;

	assert(NULL != session);
	assert(NULL != pub_key);

	/*
	 * WARNING: Here be dragons
	 *
	 * This is where we do the modified ntor handshake and obtain the
	 * session keys.  This routine is also only constant time when the
	 * handshake is successful and not when it fails.  This is ok because
	 * no indication of failure is sent on the wire.
	 */

	if (session->is_initiator) {
		const lodp_ecdh_private_key *x;

		/*
		 * Initiator:
		 *  * X -> session->session_ecdh_keypair.public_key
		 *  * x -> session->session_ecdh_keypair.private_key
		 *  * Y -> public_key
		 *  * B -> session->remote_public_key
		 *
		 * SecretInput = EXP(Y,x) | EXP(B,x) | B | X | Y | PROTOID
		 */

		X = &session->session_ecdh_keypair.public_key;
		x = &session->session_ecdh_keypair.private_key;
		Y = pub_key;
		B = &session->remote_public_key;

		lodp_ecdh(&secret, x, Y);
		if (lodp_ecdh_validate_pubkey(Y))
			goto out;
		memcpy(secret_input.secret_1, secret.secret, LODP_ECDH_SECRET_LEN);
		lodp_ecdh(&secret, x, B);
		if (lodp_ecdh_validate_pubkey(B))
			goto out;
		memcpy(secret_input.secret_2, secret.secret, LODP_ECDH_SECRET_LEN);
	} else {
		const lodp_ecdh_private_key *b;

		/*
		 * Responder:
		 *  * X-> public_key
		 *  * Y -> session->session_ecdh_keypair.public_key
		 *  * y -> session->session_ecdh_keypair.private_key
		 *  * B -> ep->intro_ecdh_keypair.public_key
		 *  * b -> ep->intro_ecdh_keypair.private_key
		 *
		 * SecretInput = EXP(X, y) | EXP(X,b) |  B | X | Y | PROTOID
		 */

		X = pub_key;
		Y = &session->session_ecdh_keypair.public_key;
		y = &session->session_ecdh_keypair.private_key;
		B = &session->ep->intro_ecdh_keypair.public_key;
		b = &session->ep->intro_ecdh_keypair.private_key;
		lodp_ecdh(&secret, y, X);
		if (lodp_ecdh_validate_pubkey(X))
			goto out;
		memcpy(secret_input.secret_1, secret.secret, LODP_ECDH_SECRET_LEN);
		lodp_ecdh(&secret, b, X);
		memcpy(secret_input.secret_2, secret.secret, LODP_ECDH_SECRET_LEN);
	}

	/*
	 * SharedSecret = H(PROTOID | ":key_extract", SecretInput)
	 * Verify = H(PROTOID | ":key_verify", SecretInput)
	 */

	memcpy(secret_input.id, PROTOID, sizeof(PROTOID));
	memcpy(secret_input.B, B->public_key, LODP_ECDH_PUBLIC_KEY_LEN);
	memcpy(secret_input.X, X->public_key, LODP_ECDH_PUBLIC_KEY_LEN);
	memcpy(secret_input.Y, Y->public_key, LODP_ECDH_PUBLIC_KEY_LEN);
	ret = lodp_mac(secret.secret, (uint8_t *)&secret_input, &ss_key,
		sizeof(secret.secret), sizeof(secret_input));
	if (ret)
		goto out;
	ret = lodp_mac(verify, (uint8_t *)&secret_input, &verify_key,
		sizeof(verify), sizeof(secret_input));
	if (ret)
		goto out;
	memcpy(session->session_secret.secret, secret.secret,
	    sizeof(secret.secret));

	/*
	 * AuthInput = Verify | B | Y | X | PROTOID | "Responder"
	 * Auth = H(PROTOID | ":mac", AuthInput)
	 */

	memcpy(auth_input.verify, verify, sizeof(verify));
	memcpy(auth_input.B, B->public_key, LODP_ECDH_PUBLIC_KEY_LEN);
	memcpy(auth_input.X, X->public_key, LODP_ECDH_PUBLIC_KEY_LEN);
	memcpy(auth_input.Y, Y->public_key, LODP_ECDH_PUBLIC_KEY_LEN);
	memcpy(auth_input.id, PROTOID, sizeof(PROTOID));
	memcpy(auth_input.responder, RESPONDER, sizeof(RESPONDER));
	ret = lodp_mac(session->session_secret_verifier, (uint8_t *)&auth_input,
		&auth_key, sizeof(session->session_secret_verifier),
		sizeof(auth_input));
	if (ret)
		goto out;

	if (session->is_initiator)
		ret = lodp_derive_sessionkeys(&session->tx_key, &session->rx_key, &secret);
	else
		ret = lodp_derive_sessionkeys(&session->rx_key, &session->tx_key, &secret);

out:
	lodp_memwipe(&secret_input, sizeof(secret_input));
	lodp_memwipe(&secret, sizeof(secret));
	lodp_memwipe(verify, sizeof(verify));
	lodp_memwipe(&auth_input, sizeof(auth_input));
	return ((0 == ret) ? ret : LODP_ERR_BAD_HANDSHAKE);
}


static void
scrub_handshake_material(lodp_session *session)
{
	assert(NULL != session);

	/* Wipe the cookie */
	if (NULL != session->cookie) {
		lodp_memwipe(session->cookie, sizeof(session->cookie_len));
		free(session->cookie);
		session->cookie = NULL;
	}

	/* Wipe the handshake parameters */
	lodp_memwipe(&session->session_ecdh_keypair,
	    sizeof(session->session_ecdh_keypair));

	/* Wipe the cached shared secret/validator */
	lodp_memwipe(&session->session_secret, sizeof(session->session_secret));
	lodp_memwipe(&session->session_secret_verifier,
	    sizeof(session->session_secret_verifier));
}


static int
on_init_pkt(lodp_endpoint *ep, const lodp_pkt_init *init_pkt, const struct
    sockaddr *addr, socklen_t addr_len)
{
	lodp_symmetric_key key;
	lodp_pkt_init_ack *pkt;
	lodp_buf *buf;
	int ret = 0;

	assert(NULL != ep);
	assert(NULL != init_pkt);
	assert(NULL != addr);

	/* Validate the INIT packet */
	if (PKT_HDR_INIT_LEN != init_pkt->hdr.length)
		return (LODP_ERR_BAD_PACKET);

	/*
	 * TODO: Implement rate limiting here, and silently drop the packet if
	 * rate limiting will be tripped.
	 */

	/* Pull out the peer's keys */
	memcpy(key.mac_key.mac_key, init_pkt->intro_mac_key, sizeof(key.mac_key.mac_key));
	memcpy(key.bulk_key.bulk_key, init_pkt->intro_bulk_key, sizeof(key.mac_key.mac_key));

	/* Generate the INIT ACK */
	buf = lodp_buf_alloc();
	if (NULL == buf)
		return (LODP_ERR_NOBUFS);

	buf->len = PKT_INIT_ACK_LEN + COOKIE_LEN;
	assert(buf->len < LODP_MSS);

	pkt = (lodp_pkt_init_ack *)buf->plaintext;
	pkt->hdr.type = PKT_INIT_ACK;
	pkt->hdr.flags = 0;
	pkt->hdr.length = htons(PKT_HDR_INIT_ACK_LEN + COOKIE_LEN);
	generate_cookie((lodp_cookie *)pkt->cookie, 0, ep, (lodp_pkt_raw *)init_pkt,
	    addr, addr_len);

	ret = encrypt_then_mac(ep, &key, buf);
	if (ret)
		goto out;
	ret = ep->callbacks.sendto_fn(ep, ep->ctxt, buf->ciphertext, buf->len,
		addr, addr_len);

out:
	lodp_memwipe(&key, sizeof(key));
	lodp_buf_free(buf);
	return (ret);
}


static int
on_handshake_pkt(lodp_endpoint *ep, lodp_session *session, const
    lodp_pkt_handshake *hs_pkt, const struct sockaddr *addr, socklen_t addr_len)
{
	lodp_ecdh_public_key pub_key;
	lodp_symmetric_key key;
	lodp_cookie cookie;
	lodp_pkt_handshake_ack *pkt;
	lodp_buf *buf;
	time_t now = time(NULL);
	int should_callback = 1;
	int ret = 0;

	assert(NULL != ep);
	assert(NULL != hs_pkt);
	assert(NULL != addr);

	/* Validate the HANDSHAKE packet */
	if (PKT_HDR_HANDSHAKE_LEN + COOKIE_LEN != hs_pkt->hdr.length)
		return (LODP_ERR_BAD_PACKET);

	/* Validate the cookie */
	ret = generate_cookie(&cookie, 0, ep, (lodp_pkt_raw *)hs_pkt, addr,
		addr_len);
	if (ret)
		goto out;
	if (lodp_memcmp(cookie.bytes, hs_pkt->cookie, COOKIE_LEN)) {
		/* If not match, check the previous cookie if not stale */
		if (now > ep->cookie_expire_time) {
			ret = LODP_ERR_INVALID_COOKIE;
			goto out;
		}
		ret = generate_cookie(&cookie, 1, ep, (lodp_pkt_raw *)hs_pkt,
			addr, addr_len);
		if (ret)
			goto out;
		if (lodp_memcmp(cookie.bytes, hs_pkt->cookie, COOKIE_LEN)) {
			ret = LODP_ERR_INVALID_COOKIE;
			goto out;
		}
	}

	/* Pull out the peer's keys */
	memcpy(key.mac_key.mac_key, hs_pkt->intro_mac_key, sizeof(key.mac_key.mac_key));
	memcpy(key.bulk_key.bulk_key, hs_pkt->intro_bulk_key, sizeof(key.mac_key.mac_key));
	memcpy(pub_key.public_key, hs_pkt->public_key, sizeof(pub_key.public_key));

	/*
	 * Chances are we will need to send a HANDSHAKE packet, so be optimistic
	 * and generate a HANDSHAKE ACK with everything but the validator, since
	 * we can.
	 */
	buf = lodp_buf_alloc();
	if (NULL == buf) {
		ret = LODP_ERR_NOBUFS;
		goto out_free;
	}
	buf->len = PKT_HANDSHAKE_ACK_LEN;
	assert(buf->len < LODP_MSS);

	pkt = (lodp_pkt_handshake_ack *)buf->plaintext;
	pkt->hdr.type = PKT_HANDSHAKE_ACK;
	pkt->hdr.flags = 0;
	pkt->hdr.length = htons(PKT_HDR_HANDSHAKE_ACK_LEN);

	/*
	 * If a session exists, a few things can have happened:
	 *  1) The HANDSHAKE_ACK got lost.
	 *  2) The one end crashed and is reusing the source port.
	 *     (Eg: RFC 793 "Half-Open Connections and Other Anomalies")
	 *  3) The client software is too damn lazy to implement their own
	 *     multiplexing and is wanting liblodp to do so.
	 *
	 * We detect 1, and retrasmit the HANDSHAKE_ACK.
	 *
	 * We ignore 2/3, on the assumption that the user implements timeouts
	 * on the responder side and will eventually kill off the stale session.
	 *
	 * I *could* go and add the notion of a RST I suppose, but that will not
	 * be a 0.0.1 feature.
	 *
	 * Case 3 is a WONTFIX on the assumpion that sockets client side are
	 * numerous.  Yes, I know Windows is brain damaged and one of the
	 * benefits of UDP is using 1 socket.  Write a proper upper layer that
	 * does multiplexing.
	 *
	 * Note:
	 * This case is explicitly not checked in the INIT handler because not
	 * doing so gives more time for either side to detect the condition and
	 * recover (It's a single packet, and cookie generation is dirt cheap).
	 */

	if (NULL != session) {
		/* Responder side TCBs start in the ESTABLISHED state */
		assert(!session->is_initiator);
		assert(STATE_ESTABLISHED != session->state);

		/*
		 * If we have not seen any payload from the user so far, the
		 * HANDSHAKE ACK got lost.  Retransmit it based off the cached
		 * shared secret/verifier.  There is no need to invoke the user
		 * callback a second time.
		 *
		 * If the protocol layered on top of LODP is of the "Server
		 * Talks First" variant, then the server potentially has
		 * transmited payload here, and wasted bandwidth, but there's
		 * nothing that can be done about that.
		 */

		if (!session->seen_peer_data)
			goto do_xmit;

		/*
		 * If there was payload received, then the peer is trying to
		 * open another connection reusing the source address (or
		 * someone is replaying a HANDSHAKE packet within it's window).
		 *
		 * Till there is a notion of a RST type packet, flat out ignore
		 * this and hope that the peer will go away + timeouts kick in
		 * and our upper layer expires the current session.
		 */

		ret = LODP_ERR_BAD_PACKET;
		goto out_free;
	}

	/* Generate a TCB */
	session = lodp_session_init(NULL, ep, addr, addr_len, pkt->public_key,
		sizeof(pkt->public_key), 0);
	if (NULL == session)
		goto out_free;

	/* Complete our side of the modified ntor handshake */
	ret = ntor_handshake(session, &pub_key);
	if (ret) {
		lodp_session_destroy(session);
		goto out_free;
	}

do_xmit:
	/* Finish building the HANDSHAKE ACK and transmit */
	memcpy(pkt->public_key,
	    session->session_ecdh_keypair.public_key.public_key,
	    sizeof(pkt->public_key));
	memcpy(pkt->digest, session->session_secret_verifier,
	    LODP_MAC_DIGEST_LEN);

	ret = encrypt_then_mac(ep, &key, buf);
	if (ret)
		goto out;
	ret = ep->callbacks.sendto_fn(ep, ep->ctxt, buf->ciphertext, buf->len,
		(struct sockaddr *)addr, addr_len);

	/* Inform the user of a incoming connection */
	if (should_callback)
		session->ep->callbacks.on_accept_fn(ep, ep->ctxt, session,
		    addr, addr_len);

	lodp_session_log(session, LODP_LOG_INFO, "Server Session Initialized");

out_free:
	lodp_buf_free(buf);
	lodp_memwipe(&key, sizeof(key));
	lodp_memwipe(&pub_key, sizeof(pub_key));
out:
	lodp_memwipe(&cookie, sizeof(cookie));
	return (ret);
}


static int
on_data_pkt(lodp_session *session, const lodp_pkt_data *pkt)
{
	const uint8_t *payload;
	uint16_t payload_len;
	int ret;

	assert(NULL != session);
	assert(NULL != pkt);
	assert(PKT_DATA == pkt->hdr.type);

	if (session->state != STATE_ESTABLISHED)
		return (LODP_ERR_BAD_PACKET);

	/*
	 * If this is the first DATA packet we received over an existing
	 * connection, and we are the responder, it is safe to wipe the keying
	 * material used for the HANDSHAKE now.  Before this point, it is
	 * beneficial to hold onto the shared secret/verifier used for session
	 * key derivation to save from having to redo the modified ntor
	 * handshake if a HANDSHAKE ACK gets lost.
	 */

	if (!session->seen_peer_data) {
		session->seen_peer_data = 1;
		if (!session->is_initiator)
			scrub_handshake_material(session);
	}

	/*
	 * Note:
	 * The packet header including the length is already known to be valid
	 * at this point.  No further validation neccecary since we support
	 * payloads ranging from 0 bytes up to the maximum.
	 */

	payload = pkt->data;
	payload_len = pkt->hdr.length - PKT_HDR_DATA_LEN;
	ret = session->ep->callbacks.on_recv_fn(session, session->ctxt,
		payload, payload_len);
	return (ret);
}


static int
on_init_ack_pkt(lodp_session *session, const lodp_pkt_init_ack *pkt)
{
	uint8_t *cookie;
	size_t cookie_len;

	assert(NULL != session);
	assert(NULL != pkt);
	assert(PKT_INIT_ACK == pkt->hdr.type);

	/* INIT ACK when in invalid states is silently dropped */
	if ((!session->is_initiator) || (STATE_INIT != session->state))
		return (LODP_ERR_BAD_PACKET);

	/*
	 * Save the cookie
	 *
	 * Note:
	 * Yes, this is a malloc in the critical path.  While it is possible to
	 * assume that the peer is using the liblodp cookie format and include
	 * a static cookie field in the TCB, this will break with non-liblodp
	 * implementations and isn't future proof.
	 */

	cookie_len = pkt->hdr.length - PKT_HDR_INIT_ACK_LEN;
	if (cookie_len == 0)
		return (LODP_ERR_BAD_PACKET);

	cookie = calloc(1, cookie_len);
	if (NULL == cookie) {
		session->state = STATE_ERROR;
		session->ep->callbacks.on_connect_fn(session, session->ctxt,
		    LODP_ERR_NOBUFS);
		return (LODP_ERR_NOBUFS);
	}
	memcpy(cookie, pkt->cookie, cookie_len);

	session->cookie = cookie;
	session->cookie_len = cookie_len;

	/* Send a HANDSHAKE */
	session->state = STATE_HANDSHAKE;
	return (lodp_handshake(session));
}


static int
on_handshake_ack_pkt(lodp_session *session, const lodp_pkt_handshake_ack *pkt)
{
	lodp_ecdh_public_key pub_key;
	int ret = 0;

	assert(NULL != session);
	assert(NULL != pkt);
	assert(PKT_HANDSHAKE_ACK == pkt->hdr.type);

	/* HANDSHAKE ACK when in invalid states is silently dropped */
	if ((!session->is_initiator) || (STATE_HANDSHAKE != session->state))
		return (LODP_ERR_BAD_PACKET);

	/* Validate the HANDSHAKE ACK */
	if (PKT_HDR_HANDSHAKE_ACK_LEN != pkt->hdr.length)
		return (LODP_ERR_BAD_PACKET);

	/* Pull out the responder's public key */
	memcpy(pub_key.public_key, pkt->public_key, sizeof(pub_key.public_key));

	/* Complete our side of the modified ntor handshake */
	ret = ntor_handshake(session, &pub_key);
	if (ret) {
		session->state = STATE_ERROR;
		goto out;
	}

	/* Confirm that the correct shared secret was derived */
	if (lodp_memcmp(pkt->digest, session->session_secret_verifier,
	    sizeof(pkt->digest))) {
		session->state = STATE_ERROR;
		ret = LODP_ERR_BAD_HANDSHAKE;
		goto out;
	}

	/* Inform the user that the connection is established */
	session->state = STATE_ESTABLISHED;
out:
	scrub_handshake_material(session);
	session->ep->callbacks.on_connect_fn(session, session->ctxt, ret);
	return (ret);
}


static int
on_heartbeat_pkt(lodp_session *session, const lodp_pkt_heartbeat *hb_pkt)
{
	const uint8_t *payload;
	uint16_t payload_len;
	lodp_pkt_heartbeat_ack *pkt;
	lodp_buf *buf;
	int ret;

	assert(NULL != session);
	assert(NULL != hb_pkt);
	assert(PKT_HEARTBEAT == hb_pkt->hdr.type);

	if (session->state != STATE_ESTABLISHED)
		return (LODP_ERR_BAD_PACKET);

	/*
	 * TODO: Implement rate limiting here, and silently drop the packet if
	 * the rate limit will get tripped.
	 */

	/*
	 * If execution gets here, the the packet's lenght field is valid, so
	 * just directly echo the heartbeat data in the HEARTBEAT ACK.
	 */

	payload = hb_pkt->data;
	payload_len = hb_pkt->hdr.length - PKT_HDR_HEARTBEAT_LEN;

	buf = lodp_buf_alloc();
	if (NULL == buf)
		return (LODP_ERR_NOBUFS);

	buf->len = PKT_HEARTBEAT_ACK_LEN + payload_len;
	assert(buf->len < LODP_MSS);

	pkt = (lodp_pkt_heartbeat_ack *)buf->plaintext;
	pkt->hdr.type = PKT_HEARTBEAT_ACK;
	pkt->hdr.flags = 0;
	pkt->hdr.length = htons(PKT_HDR_HEARTBEAT_ACK_LEN + payload_len);
	memcpy(pkt->data, payload, payload_len);

	ret = encrypt_then_mac(session->ep, &session->tx_key, buf);
	if (ret)
		goto out;
	ret = session->ep->callbacks.sendto_fn(session->ep, session->ep->ctxt,
		buf->ciphertext, buf->len, (struct sockaddr *)
		&session->peer_addr, session->peer_addr_len);
out:
	lodp_buf_free(buf);
	return (ret);
}


static int
on_heartbeat_ack_pkt(lodp_session *session, const lodp_pkt_heartbeat_ack *pkt)
{
	const uint8_t *payload;
	uint16_t payload_len;

	assert(NULL != session);
	assert(NULL != pkt);
	assert(PKT_HEARTBEAT_ACK == pkt->hdr.type);

	if (session->state != STATE_ESTABLISHED)
		return (LODP_ERR_BAD_PACKET);

	/*
	 * If execution gets here, the the packet's lenght field is valid, so
	 * just inform the user that a HEARTBEAT ACK has arrived.
	 */

	payload = pkt->data;
	payload_len = pkt->hdr.length - PKT_HDR_HEARTBEAT_ACK_LEN;
	if (NULL != session->ep->callbacks.on_heartbeat_ack_fn)
		session->ep->callbacks.on_heartbeat_ack_fn(session,
		    session->ctxt, payload, payload_len);
	return (0);
}