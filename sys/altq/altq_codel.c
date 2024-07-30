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

#endif /* ALTQ_CODEL */