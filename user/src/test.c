#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <arpa/inet.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <netinet/in_systm.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#define __FAVOR_BSD
#include <netinet/tcp.h>
#include <err.h>
#include <string.h>

#include "tcpcrypt_ctl.h"
#include "tcpcrypt.h"
#include "tcpcryptd.h"
#include "crypto.h"
#include "profile.h"
#include "divert.h"
#include "test.h"

static struct state {
	int	s_dlen;
	int	s_drop_packet_num;
	int	s_drop_times;
	int	s_drop_hook;
} _state;

static struct tc *create_tc(void)
{
	struct tc *tc = xmalloc(sizeof(*tc));

	return tc;
}

static void destroy_tc(struct tc *tc)
{
	if (tc->tc_crypt_ops)
		crypto_finish(tc);

	free(tc);
}

static struct tc *setup_cipher(int type, int id)
{
	struct tc *tc = create_tc();
	int klen = 20;
	void *key;

	tc->tc_crypt_ops = crypto_find_cipher(type, id);
	if (!tc->tc_crypt_ops)
		errx(1, "Can't find cipher %d (type %d)", id, type);

	crypto_init(tc);

	key = alloca(klen);
	assert(key);

	memset(key, 0, klen);

	crypto_set_key(tc, key, klen);

	return tc;
}

static unsigned int cipher_throughput(float sample, unsigned int avg)
{
	unsigned int ops;
	unsigned int bits;

	ops  = (unsigned int) (sample * 1000.0 * 1000.0);
	bits = (unsigned int) (sample * (float) _state.s_dlen * 8.0);

	printf("%u ops / sec (%u Mbit/s) [avg %u]\n", ops, bits, avg);

	return ops;
}

void test_sym_throughput(void)
{
	struct tc *tc;
	int id	    = TC_AES128_CTR_SEQIV;
	uint64_t iv = 0;
	int dlen    = 1420;
	void *data;

	tc   	      = setup_cipher(TYPE_SYM, id);
	data	      = alloca(dlen);
	_state.s_dlen = dlen;
	memset(data, 0, dlen);

	printf("Encrypting %d bytes of data\n", dlen);

	speed_start(cipher_throughput);

	while (1) {
		crypto_encrypt(tc, &iv, data, dlen);
		speed_add(1);
	}

	destroy_tc(tc);
}

static int get_test_param(int idx, int def)
{
	char *p = test_param(idx);

	if (!p)
		return def;

	return atoi(p);
}

void test_mac_throughput(void)
{
	struct tc *tc;
	int id = TC_HMAC_SHA1_128;
	int len = get_test_param(0, 8);
	int num = get_test_param(1, 1);
	struct iovec *iov;
	int i;
	unsigned char out[1024];
	int outlen = sizeof(out);

	tc = setup_cipher(TYPE_MAC, id);

	iov = alloca(sizeof(*iov) * num);

	for (i = 0; i < num; i++) {
		iov[i].iov_len  = len;
		iov[i].iov_base = alloca(iov[i].iov_len);
		memset(iov[i].iov_base, 0, iov[i].iov_len);
	}

	printf("MACing %d iovecs of %d bytes each\n", num, len);

	speed_start(cipher_throughput);

	while (1) {
		crypto_mac(tc, iov, num, NULL, out, &outlen);
		speed_add(1);
	}

	destroy_tc(tc);
}

void print_packet(struct ip *ip, struct tcphdr *tcp, int flags, struct tc *tc)
{       
        char src[16];
        char flagz[16];
        int i = 0;
	int level = XP_NOISY;

        if (_conf.cf_verbose < level)
                return;

        if (tcp->th_flags & TH_SYN)
                flagz[i++] = 'S';

        if (tcp->th_flags & TH_ACK)
                flagz[i++] = 'A';

        if (tcp->th_flags & TH_RST)
                flagz[i++] = 'R';

        if (tcp->th_flags & TH_FIN)
                flagz[i++] = 'F';

        flagz[i] = 0;

        strcpy(src, inet_ntoa(ip->ip_src));
        xprintf(level, "%s:%d->%s:%d %d %s [%s] tc %p\n",
                src,
                ntohs(tcp->th_sport),
                inet_ntoa(ip->ip_dst),
                ntohs(tcp->th_dport),
                ntohs(ip->ip_len),
                flagz,
                flags & DF_IN ? "in" : "out",
                tc);
}

static int dropper(int rc, void *packet, int len, int flags)
{
	if (_state.s_drop_packet_num != 1) {
		_state.s_drop_packet_num--;
		return rc;
	}

	if (_state.s_drop_times != 0) {
		struct ip *ip = packet;
		struct tcphdr *tcp;

		tcp = (struct tcphdr *) ((unsigned long) ip + (ip->ip_hl << 2));

		xprintf(XP_NOISY, "Dropping: ");
		print_packet(ip, tcp, flags, NULL);

		_state.s_drop_packet_num--;

		return DIVERT_DROP;
	}

	return rc;
}

static int dropper_pre(int rc, void *packet, int len, int flags)
{
	if (flags & DF_IN)
		return dropper(rc, packet, len, flags);

	return rc;
}

static int dropper_post(int rc, void *packet, int len, int flags)
{
	if (flags & DF_IN)
		return rc;

	return dropper(rc, packet, len, flags);
}

void test_dropper(void)
{
	_state.s_drop_packet_num   = get_test_param(0, 0);
	_state.s_drop_times	   = get_test_param(1, 1);
	_state.s_drop_hook	   = get_test_param(2, -1);

	if (_state.s_drop_packet_num <= 0)
		errx(1, "Need a packet number parameter.  1 is first.");

	switch (_state.s_drop_hook) {
	case -1:
		set_packet_hook(0, dropper_pre);
		set_packet_hook(1, dropper_post);
		break;

	default:
		set_packet_hook(_state.s_drop_hook, dropper);
		break;
	}

	tcpcryptd();
}