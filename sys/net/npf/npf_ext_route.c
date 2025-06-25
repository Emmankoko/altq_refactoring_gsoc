/*-
 * Copyright (c) 2013 The NetBSD Foundation, Inc.
 * All rights reserved.
 *
 * This code is derived from software contributed to The NetBSD Foundation
 * by Mindaugas Rasiukevicius.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE NETBSD FOUNDATION, INC. AND CONTRIBUTORS
 * ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 * TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE FOUNDATION OR CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

/*
 * NPF route extension.
 */

#include <sys/cdefs.h>
__KERNEL_RCSID(0, "$NetBSD$");

#include <sys/param.h>
#include <sys/types.h>
#include <sys/module.h>

#include <sys/conf.h>
#include <sys/kmem.h>
#include <sys/mbuf.h>
#include <sys/mutex.h>
#include <sys/queue.h>

#include <net/if.h>
#include <net/if_types.h>
#include <net/bpf.h>
#include <net/route.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <netinet/ip6.h>
#include <netinet6/in6_var.h>
#include <netinet6/nd6.h>

#include "npf_impl.h"

NPF_EXT_MODULE(npf_ext_route, "");

#define        NPFEXT_ROUTE_VER                1

static void *          npf_ext_route_id;

typedef struct {
    char            ifname[IFNAMSIZ];
	const char *	inet64_addr;
} npf_ext_route_t;

static int
npf_route_ctor(npf_rproc_t *rp, const nvlist_t* params)
{
	npf_ext_route_t *meta;
	const char *ifname;

	meta = kmem_zalloc(sizeof(*meta), KM_SLEEP);
	ifname = nvlist_get_string(params, "route-interface");
	/* XXX use something like npf_ifmap */
	strlcpy(meta->ifname, ifname, IFNAMSIZ);
	npf_rproc_assign(rp, meta);
	return 0;
}

static void
npf_route_dtor(npf_rproc_t *rp, void *meta)
{
	kmem_free(meta, sizeof(*rp));
}

static bool
npf_route(npf_cache_t *npc, nbuf_t *nbuf, void *meta, int *decision)
{
	struct mbuf *m = nbuf_head_mbuf(nbuf);
	const npf_ext_route_t *route = meta;
	struct ifnet *ifp;
	union {
			struct sockaddr_in v4;
			struct sockaddr_in6 v6;
	} dst;
	int error = 0;

	/* Skip, if already blocking. */
	if (*decision == NPF_DECISION_BLOCK) {
			return true;
	}

	KERNEL_LOCK(1, NULL);
	ifp = ifunit(route->ifname);
	if (ifp == NULL) {
			/* XXX: oops */
			goto bad;
	}

	if (npf_iscached(npc, NPC_IP6)) {
			struct ip6_hdr *ip6 = npc->npc_ip.v6;
			sockaddr_in6_init(&dst.v6, &ip6->ip6_dst, 0, 0, 0);

			if (IN6_IS_SCOPE_EMBEDDABLE(&dst.v6.sin6_addr))
					dst.v6.sin6_addr.s6_addr16[1] = htons(ifp->if_index);

			if (m->m_pkthdr.len <= ifp->if_mtu) {
					error = ip6_if_output(ifp, ifp, m, &dst.v6, NULL);
			} else {
					in6_ifstat_inc(ifp, ifs6_in_toobig);
					icmp6_error(m, ICMP6_PACKET_TOO_BIG, 0, ifp->if_mtu);
			}
	} else if (npf_iscached(npc, NPC_IP4)) {
			struct ip *ip = npc->npc_ip.v4;
			struct mbuf *m0 = m, *m1;
			sockaddr_in_init(&dst.v4, &ip->ip_dst, 0);

			/* copied from pf_route, which was also copied from ip_output */
			if (m->m_pkthdr.csum_flags & (M_CSUM_TCPv4|M_CSUM_UDPv4)) {
				in_undefer_cksum_tcpudp(m);
				m->m_pkthdr.csum_flags &= ~(M_CSUM_TCPv4|M_CSUM_UDPv4);
			}

			if (ntohs(ip->ip_len) <= ifp->if_mtu) {

				error = if_output_lock(ifp, ifp, m, (struct sockaddr *)&dst.v4, NULL);
			} else {
				/*
				 * Too large for interface; fragment if possible.
				 * Must be able to put at least 8 bytes per fragment.
				 */
				if (ip->ip_off & htons(IP_DF)) {
					ip_statinc(IP_STAT_CANTFRAG);
					icmp_error(m0, ICMP_UNREACH, ICMP_UNREACH_NEEDFRAG, 0,
					    ifp->if_mtu);
					goto done;
				}

				/* Make ip_fragment re-compute checksums. */
				if (IN_NEED_CHECKSUM(ifp, M_CSUM_IPv4)) {
					m0->m_pkthdr.csum_flags |= M_CSUM_IPv4;
				}
				m1 = m0;
				error = ip_fragment(m0, ifp, ifp->if_mtu);
				if (error) {
					m0 = NULL;
					goto bad;
				}

				for (m0 = m1; m0; m0 = m1) {
					m1 = m0->m_nextpkt;
					m0->m_nextpkt = 0;
					if (error == 0)
						error = (*ifp->if_output)(ifp, m0, dst, NULL);
					else
						m_freem(m0);
				}

				if (error == 0) {
					ip_statinc(IP_STAT_FRAGMENTED);
				}
			}
	}
	if (error) {
			/* XXX: statistics */
			goto done;
	}

done:
	KERNEL_UNLOCK_ONE(NULL);
	return false;

bad:
	m_freem(m0);
	KERNEL_UNLOCK_ONE(NULL);
	return true;
}

__dso_public int
npf_ext_route_init(npf_t *npf)
{
	static const npf_ext_ops_t npf_route_ops = {
		.version        = NPFEXT_ROUTE_VER,
		.ctx            = NULL,
		.ctor           = npf_route_ctor,
		.dtor           = npf_route_dtor,
		.proc           = npf_route
	};
	npf_ext_route_id = npf_ext_register(npf, "route", &npf_route_ops);
	return npf_ext_log_id ? 0 : EEXIST;
}

__dso_public int
npf_ext_route_fini(npf_t *npf)
{
	return npf_ext_unregister(npf, npf_ext_route_id);
}

#ifdef _KERNEL
static int
npf_ext_route_modcmd(modcmd_t cmd, void *arg)
{
	int error;

	switch (cmd) {
	case MODULE_CMD_INIT:
		return npf_ext_route_init(npf);
	case MODULE_CMD_FINI:
		return npf_ext_route_fini(npf);
	case MODULE_CMD_AUTOUNLOAD:
		/* Allow auto-unload only if NPF permits it. */
		return npf_autounload_p() ? 0 : EBUSY;
	default:
		return ENOTTY;
	}
	return 0;
}
#endif
