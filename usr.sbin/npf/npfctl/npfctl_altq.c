/*-
 * Copyright (c) 2011-2020 The NetBSD Foundation, Inc.
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

#include <sys/types.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/queue.h>

#ifdef __NetBSD__
#include <sys/param.h>
#include <sys/mbuf.h>
#endif

#include <net/if.h>
#include <netinet/in.h>

#include <err.h>
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "npf.h"

#include <altq/altq.h>
#include <altq/altq_cbq.h>
#include <altq/altq_priq.h>
#include <altq/altq_hfsc.h>

#include "npfctl.h"

static int	eval_npfqueue_cbq(struct npf_altq *);
static int	cbq_compute_idletime(struct npf_altq *);
//static int	check_commit_cbq(int, int, struct npf_altq *);
//static int	print_cbq_opts(const struct npf_altq *);

static int	eval_npfqueue_priq(struct npf_altq *);
//static int	check_commit_priq(int, int, struct npf_altq *);
//static int	print_priq_opts(const struct npf_altq *);

static int	eval_npfqueue_hfsc(struct npf_altq *);
//static int	check_commit_hfsc(int, int, struct npf_altq *);
//static int	print_hfsc_opts(const struct npf_altq *,
//		    const struct node_queue_opt *);

//static void		 gsc_add_sc(struct gen_sc *, struct service_curve *);
//static int		 is_gsc_under_sc(struct gen_sc *,
//			     struct service_curve *);
//static void		 gsc_destroy(struct gen_sc *);
//static struct segment	*gsc_getentry(struct gen_sc *, double);
//static int		 gsc_add_seg(struct gen_sc *, double, double, double,
//			     double);
//static double		 sc_x2y(struct service_curve *, double);

//void		 print_hfsc_sc(const char *, u_int, u_int, u_int,
//		     const struct node_hfsc_sc *);

extern int npfctl_open_dev(const char *);

extern int altqsupport;
TAILQ_HEAD(altqs, npf_altq) altqs = TAILQ_HEAD_INITIALIZER(altqs);
#define is_sc_null(sc)	(((sc) == NULL) || ((sc)->m1 == 0 && (sc)->m2 == 0))

struct node_queue *queues = NULL;

LIST_HEAD(gen_sc, segment) rtsc, lssc;

#define FREE_LIST(T,r) \
	do { \
		T *p, *node = r; \
		while (node != NULL) { \
			p = node; \
			node = node->next; \
			free(p); \
		} \
	} while (0)

#define LOOP_THROUGH(T,n,r,C) \
	do { \
		T *n; \
		if (r == NULL) { \
			r = calloc(1, sizeof(T)); \
			if (r == NULL) \
				err(1, "LOOP: calloc"); \
			r->next = NULL; \
		} \
		n = r; \
		while (n != NULL) { \
			do { \
				C; \
			} while (0); \
			n = n->next; \
		} \
	} while (0)

int
npfctl_test_altqsupport(int dev)
{
	struct npfioc_altq pa;
	if (ioctl(dev, IOC_NPF_GET_ALTQS, &pa)) {
		if (errno == ENODEV) {
			fprintf(stderr, "No ALTQ support in kernel\n"
				"ALTQ related functions disabled\n");
			return (0);
		} else
		err(1, "IOC_GET_ALTQS");
	}
	return (1);
}

int
expand_altq(struct npf_altq *a, const char *ifname,
    struct node_queue *nqueues, struct node_queue_bw bwspec,
    struct node_queue_opt *opts)
{
	struct npf_altq		 pa, pb;
	char			 qname[NPF_QNAME_SIZE];
	struct node_queue	*n;
	struct node_queue_bw	 bw;
	int			 errs = 0;

	memcpy(&pa, a, sizeof(struct npf_altq));
	if (strlcpy(pa.ifname, ifname,
		sizeof(pa.ifname)) >= sizeof(pa.ifname))
		errx(1, "expand_altq: strlcpy");

	if (ifname == NULL) {
		yyerror("altq on ! <interface> is not supported");
		errs++;
	} else {
		if (eval_npfaltq(&pa, &bwspec, opts))
			errs++;
		else
			if (npfctl_add_altq(&pa))
				errs++;

		if (pa.scheduler == ALTQT_CBQ ||
			pa.scheduler == ALTQT_HFSC) {
			/* now create a root queue */
			memset(&pb, 0, sizeof(struct npf_altq));
			if (strlcpy(qname, "root_", sizeof(qname)) >=
				sizeof(qname))
				errx(1, "expand_altq: strlcpy");
			if (strlcat(qname, ifname,
				sizeof(qname)) >= sizeof(qname))
				errx(1, "expand_altq: strlcat");
			if (strlcpy(pb.qname, qname,
				sizeof(pb.qname)) >= sizeof(pb.qname))
				errx(1, "expand_altq: strlcpy");
			if (strlcpy(pb.ifname, ifname,
				sizeof(pb.ifname)) >= sizeof(pb.ifname))
				errx(1, "expand_altq: strlcpy");
			pb.qlimit = pa.qlimit;
			pb.scheduler = pa.scheduler;
			bw.bw_absolute = pa.ifbandwidth;
			bw.bw_percent = 0;
			if (eval_npfqueue(&pb, &bw, opts))
				errs++;
			else
				if (npfctl_add_altq(&pb))
					errs++;
		}

		LOOP_THROUGH(struct node_queue, queue, nqueues,
			n = calloc(1, sizeof(struct node_queue));
			if (n == NULL)
				err(1, "expand_altq: calloc");
			if (pa.scheduler == ALTQT_CBQ ||
				pa.scheduler == ALTQT_HFSC)
				if (strlcpy(n->parent, qname,
					sizeof(n->parent)) >=
					sizeof(n->parent))
					errx(1, "expand_altq: strlcpy");
			if (strlcpy(n->queue, queue->queue,
				sizeof(n->queue)) >= sizeof(n->queue))
				errx(1, "expand_altq: strlcpy");
			if (strlcpy(n->ifname, ifname,
				sizeof(n->ifname)) >= sizeof(n->ifname))
				errx(1, "expand_altq: strlcpy");
			n->scheduler = pa.scheduler;
			n->next = NULL;
			n->tail = n;
			if (queues == NULL)
				queues = n;
			else {
				queues->tail->next = n;
				queues->tail = n;
			}
		);
	}
	FREE_LIST(struct node_queue, nqueues);

	return (errs);
}

int
expand_queue(struct npf_altq *a, const char *ifname,
    struct node_queue *nqueues, struct node_queue_bw bwspec,
    struct node_queue_opt *opts)
{
	struct node_queue	*n, *nq;
	struct npf_altq		 pa;
	u_int8_t		 found = 0;
	u_int8_t		 errs = 0;
/*
	if ((pf->loadopt & PFCTL_FLAG_ALTQ) == 0) {
		FREE_LIST(struct node_queue, nqueues);
		return (0);
	}
	*/

	if (queues == NULL) {
		yyerror("queue %s has no parent", a->qname);
		FREE_LIST(struct node_queue, nqueues);
		return (1);
	}

	LOOP_THROUGH(struct node_queue, tqueue, queues,
		if (!strncmp(a->qname, tqueue->queue, NPF_QNAME_SIZE) &&
			(ifname == 0 ||
			(!strncmp(ifname, tqueue->ifname, IFNAMSIZ)) ||
			(strncmp(ifname, tqueue->ifname, IFNAMSIZ)))){
			/* found ourself in the child queues */
			found++;


			memcpy(&pa, a, sizeof(struct npf_altq));

			if (pa.scheduler != ALTQT_NONE &&
				pa.scheduler != tqueue->scheduler) {
				yyerror("exactly one scheduler type "
					"per interface allowed");
				errs++;
				goto out;
			}
			pa.scheduler = tqueue->scheduler;

			/* scheduler dependent error checking */
			switch (pa.scheduler) {
			case ALTQT_PRIQ:
				if (nqueues != NULL) {
					yyerror("priq queues cannot "
						"have child queues");
					errs++;
					goto out;
				}
				if (bwspec.bw_absolute > 0 ||
					bwspec.bw_percent < 100) {
					yyerror("priq doesn't take "
						"bandwidth");
					errs++;
					goto out;
				}
				break;
			default:
				break;
			}

			if (strlcpy(pa.ifname, tqueue->ifname,
				sizeof(pa.ifname)) >= sizeof(pa.ifname))
				errx(1, "expand_queue: strlcpy");
			if (strlcpy(pa.parent, tqueue->parent,
				sizeof(pa.parent)) >= sizeof(pa.parent))
				errx(1, "expand_queue: strlcpy");

			if (eval_npfqueue(&pa, &bwspec, opts))
				errs++;
			else
				if (npfctl_add_altq(&pa))
					errs++;

			for (nq = nqueues; nq != NULL; nq = nq->next) {
				if (!strcmp(a->qname, nq->queue)) {
					yyerror("queue cannot have "
						"itself as child");
					errs++;
					continue;
				}
				n = calloc(1,
					sizeof(struct node_queue));
				if (n == NULL)
					err(1, "expand_queue: calloc");
				if (strlcpy(n->parent, a->qname,
					sizeof(n->parent)) >=
					sizeof(n->parent))
					errx(1, "expand_queue strlcpy");
				if (strlcpy(n->queue, nq->queue,
					sizeof(n->queue)) >=
					sizeof(n->queue))
					errx(1, "expand_queue strlcpy");
				if (strlcpy(n->ifname, tqueue->ifname,
					sizeof(n->ifname)) >=
					sizeof(n->ifname))
					errx(1, "expand_queue strlcpy");
				n->scheduler = tqueue->scheduler;
				n->next = NULL;
				n->tail = n;
				if (queues == NULL)
					queues = n;
				else {
					queues->tail->next = n;
					queues->tail = n;
				}
			}
/*
			if ((pf->opts & PF_OPT_VERBOSE) && (
				(found == 1 && interface->ifname[0] == 0) ||
				(found > 0 && interface->ifname[0] != 0))) {
				print_queue(&pf->paltq->altq, 0,
					&bwspec, interface->ifname[0] != 0,
					opts);
				if (nqueues && nqueues->tail) {
					printf("{ ");
					LOOP_THROUGH(struct node_queue,
						queue, nqueues,
						printf("%s ",
							queue->queue);
					);
					printf("}");
				}
				printf("\n");
			}
*/
		}
	);

out:
	FREE_LIST(struct node_queue, nqueues);

	if (!found) {
		yyerror("queue %s has no parent", a->qname);
		errs++;
	}

	if (errs)
		return (1);
	else
		return (0);
}

/*
 * eval_npfaltq computes the discipline parameters.
 */
int
eval_npfaltq(struct npf_altq *pa, struct node_queue_bw *bw,
    struct node_queue_opt *opts)
{
	u_int	rate, size, errors = 0;

	if (bw->bw_absolute > 0)
		pa->ifbandwidth = bw->bw_absolute;
	else
		if ((rate = get_ifspeed(pa->ifname)) == 0) {
			fprintf(stderr, "interface %s does not know its bandwidth, "
			    "please specify an absolute bandwidth\n",
			    pa->ifname);
			errors++;
		} else if ((pa->ifbandwidth = npf_eval_bwspec(bw, rate)) == 0)
			pa->ifbandwidth = rate;

	errors += npf_eval_queue_opts(pa, opts, pa->ifbandwidth);

	/* if tbrsize is not specified, use heuristics */
	if (pa->tbrsize == 0) {
		rate = pa->ifbandwidth;
		if (rate <= 1 * 1000 * 1000)
			size = 1;
		else if (rate <= 10 * 1000 * 1000)
			size = 4;
		else if (rate <= 200 * 1000 * 1000)
			size = 8;
		else
			size = 24;
		size = size * get_ifmtu(pa->ifname);
		if (size > 0xffff)
			size = 0xffff;
		pa->tbrsize = size;
	}
	return (errors);
}

int
npf_eval_queue_opts(struct npf_altq *pa, struct node_queue_opt *opts,
    u_int32_t ref_bw)
{
	int	errors = 0;

	switch (pa->scheduler) {
	case ALTQT_CBQ:
		pa->pq_u.cbq_opts = opts->data.cbq_opts;
		break;
	case ALTQT_PRIQ:
		pa->pq_u.priq_opts = opts->data.priq_opts;
		break;
	case ALTQT_HFSC:
		pa->pq_u.hfsc_opts.flags = opts->data.hfsc_opts.flags;
		if (opts->data.hfsc_opts.linkshare.used) {
			pa->pq_u.hfsc_opts.lssc_m1 =
			    npf_eval_bwspec(&opts->data.hfsc_opts.linkshare.m1,
			    ref_bw);
			pa->pq_u.hfsc_opts.lssc_m2 =
			    npf_eval_bwspec(&opts->data.hfsc_opts.linkshare.m2,
			    ref_bw);
			pa->pq_u.hfsc_opts.lssc_d =
			    opts->data.hfsc_opts.linkshare.d;
		}
		if (opts->data.hfsc_opts.realtime.used) {
			pa->pq_u.hfsc_opts.rtsc_m1 =
			    npf_eval_bwspec(&opts->data.hfsc_opts.realtime.m1,
			    ref_bw);
			pa->pq_u.hfsc_opts.rtsc_m2 =
			    npf_eval_bwspec(&opts->data.hfsc_opts.realtime.m2,
			    ref_bw);
			pa->pq_u.hfsc_opts.rtsc_d =
			    opts->data.hfsc_opts.realtime.d;
		}
		if (opts->data.hfsc_opts.upperlimit.used) {
			pa->pq_u.hfsc_opts.ulsc_m1 =
			    npf_eval_bwspec(&opts->data.hfsc_opts.upperlimit.m1,
			    ref_bw);
			pa->pq_u.hfsc_opts.ulsc_m2 =
			    npf_eval_bwspec(&opts->data.hfsc_opts.upperlimit.m2,
			    ref_bw);
			pa->pq_u.hfsc_opts.ulsc_d =
			    opts->data.hfsc_opts.upperlimit.d;
		}
		break;
	default:
		warnx("eval_queue_opts: unknown scheduler type %u",
		    opts->qtype);
		errors++;
		break;
	}

	return (errors);
}

int
npfctl_add_altq(struct npf_altq *a)
{
	struct npfioc_altq *npaltq;
	int fd = npfctl_open_dev(NPF_DEV_PATH);
	if (altqsupport ) {
		memcpy(&npaltq->altq, a, sizeof(struct npfioc_altq));
		if (ioctl(fd, IOC_NPF_ADD_ALTQ, npaltq)) {
			if (errno == ENXIO)
				errx(1, "qtype not configured");
			else if (errno == ENODEV)
				errx(1, "%s: driver does not support "
					"altq", a->ifname);
			else
				err(1, "NPFADDALTQ");
		}
	}
		npfaltq_store(&npaltq->altq);
	return (0);
}

void
npfaltq_store(struct npf_altq *a)
{
	struct npf_altq	*altq;

	if ((altq = malloc(sizeof(*altq))) == NULL)
		err(1, "malloc");
	memcpy(altq, a, sizeof(struct npf_altq));
	TAILQ_INSERT_TAIL(&altqs, altq, entries);
}

u_int32_t
npf_eval_bwspec(struct node_queue_bw *bw, u_int32_t ref_bw)
{
	if (bw->bw_absolute > 0)
		return (bw->bw_absolute);

	if (bw->bw_percent > 0)
		return (ref_bw / 100 * bw->bw_percent);

	return (0);
}

u_int32_t
get_ifspeed(char *ifname)
{
	int			 s;
	struct ifdatareq	 ifdr;
	struct if_data		*ifrdat;

	if ((s = socket(AF_INET, SOCK_DGRAM, 0)) < 0)
		err(1, "getifspeed: socket");
	memset(&ifdr, 0, sizeof(ifdr));
	if (strlcpy(ifdr.ifdr_name, ifname, sizeof(ifdr.ifdr_name)) >=
	    sizeof(ifdr.ifdr_name))
		errx(1, "getifspeed: strlcpy");
	if (ioctl(s, SIOCGIFDATA, &ifdr) == -1)
		err(1, "getifspeed: SIOCGIFDATA");
	ifrdat = &ifdr.ifdr_data;
	if (close(s) == -1)
		err(1, "getifspeed: close");
	return ((u_int32_t)ifrdat->ifi_baudrate);
}

u_long
get_ifmtu(char *ifname)
{
	int		s;
	struct ifreq	ifr;

	if ((s = socket(AF_INET, SOCK_DGRAM, 0)) < 0)
		err(1, "socket");
	bzero(&ifr, sizeof(ifr));
	if (strlcpy(ifr.ifr_name, ifname, sizeof(ifr.ifr_name)) >=
	    sizeof(ifr.ifr_name))
		errx(1, "getifmtu: strlcpy");
	if (ioctl(s, SIOCGIFMTU, (caddr_t)&ifr) == -1)
		err(1, "SIOCGIFMTU");
	if (close(s) == -1)
		err(1, "close");
	if (ifr.ifr_mtu > 0)
		return (ifr.ifr_mtu);
	else {
		warnx("could not get mtu for %s, assuming 1500", ifname);
		return (1500);
	}
}

/*
 * eval_npfqueue computes the queue parameters.
 */
int
eval_npfqueue(struct npf_altq *pa, struct node_queue_bw *bw,
    struct node_queue_opt *opts)
{
	/* should be merged with expand_queue */
	struct npf_altq	*if_pa, *parent, *altq;
	u_int32_t	 bwsum;
	int		 error = 0;

	/* find the corresponding interface and copy fields used by queues */
	if ((if_pa = npfaltq_lookup(pa->ifname)) == NULL) {
		fprintf(stderr, "altq not defined on %s\n", pa->ifname);
		return (1);
	}
	pa->scheduler = if_pa->scheduler;
	pa->ifbandwidth = if_pa->ifbandwidth;

	if (qname_to_npfaltq(pa->qname, pa->ifname) != NULL) {
		fprintf(stderr, "queue %s already exists on interface %s\n",
		    pa->qname, pa->ifname);
		return (1);
	}
	pa->qid = qname_to_qid(pa->qname);

	parent = NULL;
	if (pa->parent[0] != 0) {
		parent = qname_to_npfaltq(pa->parent, pa->ifname);
		if (parent == NULL) {
			fprintf(stderr, "parent %s not found for %s\n",
			    pa->parent, pa->qname);
			return (1);
		}
		pa->parent_qid = parent->qid;
	}
	if (pa->qlimit == 0)
		pa->qlimit = DEFAULT_QLIMIT;

	if (pa->scheduler == ALTQT_CBQ || pa->scheduler == ALTQT_HFSC) {
		pa->bandwidth = npf_eval_bwspec(bw,
		    parent == NULL ? 0 : parent->bandwidth);

		if (pa->bandwidth > pa->ifbandwidth) {
			fprintf(stderr, "bandwidth for %s higher than "
			    "interface\n", pa->qname);
			return (1);
		}
		/* check the sum of the child bandwidth is under parent's */
		if (parent != NULL) {
			if (pa->bandwidth > parent->bandwidth) {
				warnx("bandwidth for %s higher than parent",
				    pa->qname);
				return (1);
			}
			bwsum = 0;
			TAILQ_FOREACH(altq, &altqs, entries) {
				if (strncmp(altq->ifname, pa->ifname,
				    IFNAMSIZ) == 0 &&
				    altq->qname[0] != 0 &&
				    strncmp(altq->parent, pa->parent,
				    NPF_QNAME_SIZE) == 0)
					bwsum += altq->bandwidth;
			}
			bwsum += pa->bandwidth;
			if (bwsum > parent->bandwidth) {
				warnx("the sum of the child bandwidth higher"
				    " than parent \"%s\"", parent->qname);
			}
		}
	}

	if (npf_eval_queue_opts(pa, opts, parent == NULL? 0 : parent->bandwidth))
		return (1);

	switch (pa->scheduler) {
	case ALTQT_CBQ:
		error = eval_npfqueue_cbq(pa);
		break;
	case ALTQT_PRIQ:
		error = eval_npfqueue_priq(pa);
		break;
	case ALTQT_HFSC:
		error = eval_npfqueue_hfsc(pa);
		break;
	default:
		break;
	}
	return (error);
}

struct npf_altq *
npfaltq_lookup(const char *ifname)
{
	struct npf_altq	*altq;

	TAILQ_FOREACH(altq, &altqs, entries) {
		if (strncmp(ifname, altq->ifname, IFNAMSIZ) == 0 &&
		    altq->qname[0] == 0)
			return (altq);
	}
	return (NULL);
}

/*
 * CBQ support functions
 */
#define	RM_FILTER_GAIN	5	/* log2 of gain, e.g., 5 => 31/32 */
#define	RM_NS_PER_SEC	(1000000000)

static int
eval_npfqueue_cbq(struct npf_altq *pa)
{
	struct npf_cbq_opts	*opts;
	u_int		 ifmtu;

	if (pa->priority >= CBQ_MAXPRI) {
		warnx("priority out of range: max %d", CBQ_MAXPRI - 1);
		return (-1);
	}

	ifmtu = get_ifmtu(pa->ifname);
	opts = &pa->pq_u.cbq_opts;

	if (opts->pktsize == 0) {	/* use default */
		opts->pktsize = ifmtu;
		if (opts->pktsize > MCLBYTES)	/* do what TCP does */
			opts->pktsize &= ~MCLBYTES;
	} else if (opts->pktsize > ifmtu)
		opts->pktsize = ifmtu;
	if (opts->maxpktsize == 0)	/* use default */
		opts->maxpktsize = ifmtu;
	else if (opts->maxpktsize > ifmtu)
		opts->pktsize = ifmtu;

	if (opts->pktsize > opts->maxpktsize)
		opts->pktsize = opts->maxpktsize;

	if (pa->parent[0] == 0)
		opts->flags |= (CBQCLF_ROOTCLASS | CBQCLF_WRR);

	cbq_compute_idletime(pa);
	return (0);
}

/*
 * compute ns_per_byte, maxidle, minidle, and offtime
 */
static int
cbq_compute_idletime(struct npf_altq *pa)
{
	struct npf_cbq_opts	*opts;
	double		 maxidle_s, maxidle, minidle;
	double		 offtime, nsPerByte, ifnsPerByte, ptime, cptime;
	double		 z, g, f, gton, gtom;
	u_int		 minburst, maxburst;

	opts = &pa->pq_u.cbq_opts;
	ifnsPerByte = (1.0 / (double)pa->ifbandwidth) * RM_NS_PER_SEC * 8;
	minburst = opts->minburst;
	maxburst = opts->maxburst;

	if (pa->bandwidth == 0)
		f = 0.0001;	/* small enough? */
	else
		f = ((double) pa->bandwidth / (double) pa->ifbandwidth);

	nsPerByte = ifnsPerByte / f;
	ptime = (double)opts->pktsize * ifnsPerByte;
	cptime = ptime * (1.0 - f) / f;

	if (nsPerByte * (double)opts->maxpktsize > (double)INT_MAX) {
		/*
		 * this causes integer overflow in kernel!
		 * (bandwidth < 6Kbps when max_pkt_size=1500)
		 */
		if (pa->bandwidth != 0) {
			warnx("queue bandwidth must be larger than %s",
			    rate2str(ifnsPerByte * (double)opts->maxpktsize /
			    (double)INT_MAX * (double)pa->ifbandwidth));
			fprintf(stderr, "cbq: queue %s is too slow!\n",
			    pa->qname);
		}
		nsPerByte = (double)(INT_MAX / opts->maxpktsize);
	}

	if (maxburst == 0) {  /* use default */
		if (cptime > 10.0 * 1000000)
			maxburst = 4;
		else
			maxburst = 16;
	}
	if (minburst == 0)  /* use default */
		minburst = 2;
	if (minburst > maxburst)
		minburst = maxburst;

	z = (double)(1 << RM_FILTER_GAIN);
	g = (1.0 - 1.0 / z);
	gton = pow(g, (double)maxburst);
	gtom = pow(g, (double)(minburst-1));
	maxidle = ((1.0 / f - 1.0) * ((1.0 - gton) / gton));
	maxidle_s = (1.0 - g);
	if (maxidle > maxidle_s)
		maxidle = ptime * maxidle;
	else
		maxidle = ptime * maxidle_s;
	offtime = cptime * (1.0 + 1.0/(1.0 - g) * (1.0 - gtom) / gtom);
	minidle = -((double)opts->maxpktsize * (double)nsPerByte);

	/* scale parameters */
	maxidle = ((maxidle * 8.0) / nsPerByte) *
	    pow(2.0, (double)RM_FILTER_GAIN);
	offtime = (offtime * 8.0) / nsPerByte *
	    pow(2.0, (double)RM_FILTER_GAIN);
	minidle = ((minidle * 8.0) / nsPerByte) *
	    pow(2.0, (double)RM_FILTER_GAIN);

	maxidle = maxidle / 1000.0;
	offtime = offtime / 1000.0;
	minidle = minidle / 1000.0;

	opts->minburst = minburst;
	opts->maxburst = maxburst;
	opts->ns_per_byte = (u_int)nsPerByte;
	opts->maxidle = (u_int)fabs(maxidle);
	opts->minidle = (int)minidle;
	opts->offtime = (u_int)fabs(offtime);

	return (0);
}

/*
 * PRIQ support functions
 */
static int
eval_npfqueue_priq(struct npf_altq *pa)
{
	struct npf_altq	*altq;

	if (pa->priority >= PRIQ_MAXPRI) {
		warnx("priority out of range: max %d", PRIQ_MAXPRI - 1);
		return (-1);
	}
	/* the priority should be unique for the interface */
	TAILQ_FOREACH(altq, &altqs, entries) {
		if (strncmp(altq->ifname, pa->ifname, IFNAMSIZ) == 0 &&
		    altq->qname[0] != 0 && altq->priority == pa->priority) {
			warnx("%s and %s have the same priority",
			    altq->qname, pa->qname);
			return (-1);
		}
	}

	return (0);
}

/*
 * HFSC support functions
 */
static int
eval_npfqueue_hfsc(struct npf_altq *pa)
{
	struct npf_altq		*altq, *parent;
	struct npf_hfsc_opts	*opts;
	struct service_curve	 sc;

	opts = &pa->pq_u.hfsc_opts;

	if (pa->parent[0] == 0) {
		/* root queue */
		opts->lssc_m1 = pa->ifbandwidth;
		opts->lssc_m2 = pa->ifbandwidth;
		opts->lssc_d = 0;
		return (0);
	}

	LIST_INIT(&rtsc);
	LIST_INIT(&lssc);

	/* if link_share is not specified, use bandwidth */
	if (opts->lssc_m2 == 0)
		opts->lssc_m2 = pa->bandwidth;

	if ((opts->rtsc_m1 > 0 && opts->rtsc_m2 == 0) ||
	    (opts->lssc_m1 > 0 && opts->lssc_m2 == 0) ||
	    (opts->ulsc_m1 > 0 && opts->ulsc_m2 == 0)) {
		warnx("m2 is zero for %s", pa->qname);
		return (-1);
	}

	if ((opts->rtsc_m1 < opts->rtsc_m2 && opts->rtsc_m1 != 0) ||
	    (opts->lssc_m1 < opts->lssc_m2 && opts->lssc_m1 != 0) ||
	    (opts->ulsc_m1 < opts->ulsc_m2 && opts->ulsc_m1 != 0)) {
		warnx("m1 must be zero for convex curve: %s", pa->qname);
		return (-1);
	}

	/*
	 * admission control:
	 * for the real-time service curve, the sum of the service curves
	 * should not exceed 80% of the interface bandwidth.  20% is reserved
	 * not to over-commit the actual interface bandwidth.
	 * for the linkshare service curve, the sum of the child service
	 * curve should not exceed the parent service curve.
	 * for the upper-limit service curve, the assigned bandwidth should
	 * be smaller than the interface bandwidth, and the upper-limit should
	 * be larger than the real-time service curve when both are defined.
	 */
	parent = qname_to_npfaltq(pa->parent, pa->ifname);
	if (parent == NULL)
		errx(1, "parent %s not found for %s", pa->parent, pa->qname);

	TAILQ_FOREACH(altq, &altqs, entries) {
		if (strncmp(altq->ifname, pa->ifname, IFNAMSIZ) != 0)
			continue;
		if (altq->qname[0] == 0)  /* this is for interface */
			continue;

		/* if the class has a real-time service curve, add it. */
		if (opts->rtsc_m2 != 0 && altq->pq_u.hfsc_opts.rtsc_m2 != 0) {
			sc.m1 = altq->pq_u.hfsc_opts.rtsc_m1;
			sc.d = altq->pq_u.hfsc_opts.rtsc_d;
			sc.m2 = altq->pq_u.hfsc_opts.rtsc_m2;
			gsc_add_sc(&rtsc, &sc);
		}

		if (strncmp(altq->parent, pa->parent, NPF_QNAME_SIZE) != 0)
			continue;

		/* if the class has a linkshare service curve, add it. */
		if (opts->lssc_m2 != 0 && altq->pq_u.hfsc_opts.lssc_m2 != 0) {
			sc.m1 = altq->pq_u.hfsc_opts.lssc_m1;
			sc.d = altq->pq_u.hfsc_opts.lssc_d;
			sc.m2 = altq->pq_u.hfsc_opts.lssc_m2;
			gsc_add_sc(&lssc, &sc);
		}
	}

	/* check the real-time service curve.  reserve 20% of interface bw */
	if (opts->rtsc_m2 != 0) {
		/* add this queue to the sum */
		sc.m1 = opts->rtsc_m1;
		sc.d = opts->rtsc_d;
		sc.m2 = opts->rtsc_m2;
		gsc_add_sc(&rtsc, &sc);
		/* compare the sum with 80% of the interface */
		sc.m1 = 0;
		sc.d = 0;
		sc.m2 = pa->ifbandwidth / 100 * 80;
		if (!is_gsc_under_sc(&rtsc, &sc)) {
			warnx("real-time sc exceeds 80%% of the interface "
			    "bandwidth (%s)", rate2str((double)sc.m2));
			goto err_ret;
		}
	}

	/* check the linkshare service curve. */
	if (opts->lssc_m2 != 0) {
		/* add this queue to the child sum */
		sc.m1 = opts->lssc_m1;
		sc.d = opts->lssc_d;
		sc.m2 = opts->lssc_m2;
		gsc_add_sc(&lssc, &sc);
		/* compare the sum of the children with parent's sc */
		sc.m1 = parent->pq_u.hfsc_opts.lssc_m1;
		sc.d = parent->pq_u.hfsc_opts.lssc_d;
		sc.m2 = parent->pq_u.hfsc_opts.lssc_m2;
		if (!is_gsc_under_sc(&lssc, &sc)) {
			warnx("linkshare sc exceeds parent's sc");
			goto err_ret;
		}
	}

	/* check the upper-limit service curve. */
	if (opts->ulsc_m2 != 0) {
		if (opts->ulsc_m1 > pa->ifbandwidth ||
		    opts->ulsc_m2 > pa->ifbandwidth) {
			warnx("upper-limit larger than interface bandwidth");
			goto err_ret;
		}
		if (opts->rtsc_m2 != 0 && opts->rtsc_m2 > opts->ulsc_m2) {
			warnx("upper-limit sc smaller than real-time sc");
			goto err_ret;
		}
	}

	gsc_destroy(&rtsc);
	gsc_destroy(&lssc);

	return (0);

err_ret:
	gsc_destroy(&rtsc);
	gsc_destroy(&lssc);
	return (-1);
}
