/*
 * Copyright (c) Sun Microsystems, Inc. 1993-1998 All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * 3. All advertising materials mentioning features or use of this software
 *    must display the following acknowledgement:
 *      This product includes software developed by the SMCC Technology
 *      Development Group at Sun Microsystems, Inc.
 *
 * 4. The name of the Sun Microsystems, Inc nor may not be used to endorse or
 *      promote products derived from this software without specific prior
 *      written permission.
 *
 * SUN MICROSYSTEMS DOES NOT CLAIM MERCHANTABILITY OF THIS SOFTWARE OR THE
 * SUITABILITY OF THIS SOFTWARE FOR ANY PARTICULAR PURPOSE.  The software is
 * provided "as is" without express or implied warranty of any kind.
 *
 * These notices must be retained in any copies of any part of this software.
 * 
 * CoDel - The Controlled-Delay Active Queue Management algorithm
 */

#include "opt_altq.h"
#include "opt_inet.h"
#ifdef ALTQ_CODEL	/* cbq is enabled by ALTQ_CBQ option in opt_altq.h */

#include <sys/param.h>
#include <sys/malloc.h>
#include <sys/mbuf.h>
#include <sys/socket.h>
#include <sys/systm.h>
#include <sys/proc.h>
#include <sys/errno.h>
#include <sys/time.h>

#include <net/if.h>
#include <netinet/in.h>

#if NPF > 0
#include <net/pfvar.h>
#endif

#include <altq/if_altq.h>
#include <altq/altq.h>
#include <altq/altq_codel.h>

static int codel_request(struct ifaltq *, int, void *);

static int codel_enqueue(struct ifaltq *, struct mbuf *, struct altq_pktattr *);
static struct mbuf *codel_dequeue(struct ifaltq *, int);


int
codel_pfattach(struct pf_altq *a)
{
	struct ifnet *ifp;

	if ((ifp = ifunit(a->ifname)) == NULL || a->altq_disc == NULL)
		return EINVAL;

	return (altq_attach(&ifp->if_snd, ALTQT_CODEL, a->altq_disc,
	    codel_enqueue, codel_dequeue, codel_request));
}

static int
codel_request(struct ifaltq *ifq, int req, void *arg)
{
	struct codel_if	*cif = (struct codel_if *)ifq->altq_disc;
	struct mbuf *m;

	IFQ_LOCK_ASSERT(ifq);

	switch (req) {
	case ALTRQ_PURGE:
		if (!ALTQ_IS_ENABLED(cif->cif_ifq))
			break;

		if (qempty(cif->cl_q))
			break;

		while ((m = _getq(cif->cl_q)) != NULL) {
			PKTCNTR_ADD(&cif->cl_stats.cl_dropcnt, m_pktlen(m));
			m_freem(m);
			IFQ_DEC_LEN(cif->cif_ifq);
		}
		cif->cif_ifq->ifq_len = 0;
		break;
	}

	return 0;
}

static int
codel_enqueue(struct ifaltq *ifq, struct mbuf *m, struct altq_pktattr *pktattr)
{

	struct codel_if *cif = (struct codel_if *) ifq->altq_disc;

	IFQ_LOCK_ASSERT(ifq);

	/* grab class set by classifier */
	if ((m->m_flags & M_PKTHDR) == 0) {
		/* should not happen */
		printf("altq: packet for %s does not have pkthdr\n",
		   ifq->altq_ifp->if_xname);
		m_freem(m);
		PKTCNTR_ADD(&cif->cl_stats.cl_dropcnt, m_pktlen(m));
		return ENOBUFS;
	}

	if (codel_addq(&cif->codel, cif->cl_q, m)) {
		PKTCNTR_ADD(&cif->cl_stats.cl_dropcnt, m_pktlen(m));
		return ENOBUFS;
	}
	IFQ_INC_LEN(ifq);

	return 0;
}

static struct mbuf *
codel_dequeue(struct ifaltq *ifq, int op)
{
	struct codel_if *cif = (struct codel_if *)ifq->altq_disc;
	struct mbuf *m;

	IFQ_LOCK_ASSERT(ifq);

	if (IFQ_IS_EMPTY(ifq))
		return NULL;

	if (op == ALTDQ_POLL)
		return (qhead(cif->cl_q));

	m = codel_getq(&cif->codel, cif->cl_q);
	if (m != NULL) {
		IFQ_DEC_LEN(ifq);
		PKTCNTR_ADD(&cif->cl_stats.cl_xmitcnt, m_pktlen(m));
		return m;
	}

	return NULL;
}

#endif /* ALTQ_CODEL */