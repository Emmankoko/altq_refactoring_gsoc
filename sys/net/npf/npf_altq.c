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

#include <unistd.h>
#include <sys/param.h>
#include <sys/types.h>
#include <sys/queue.h>
#include <sys/tree.h>
#include <sys/rwlock.h>
#include <sys/pool.h>

#include "npf_altq.h"

struct pool		 npf_altq_pl;
#ifdef ALTQ
static int		 pf_altq_running;
#endif

/*
 * starting point of all ALTQ kernel routines
 */
int
npf_altq_start()
{
    struct npf_altq		*altq;

    /* enable all altq interfaces on active list */
    TAILQ_FOREACH(altq, pf_altqs_active, entries) {
        if (altq->qname[0] == 0) {
            error = npf_enable_altq(altq);
            if (error != 0)
                break;
        }
    }
    if (error == 0)
        npf_altq_running = 1;
    DPFPRINTF(PF_DEBUG_MISC, ("altq: started\n"));
}

int npf_altq_stop()
{
		struct npf_altq		*altq;

		/* disable all altq interfaces on active list */
		TAILQ_FOREACH(altq, npf_altqs_active, entries) {
			if (altq->qname[0] == 0) {
				error = npf_disable_altq(altq);
				if (error != 0)
					break;
			}
		}
		if (error == 0)
			npf_altq_running = 0;
		DPFPRINTF(PF_DEBUG_MISC, ("altq: stopped\n"));
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
npf_disable_altq(struct pf_altq *altq)
{
	struct ifnet		*ifp;
	struct tb_profile	 tb;
	int			 s, error;

	if ((ifp = ifunit(altq->ifname)) == NULL)
		return (EINVAL);

	/*
	 * when the discipline is no longer referenced, it was overridden
	 * by a new one.  if so, just return.
	 */
	if (altq->altq_disc != ifp->if_snd.altq_disc)
		return (0);

	error = altq_disable(&ifp->if_snd);

	if (error == 0) {
		/* clear tokenbucket regulator */
		tb.rate = 0;
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
    TAILQ_FOREACH(altq, pf_altqs_active, entries)
        paa->nr++;
    paa->ticket = ticket_altqs_active;
}

int
npf_add_altq(void *data)
{
	struct npfioc_altq	*paa = (struct npfioc_altq *)data;
	struct npf_altq		*altq, *a;

	if (paa->ticket != ticket_altqs_inactive) {
		error = EBUSY;
		break;
	}
	altq = pool_get(&npf_altq_pl, PR_NOWAIT);
	if (altq == NULL) {
		error = ENOMEM;
		break;
	}
	bcopy(&paa->altq, altq, sizeof(struct pf_altq));

	/*
		* if this is for a queue, find the discipline and
		* copy the necessary fields
		*/
	if (altq->qname[0] != 0) {
		if ((altq->qid = pf_qname2qid(altq->qname)) == 0) {
			error = EBUSY;
			pool_put(&pf_altq_pl, altq);
			break;
		}
		TAILQ_FOREACH(a, pf_altqs_inactive, entries) {
			if (strncmp(a->ifname, altq->ifname,
				IFNAMSIZ) == 0 && a->qname[0] == 0) {
				altq->altq_disc = a->altq_disc;
				break;
			}
		}
	}

	error = altq_add(altq);
	if (error) {
		pool_put(&npf_altq_pl, altq);
		break;
	}

	TAILQ_INSERT_TAIL(pf_altqs_inactive, altq, entries);
	bcopy(altq, &paa->altq, sizeof(struct pf_altq));
}

u_int32_t
pf_qname2qid(char *qname)
{
	return ((u_int32_t)tagname2tag(&pf_qids, qname));
}

void
pf_qid2qname(u_int32_t qid, char *p)
{
	tag2tagname(&pf_qids, (u_int16_t)qid, p);
}

void
pf_qid_unref(u_int32_t qid)
{
	tag_unref(&pf_qids, (u_int16_t)qid);
}

int
pf_begin_altq(u_int32_t *ticket)
{
	struct pf_altq	*altq;
	int		 error = 0;

	/* Purge the old altq list */
	while ((altq = TAILQ_FIRST(pf_altqs_inactive)) != NULL) {
		TAILQ_REMOVE(pf_altqs_inactive, altq, entries);
		if (altq->qname[0] == 0) {
			/* detach and destroy the discipline */
			error = altq_remove(altq);
		} else
			pf_qid_unref(altq->qid);
		pool_put(&pf_altq_pl, altq);
	}
	if (error)
		return (error);
	*ticket = ++ticket_altqs_inactive;
	altqs_inactive_open = 1;
	return (0);
}

int
pf_rollback_altq(u_int32_t ticket)
{
	struct pf_altq	*altq;
	int		 error = 0;

	if (!altqs_inactive_open || ticket != ticket_altqs_inactive)
		return (0);
	/* Purge the old altq list */
	while ((altq = TAILQ_FIRST(pf_altqs_inactive)) != NULL) {
		TAILQ_REMOVE(pf_altqs_inactive, altq, entries);
		if (altq->qname[0] == 0) {
			/* detach and destroy the discipline */
			error = altq_remove(altq);
		} else
			pf_qid_unref(altq->qid);
		pool_put(&pf_altq_pl, altq);
	}
	altqs_inactive_open = 0;
	return (error);
}

int
pf_commit_altq(u_int32_t ticket)
{
	struct pf_altqqueue	*old_altqs;
	struct pf_altq		*altq;
	int			 s, err, error = 0;

	if (!altqs_inactive_open || ticket != ticket_altqs_inactive)
		return (EBUSY);

	/* swap altqs, keep the old. */
	s = splsoftnet();
	old_altqs = pf_altqs_active;
	pf_altqs_active = pf_altqs_inactive;
	pf_altqs_inactive = old_altqs;
	ticket_altqs_active = ticket_altqs_inactive;

	/* Attach new disciplines */
	TAILQ_FOREACH(altq, pf_altqs_active, entries) {
		if (altq->qname[0] == 0) {
			/* attach the discipline */
			error = altq_pfattach(altq);
			if (error == 0 && pf_altq_running)
				error = pf_enable_altq(altq);
			if (error != 0) {
				splx(s);
				return (error);
			}
		}
	}

	/* Purge the old altq list */
	while ((altq = TAILQ_FIRST(pf_altqs_inactive)) != NULL) {
		TAILQ_REMOVE(pf_altqs_inactive, altq, entries);
		if (altq->qname[0] == 0) {
			/* detach and destroy the discipline */
			if (pf_altq_running)
				error = pf_disable_altq(altq);
			err = altq_pfdetach(altq);
			if (err != 0 && error == 0)
				error = err;
			err = altq_remove(altq);
			if (err != 0 && error == 0)
				error = err;
		} else
			pf_qid_unref(altq->qid);
		pool_put(&pf_altq_pl, altq);
	}
	splx(s);

	altqs_inactive_open = 0;
	return (error);
}