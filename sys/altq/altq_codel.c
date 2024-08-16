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

#ifdef _KERNEL_OPT
#include "opt_altq.h"
#include "opt_inet.h"
#include "pf.h"
#endif

#ifdef ALTQ_CODEL	/* cbq is enabled by ALTQ_CBQ option in opt_altq.h */

#include <sys/param.h>
#include <sys/malloc.h>
#include <sys/mbuf.h>
#include <sys/socket.h>
#include <sys/systm.h>
#include <sys/proc.h>
#include <sys/errno.h>
#include <sys/time.h>
#include <sys/kauth.h>
#include <net/if.h>
#include <netinet/in.h>

#include <altq/if_altq.h>
#include <altq/altq.h>
#include <altq/altq_codel.h>

#ifdef ALTQ3_COMPAT
#include <altq/altq_conf.h>
#endif

/* codel interface state list to keep all codel states allocated*/
static struct codel_if *codel_list = NULL;

#define CODEL_LIMIT 1000
#define DEF_TARG	5
#define DEF_INT		100
#define DEF_ECN		0

static int default_interval = DEF_INT;
static int default_target =   DEF_TARG;
static int default_ecn = DEF_ECN;

static int		 codel_should_drop(struct codel *, class_queue_t *,
			    struct mbuf *, u_int64_t);
static void		 codel_Newton_step(struct codel_vars *);
static u_int64_t	 codel_control_law(u_int64_t t, u_int64_t, u_int32_t);

#define	codel_time_after(a, b)		((int64_t)(a) - (int64_t)(b) > 0)
#define	codel_time_after_eq(a, b)	((int64_t)(a) - (int64_t)(b) >= 0)
#define	codel_time_before(a, b)		((int64_t)(a) - (int64_t)(b) < 0)
#define	codel_time_before_eq(a, b)	((int64_t)(a) - (int64_t)(b) <= 0)

#ifdef ALTQ3_COMPAT
static int codel_request(struct ifaltq *, int, void *);

static int codel_enqueue(struct ifaltq *, struct mbuf *);
static struct mbuf *codel_dequeue(struct ifaltq *, int);
static int codel_detach(struct codel_if *);
static void codel_purgeq(struct codel_if *);
#endif /* ALTQ3_COMPAT */

struct codel *
codel_alloc(int target, int interval, int ecn)
{
	struct codel *c;

	c = malloc(sizeof(*c), M_DEVBUF, M_NOWAIT | M_ZERO);
	if (c != NULL) {
		if (target == 0)
			target = default_target;
		c->params.target = machclk_freq * target / 1000;

		if (interval == 0)
			interval = default_interval;
		c->params.interval = machclk_freq * interval / 1000;
		c->params.ecn = ecn;
		c->stats.maxpacket = 256;
	}

	return (c);
}

void
codel_destroy(struct codel *c)
{

	free(c, M_DEVBUF);
}

#define	MTAG_CODEL	1438031249
int
codel_addq(struct codel *c, class_queue_t *q, struct mbuf *m)
{
	struct m_tag *mtag;
	uint64_t *enqueue_time;

	if (qlen(q) < qlimit(q)) {
		mtag = m_tag_find(m, MTAG_CODEL);
		if (mtag == NULL) {
			mtag = m_tag_get(MTAG_CODEL, sizeof(uint64_t),
			    M_NOWAIT);
			if (mtag != NULL)
				m_tag_prepend(m, mtag);
		}
		if (mtag == NULL) {
			m_freem(m);
			return -1;
		}
		enqueue_time = (uint64_t *)(mtag + 1);
		*enqueue_time = read_machclk();
		_addq(q, m);
		return 0;
	}
	c->drop_overlimit++;
	m_freem(m);

	return -1;
}

static int
codel_should_drop(struct codel *c, class_queue_t *q, struct mbuf *m,
    u_int64_t now)
{
	struct m_tag *mtag;
	uint64_t *enqueue_time;

	if (m == NULL) {
		c->vars.first_above_time = 0;
		return (0);
	}

	mtag = m_tag_find(m, MTAG_CODEL);
	if (mtag == NULL) {
		/* Only one warning per second. */
		if (ppsratecheck(&c->last_log, &c->last_pps, 1))
			printf("%s: could not found the packet mtag!\n",
			    __func__);
		c->vars.first_above_time = 0;
		return (0);
	}
	enqueue_time = (uint64_t *)(mtag + 1);
	c->vars.ldelay = now - *enqueue_time;
	c->stats.maxpacket = MAX(c->stats.maxpacket, m_pktlen(m));

	if (codel_time_before(c->vars.ldelay, c->params.target) ||
	    qsize(q) <= c->stats.maxpacket) {
		/* went below - stay below for at least interval */
		c->vars.first_above_time = 0;
		return (0);
	}
	if (c->vars.first_above_time == 0) {
		/* just went above from below. If we stay above
		 * for at least interval we'll say it's ok to drop
		 */
		c->vars.first_above_time = now + c->params.interval;
		return (0);
	}
	if (codel_time_after(now, c->vars.first_above_time))
		return (1);

	return (0);
}

/*
 * Run a Newton method step:
 * new_invsqrt = (invsqrt / 2) * (3 - count * invsqrt^2)
 *
 * Here, invsqrt is a fixed point number (< 1.0), 32bit mantissa, aka Q0.32
 */
static void
codel_Newton_step(struct codel_vars *vars)
{
	uint32_t invsqrt, invsqrt2;
	uint64_t val;

/* sizeof_in_bits(rec_inv_sqrt) */
#define	REC_INV_SQRT_BITS (8 * sizeof(u_int16_t))
/* needed shift to get a Q0.32 number from rec_inv_sqrt */
#define	REC_INV_SQRT_SHIFT (32 - REC_INV_SQRT_BITS)

	invsqrt = ((u_int32_t)vars->rec_inv_sqrt) << REC_INV_SQRT_SHIFT;
	invsqrt2 = ((u_int64_t)invsqrt * invsqrt) >> 32;
	val = (3LL << 32) - ((u_int64_t)vars->count * invsqrt2);
	val >>= 2; /* avoid overflow in following multiply */
	val = (val * invsqrt) >> (32 - 2 + 1);

	vars->rec_inv_sqrt = val >> REC_INV_SQRT_SHIFT;
}

static u_int64_t
codel_control_law(u_int64_t t, u_int64_t interval, u_int32_t rec_inv_sqrt)
{

	return (t + (u_int32_t)(((u_int64_t)interval *
	    (rec_inv_sqrt << REC_INV_SQRT_SHIFT)) >> 32));
}

struct mbuf *
codel_getq(struct codel *c, class_queue_t *q)
{
	struct mbuf	*m;
	u_int64_t	 now;
	int		 drop;

	if ((m = _getq(q)) == NULL) {
		c->vars.dropping = 0;
		return (m);
	}

	now = read_machclk();
	drop = codel_should_drop(c, q, m, now);
	if (c->vars.dropping) {
		if (!drop) {
			/* sojourn time below target - leave dropping state */
			c->vars.dropping = 0;
		} else if (codel_time_after_eq(now, c->vars.drop_next)) {
			/* It's time for the next drop. Drop the current
			 * packet and dequeue the next. The dequeue might
			 * take us out of dropping state.
			 * If not, schedule the next drop.
			 * A large backlog might result in drop rates so high
			 * that the next drop should happen now,
			 * hence the while loop.
			 */
			while (c->vars.dropping &&
			    codel_time_after_eq(now, c->vars.drop_next)) {
				c->vars.count++; /* don't care of possible wrap
						  * since there is no more
						  * divide */
				codel_Newton_step(&c->vars);
				/* TODO ECN */
				PKTCNTR_ADD(&c->stats.cl_dropcnt, m_pktlen(m));
				m_freem(m);
				m = _getq(q);
				if (!codel_should_drop(c, q, m, now))
					/* leave dropping state */
					c->vars.dropping = 0;
				else
					/* and schedule the next drop */
					c->vars.drop_next =
					    codel_control_law(c->vars.drop_next,
						c->params.interval,
						c->vars.rec_inv_sqrt);
			}
		}
	} else if (drop) {
		/* TODO ECN */
		PKTCNTR_ADD(&c->stats.cl_dropcnt, m_pktlen(m));
		m_freem(m);

		m = _getq(q);
		drop = codel_should_drop(c, q, m, now);

		c->vars.dropping = 1;
		/* if min went above target close to when we last went below it
		 * assume that the drop rate that controlled the queue on the
		 * last cycle is a good starting point to control it now.
		 */
		if (codel_time_before(now - c->vars.drop_next,
		    16 * c->params.interval)) {
			c->vars.count = (c->vars.count - c->vars.lastcount) | 1;
			/* we dont care if rec_inv_sqrt approximation
			 * is not very precise :
			 * Next Newton steps will correct it quadratically.
			 */
			codel_Newton_step(&c->vars);
		} else {
			c->vars.count = 1;
			c->vars.rec_inv_sqrt = ~0U >> REC_INV_SQRT_SHIFT;
		}
		c->vars.lastcount = c->vars.count;
		c->vars.drop_next = codel_control_law(now, c->params.interval,
		    c->vars.rec_inv_sqrt);
	}

	return (m);
}

void
codel_getstats(struct codel *c, struct codel_stats *s)
{
	*s = c->stats;
}

#ifdef ALTQ3_COMPAT

/*
 * CoDel device interface
 */
altqdev_decl(codel);

int
codelopen(dev_t dev, int flag, int fmt,
		struct lwp *l)
{
	/*everything will be done the queuing scheme is attached */
	return 0;
}

int
codelclose(dev_t dev, int flag, int fmt,
		struct lwp *l)
{
	struct codel_if *cod;
	int err, error = 0;

	while ((cod = codel_list) != NULL) {
		err = codel_detach(cod);
		if (err != 0 && error == 0)
			error = err;
	}
	return error;
}

int
codelioctl(dev_t dev, ioctlcmd_t cmd, void *addr, int flag,
		struct lwp *l )
{
	struct codel_if *cod;
	struct codel_interface *ifacep;
	struct ifnet *ifp;
	int error = 0;

		/* check super-user privilege */
	switch (cmd) {
	case CODEL_GETSTATS:
		break;
	default:
		if ((error = kauth_authorize_network(l->l_cred,
		    KAUTH_NETWORK_ALTQ, KAUTH_REQ_NETWORK_ALTQ_CODEL, NULL,
		    NULL, NULL)) != 0)
			return (error);
		break;
	}

	switch (cmd) {

		case CODEL_ENABLE:
			ifacep = (struct codel_interface *)addr;
			if ((cod = altq_lookup(ifacep->codel_ifname, ALTQT_CODEL)) == NULL) {
				error = EBADF;
				break;
			}
			error = altq_enable(cod->cif_ifq);
			break;

		case CODEL_DISABLE:
			ifacep = (struct codel_interface *)addr;
			if ((cod = altq_lookup(ifacep->codel_ifname, ALTQT_CODEL)) == NULL) {
				error = EBADF;
				break;
			}
			error = altq_disable(cod->cif_ifq);
			break;

		case CODEL_IF_ATTACH:
			ifp = ifunit(((struct codel_interface *)addr)->codel_ifname);
			if (ifp == NULL) {
				error = ENXIO;
				break;
			}

			/* allocate and initialize codel_queue state */
			cod = malloc(sizeof(struct codel_if), M_DEVBUF, M_WAITOK|M_ZERO);
			if (cod == NULL) {
				error = ENOMEM;
				break;
			}

			cod->cl_q = malloc(sizeof(class_queue_t), M_DEVBUF,
				M_WAITOK|M_ZERO);
			if (cod->cl_q == NULL) {
				free(cod, M_DEVBUF);
				error = ENOMEM;
				break;
			}

			cod->codel = codel_alloc(0, 0, 0);
			if (cod->codel == NULL) {
				free(cod->cl_q, M_DEVBUF);
				free(cod, M_DEVBUF);
				error = ENOMEM;
				break;
			}

			cod->cif_ifq = &ifp->if_snd;
			qtail(cod->cl_q) = NULL;
			qlen(cod->cl_q) = 0;
			qlimit(cod->cl_q) = CODEL_LIMIT;
			qtype(cod->cl_q) = Q_CODEL;

			/*
			* set CODEL to this ifnet structure.
			*/
			error =  altq_attach(&ifp->if_snd, ALTQT_CODEL, cod,
	    		codel_enqueue, codel_dequeue, codel_request, NULL, NULL);

			if (error) {
				codel_destroy(cod->codel);
				free(cod->cl_q, M_DEVBUF);
				free(cod, M_DEVBUF);
				break;
			}

			/* add this state to the codel list */
			cod->cif_next = codel_list;
			codel_list = cod;
			break;

		case CODEL_IF_DETACH:
			ifacep = (struct codel_interface *)addr;
			if ((cod = altq_lookup(ifacep->codel_ifname, ALTQT_CODEL)) == NULL) {
				error = EBADF;
				break;
			}
			error = codel_detach(cod);
			break;

		case CODEL_GETSTATS:
			do {
				struct codel_ifstats *q_stats;
				struct codel *cd;

				q_stats = (struct codel_ifstats *)addr;
				if ((cod = altq_lookup(q_stats->iface.codel_ifname,
							ALTQT_CODEL)) == NULL) {
					error = EBADF;
					break;
				}

				q_stats->qlength 	= qlen(cod->cl_q);
				q_stats->qlimit   	= qlimit(cod->cl_q);

				cd = cod->codel;

				q_stats->stats.maxpacket = cd->stats.maxpacket;
				q_stats->stats.marked_packets = cd->stats.marked_packets;
				q_stats->stats.cl_xmitcnt = cd->stats.cl_xmitcnt;
				q_stats->stats.cl_dropcnt = cd->stats.cl_dropcnt;

				// convert your clock cycles back to time unit(milli sec) when fetching stats
				q_stats->params.target = cd->params.target * 1000 / machclk_freq;
				q_stats->params.interval = cd->params.interval * 1000 / machclk_freq;
				q_stats->params.ecn = cd->params.ecn;

			} while (/* CONSTCOND */ 0);
			break;
		case CODEL_CONFIG:
			do {
				struct codel_conf *cf;
				struct codel *new;
				int s, limit;

				cf = (struct codel_conf *)addr;
				if ((cod = altq_lookup(cf->iface.codel_ifname,
							ALTQT_CODEL)) == NULL) {
					error = EBADF;
					break;
				}
				new = codel_alloc(cf->target,
							cf->interval,
							cf->ecn);
				if (new == NULL) {
					error = ENOMEM;
					break;
				}

				s = splnet();
				codel_purgeq(cod);
				limit = cf->limit;
				qlimit(cod->cl_q) = limit;
				cf->limit = limit;

				codel_destroy(cod->codel);
				cod->codel = new;

				splx(s);
				cf->limit = limit;
				cf->target = cod->codel->params.target;
				cf->interval = cod->codel->params.interval;
				cf->ecn = cod->codel->params.ecn;
			} while (/* CONSTCOND */ 0);
			break;

		case CODEL_SETDEFAULTS:
			do {
				struct codel_params *cd;
				cd = (struct codel_params *)addr;

				default_target = cd->target;
				default_interval = cd->interval;
				default_ecn = cd->ecn;
			} while (/* CONSTCOND */ 0);
			break;

		default:
				error = EINVAL;
				break;
		}
		return error;
}

static int
codel_request(struct ifaltq *ifq, int req, void *arg)
{
	struct codel_if	*cif = (struct codel_if *)ifq->altq_disc;
	struct mbuf *m;

	switch (req) {
	case ALTRQ_PURGE:
		if (!ALTQ_IS_ENABLED(cif->cif_ifq))
			break;

		if (qempty(cif->cl_q))
			break;

		while ((m = _getq(cif->cl_q)) != NULL) {
			PKTCNTR_ADD(&cif->cl_stats.stats.cl_dropcnt, m_pktlen(m));
			m_freem(m);
			IFQ_DEC_LEN(cif->cif_ifq);
		}
		cif->cif_ifq->ifq_len = 0;
		break;
	}

	return 0;
}

static int
codel_enqueue(struct ifaltq *ifq, struct mbuf *m)
{

	struct codel_if *cif = (struct codel_if *) ifq->altq_disc;

	/* grab class set by classifier */
	if ((m->m_flags & M_PKTHDR) == 0) {
		/* should not happen */
		printf("altq: packet for %s does not have pkthdr\n",
		   ifq->altq_ifp->if_xname);
		m_freem(m);
		PKTCNTR_ADD(&cif->cl_stats.stats.cl_dropcnt, m_pktlen(m));
		return ENOBUFS;
	}

	if (codel_addq(cif->codel, cif->cl_q, m)) {
		PKTCNTR_ADD(&cif->cl_stats.stats.cl_dropcnt, m_pktlen(m));
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

	if (IFQ_IS_EMPTY(ifq))
		return NULL;

	if (op == ALTDQ_POLL)
		return (qhead(cif->cl_q));

	m = codel_getq(cif->codel, cif->cl_q);
	if (m != NULL) {
		IFQ_DEC_LEN(ifq);
		PKTCNTR_ADD(&cif->cl_stats.stats.cl_xmitcnt, m_pktlen(m));
		return m;
	}

	return NULL;
}

static int
codel_detach(struct codel_if *cif)
{
	struct codel_if *tmp;
	int error = 0;

	if (ALTQ_IS_ENABLED(cif->cif_ifq))
		altq_disable(cif->cif_ifq);

	if ((error = altq_detach(cif->cif_ifq)))
		return (error);

	if (codel_list == cif)
		codel_list = cif->cif_next;
	else {
		for (tmp = codel_list; tmp != NULL; tmp = tmp->cif_next)
			if (tmp->cif_next == cif) {
				tmp->cif_next = cif->cif_next;
				break;
			}
		if (tmp == NULL)
			printf("codel_detach: no state found in codel_list!\n");
	}
	codel_destroy(cif->codel);
	free(cif->cl_q, M_DEVBUF);
	free(cif, M_DEVBUF);
	return (error);
}

static void
codel_purgeq(struct codel_if *cif)
{
	_flushq(cif->cl_q);
	if (ALTQ_IS_ENABLED(cif->cif_ifq))
		cif->cif_ifq->ifq_len = 0;
}

#endif /* ALTQ3_COMPAT */

#endif /* ALTQ_CODEL */
