/*-
 * Copyright (c) 2010-2012 The NetBSD Foundation, Inc.
 * All rights reserved.
 *
 * This material is based upon work partially supported by The
 * NetBSD Foundation under a contract with Mindaugas Rasiukevicius.
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
#include <sys/unistd.h>
#include <sys/param.h>
#include <sys/types.h>
#include <sys/queue.h>
#include <sys/tree.h>
#include <sys/rwlock.h>
#include <sys/pool.h>

#include <netinet/in.h>
#include <netinet/in_systm.h>
#include <netinet/ip.h>
#include <netinet/ip_var.h>
#include <netinet/ip_icmp.h>

#ifdef _KERNEL_OPT
#include "opt_inet.h"
#include "opt_altq.h"
#endif

#ifdef ALTQ
#include <altq/altq.h>
#endif

#include "npf_altq.h"
#include "npf.h"

/*
 * starting point of altq kernel routines
 */
bool	 npf_altq_running = false;
u_int32_t		 nticket_altqs_active;
struct npf_altqqueue	*npf_altqs_active;
struct npf_altqqueue  npf_altqs[2];

#ifdef ALTQ

/* npf interface to start altq */
int
npf_altq_start(void)
{
	int error;
    struct npf_altq		*altq;
    /* enable all altq interfaces on active list */
    TAILQ_FOREACH(altq, npf_altqs_active, entries) {
        if (altq->qname[0] == 0) {
            error = npf_enable_altq(altq);
            if (error != 0)
                break;
        }
    }
    if (error == 0)
        npf_altq_running = true;
    //DPFPRINTF(PF_DEBUG_MISC, ("altq: started\n"));
	return error;
}

int
npf_enable_altq(struct npf_altq *altq)
{
	struct ifnet		*ifp;
	struct tb_profile	 tb;
	int			 s, error = 0;
	if ((ifp = ifunit(altq->ifname)) == NULL)
		return (EINVAL);
	if (ifp->if_snd.altq_type != ALTQT_NONE)
		error = altq_enable(&ifp->if_snd);
	/* set tokenbucket regulator */
	if (error == 0 && ifp != NULL && ALTQ_IS_ENABLED(&ifp->if_snd)) {
		tb.rate = altq->ifbandwidth;
		tb.depth = altq->tbrsize;
		s = splnet();
		error = tbr_set(&ifp->if_snd, &tb);
		splx(s);
	}
	return (error);
}

int
npf_get_altqs(void *data)
{
    struct npfioc_altq	*paa = (struct npfioc_altq *)data;
    struct npf_altq		*altq;
    paa->nr = 0;
    TAILQ_FOREACH(altq, npf_altqs_active, entries)
        paa->nr++;
    paa->ticket = nticket_altqs_active;
	return 0 ;
}

#endif /* ALTQ */