/*-
 * Copyright (c) 2025 The NetBSD Foundation, Inc.
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
	struct mbuf *m0 = nbuf_head_mbuf(nbuf);
	const npf_ext_route_t *route = meta;
	struct ifnet *ifp;
	int sw_csum, hlen;
	union {
			struct sockaddr_in6 src;
			struct sockaddr_in6 dst;
	} v6;
	union {
		struct sockaddr_in src;
		struct sockaddr_in dst;
	} v4;
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
			sockaddr_in6_init(&v6.dst, &ip6->ip6_dst, 0, 0, 0);

			if (IN6_IS_SCOPE_EMBEDDABLE(&v6.dst.sin6_addr))
					v6.dst.sin6_addr.s6_addr16[1] = htons(ifp->if_index);

			if (m->m_pkthdr.len <= ifp->if_mtu) {
					error = ip6_if_output(ifp, ifp, m0, &v6.dst, NULL);
			} else {
					in6_ifstat_inc(ifp, ifs6_in_toobig);
					icmp6_error(m, ICMP6_PACKET_TOO_BIG, 0, ifp->if_mtu);
			}
	} else if (npf_iscached(npc, NPC_IP4)) {
			struct ip *ip = npc->npc_ip.v4;
			struct mbuf *m = m0;
			sockaddr_in_init(&v4.dst, &ip->ip_dst, 0);
			sockaddr_in_init(&v4.src, &ip->ip_src, 0);

			hlen = ip->ip_hl << 2;

			m->m_pkthdr.csum_data |= hlen << 16;

			/* NB: This code is copied from ip_output */
			if (m->m_pkthdr.csum_flags & (M_CSUM_TCPv4|M_CSUM_UDPv4)) {
				in_undefer_cksum_tcpudp(m);
				m->m_pkthdr.csum_flags &= ~(M_CSUM_TCPv4|M_CSUM_UDPv4);
			}
			if (ntohs(ip->ip_len) <= ifp->if_mtu) {

				error = ip_if_output(ifp, m, sintocsa(&dst.v4), rt);
				goto done;
			}

			/*
			 * Too large for interface; fragment if possible.
			 * Must be able to put at least 8 bytes per fragment.
			 */
			if (ntohs(ip->ip_off) & IP_DF) {
				ip_statinc(IP_STAT_CANTFRAG);
				icmp_error(m0, ICMP_UNREACH, ICMP_UNREACH_NEEDFRAG, 0,
					ifp->if_mtu);
				goto bad;
			}

			error = ip_fragment(m, ifp, mtu);
			if (error) {
				m = NULL;
				goto bad;
			}

			for (; m; m = m0) {
				m0 = m->m_nextpkt;
				m->m_nextpkt = NULL;
				if (error) {
					m_freem(m);
					continue;
				}

				KASSERT((m->m_pkthdr.csum_flags &
					(M_CSUM_UDPv4 | M_CSUM_TCPv4)) == 0);
				error = ip_if_output(ifp, m, sintocsa(&dst.v4), NULL);
			}

			if (error == 0) {
				IP_STATINC(IP_STAT_FRAGMENTED);
			}

		}

	if (error) {
			/* XXX: statistics */
			goto done;
	}




	ip = mtod(m, struct ip *);
	hlen = ip->ip_hl << 2;

	m->m_pkthdr.csum_data |= hlen << 16;

	/*
	 * search for the source address structure to
	 * maintain output statistics, and verify address
	 * validity
	 */
	KASSERT(ia == NULL);
	sockaddr_in_init(&v4.sin, &ip->ip_src, 0);
	ia = ifatoia(ifaof_ifpforaddr_psref(&v4.src, ifp, &psref_ia));

	/*
	 * Ensure we only send from a valid address.
	 * A NULL address is valid because the packet could be
	 * generated from a packet filter.
	 */
	if (ia != NULL && (flags & IP_FORWARDING) == 0 &&
	    (error = ip_ifaddrvalid(ia)) != 0)
	{
		ARPLOG(LOG_ERR,
		    "refusing to send from invalid address %s (pid %d)\n",
		    ARPLOGADDR(&ip->ip_src), curproc->p_pid);
		IP_STATINC(IP_STAT_ODROPPED);
		if (error == 1)
			/*
			 * Address exists, but is tentative or detached.
			 * We can't send from it because it's invalid,
			 * so we drop the packet.
			 */
			error = 0;
		else
			error = EADDRNOTAVAIL;
		goto bad;
	}

	/* Maybe skip checksums on loopback interfaces. */
	if (IN_NEED_CHECKSUM(ifp, M_CSUM_IPv4)) {
		m->m_pkthdr.csum_flags |= M_CSUM_IPv4;
	}
	sw_csum = m->m_pkthdr.csum_flags & ~ifp->if_csum_flags_tx;

	/* Need to fragment the packet */
	if (ntohs(ip->ip_len) > mtu &&
	    (m->m_pkthdr.csum_flags & M_CSUM_TSOv4) == 0) {
		goto fragment;
	}

#if IFA_STATS
	if (ia)
		ia->ia_ifa.ifa_data.ifad_outbytes += ntohs(ip->ip_len);
#endif
	/*
	 * Always initialize the sum to 0!  Some HW assisted
	 * checksumming requires this.
	 */
	ip->ip_sum = 0;

	if ((m->m_pkthdr.csum_flags & M_CSUM_TSOv4) == 0) {
		/*
		 * Perform any checksums that the hardware can't do
		 * for us.
		 *
		 * XXX Does any hardware require the {th,uh}_sum
		 * XXX fields to be 0?
		 */
		if (sw_csum & M_CSUM_IPv4) {
			KASSERT(IN_NEED_CHECKSUM(ifp, M_CSUM_IPv4));
			ip->ip_sum = in_cksum(m, hlen);
			m->m_pkthdr.csum_flags &= ~M_CSUM_IPv4;
		}
		if (sw_csum & (M_CSUM_TCPv4|M_CSUM_UDPv4)) {
			if (IN_NEED_CHECKSUM(ifp,
			    sw_csum & (M_CSUM_TCPv4|M_CSUM_UDPv4))) {
				in_undefer_cksum_tcpudp(m);
			}
			m->m_pkthdr.csum_flags &=
			    ~(M_CSUM_TCPv4|M_CSUM_UDPv4);
		}
	}

	sa = (m->m_flags & M_MCAST) ? sintocsa(rdst) : sintocsa(dst);

	/* Send it */
	if (__predict_false(sw_csum & M_CSUM_TSOv4)) {
		/*
		 * TSO4 is required by a packet, but disabled for
		 * the interface.
		 */
		error = ip_tso_output(ifp, m, sa, rt);
	} else
		error = ip_if_output(ifp, m, sa, rt);
	goto done;

fragment:
	/*
	 * We can't use HW checksumming if we're about to fragment the packet.
	 *
	 * XXX Some hardware can do this.
	 */
	if (m->m_pkthdr.csum_flags & (M_CSUM_TCPv4|M_CSUM_UDPv4)) {
		if (IN_NEED_CHECKSUM(ifp,
		    m->m_pkthdr.csum_flags & (M_CSUM_TCPv4|M_CSUM_UDPv4))) {
			in_undefer_cksum_tcpudp(m);
		}
		m->m_pkthdr.csum_flags &= ~(M_CSUM_TCPv4|M_CSUM_UDPv4);
	}

	/*
	 * Too large for interface; fragment if possible.
	 * Must be able to put at least 8 bytes per fragment.
	 */
	if (ntohs(ip->ip_off) & IP_DF) {
		if (flags & IP_RETURNMTU) {
			KASSERT(inp != NULL);
			in4p_errormtu(inp) = mtu;
		}
		error = EMSGSIZE;
		IP_STATINC(IP_STAT_CANTFRAG);
		goto bad;
	}

	error = ip_fragment(m, ifp, mtu);
	if (error) {
		m = NULL;
		goto bad;
	}

	for (; m; m = m0) {
		m0 = m->m_nextpkt;
		m->m_nextpkt = NULL;
		if (error) {
			m_freem(m);
			continue;
		}
#if IFA_STATS
		if (ia)
			ia->ia_ifa.ifa_data.ifad_outbytes += ntohs(ip->ip_len);
#endif
		/*
		 * If we get there, the packet has not been handled by
		 * IPsec whereas it should have. Now that it has been
		 * fragmented, re-inject it in ip_output so that IPsec
		 * processing can occur.
		 */
		if (natt_frag) {
			error = ip_output(m, opt, NULL,
			    flags | IP_RAWOUTPUT | IP_NOIPNEWID,
			    imo, inp);
		} else {
			KASSERT((m->m_pkthdr.csum_flags &
			    (M_CSUM_UDPv4 | M_CSUM_TCPv4)) == 0);
			error = ip_if_output(ifp, m, (m->m_flags & M_MCAST) ?
			    sintocsa(rdst) : sintocsa(dst), rt);
		}
	}
	if (error == 0) {
		IP_STATINC(IP_STAT_FRAGMENTED);
	}

done:
	ia4_release(ia, &psref_ia);
	rtcache_unref(rt, ro);
	if (ro == &iproute) {
		rtcache_free(&iproute);
	}
	if (mifp != NULL) {
		if_put(mifp, &psref);
	}
	if (bind_need_restore)
		curlwp_bindx(bound);
	return error;

bad:
	m_freem(m);
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
