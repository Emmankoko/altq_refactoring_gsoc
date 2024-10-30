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


TAILQ_HEAD(altqs, npf_altq) altqs = TAILQ_HEAD_INITIALIZER(altqs);

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
			if (strlcat(qname, interface->ifname,
				sizeof(qname)) >= sizeof(qname))
				errx(1, "expand_altq: strlcat");
			if (strlcpy(pb.qname, qname,
				sizeof(pb.qname)) >= sizeof(pb.qname))
				errx(1, "expand_altq: strlcpy");
			if (strlcpy(pb.ifname, interface->ifname,
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
			if (strlcpy(n->ifname, interface->ifname,
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
		memcpy(npaltq->altq, a, sizeof(struct npfioc_altq));
			if (ioctl(fd, IOC_NPF_ADD_ALTQ, npaltq)) {
				if (errno == ENXIO)
					errx(1, "qtype not configured");
				else if (errno == ENODEV)
					errx(1, "%s: driver does not support "
					    "altq", a->ifname);
				else
					err(1, "DIOCADDALTQ");
			}
	}
		npfaltq_store(npaltq->altq);
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
