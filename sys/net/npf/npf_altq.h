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

#ifndef NPF_ALTQ_H_
#define NPF_ALTQ_H_

/*
 * starting point with integrating ALTQ with npf. functions and elements brought here will be dispersed accordingly
 * operation is derived from the understanding of how ALTQ is integrated in pf packet filter
 */

/*
 * npf altq facility
 */

struct cbq_opts {
	u_int		minburst;
	u_int		maxburst;
	u_int		pktsize;
	u_int		maxpktsize;
	u_int		ns_per_byte;
	u_int		maxidle;
	int		minidle;
	u_int		offtime;
	int		flags;
};

struct priq_opts {
	int		flags;
};

struct hfsc_opts {
	/* real-time service curve */
	u_int		rtsc_m1;	/* slope of the 1st segment in bps */
	u_int		rtsc_d;		/* the x-projection of m1 in msec */
	u_int		rtsc_m2;	/* slope of the 2nd segment in bps */
	/* link-sharing service curve */
	u_int		lssc_m1;
	u_int		lssc_d;
	u_int		lssc_m2;
	/* upper-limit service curve */
	u_int		ulsc_m1;
	u_int		ulsc_d;
	u_int		ulsc_m2;
	int		flags;
};


struct npf_altq {
	char			 ifname[IFNAMSIZ];

	void			*altq_disc;	/* discipline-specific state */
	TAILQ_ENTRY(pf_altq)	 entries;

	/* scheduler spec */
	u_int8_t		 scheduler;	/* scheduler type */
	u_int16_t		 tbrsize;	/* tokenbucket regulator size */
	u_int32_t		 ifbandwidth;	/* interface bandwidth */

	/* queue spec */
	char			 qname[NPF_QNAME_SIZE];	/* queue name */
	char			 parent[NPF_QNAME_SIZE];	/* parent name */
	u_int32_t		 parent_qid;	/* parent queue id */
	u_int32_t		 bandwidth;	/* queue bandwidth */
	u_int8_t		 priority;	/* priority */
	u_int16_t		 qlimit;	/* queue size limit */
	u_int16_t		 flags;		/* misc flags */
	union {
		struct cbq_opts		 cbq_opts;
		struct priq_opts	 priq_opts;
		struct hfsc_opts	 hfsc_opts;
	} pq_u;

	u_int32_t		 qid;		/* return value */
};

/*
 * ioctl parameter structures
 */

struct npfioc_altq {
	u_int32_t	 action;
	u_int32_t	 ticket;
	u_int32_t	 nr;
	struct npf_altq	 altq;
};

/*
 * ioctl operations
 */

#define DIOCSTARTALTQ	_IO  ('D', 42)
#define DIOCSTOPALTQ	_IO  ('D', 43)
#define DIOCADDALTQ	_IOWR('D', 45, struct npfioc_altq)
#define DIOCGETALTQS	_IOWR('D', 47, struct npfioc_altq)
#define DIOCGETALTQ	_IOWR('D', 48, struct npfioc_altq)
#define DIOCCHANGEALTQ	_IOWR('D', 49, struct npfioc_altq)

#define PF_RULESET_ALTQ		(PF_RULESET_MAX)

/*
 * npf_altqqueue
 */
TAILQ_HEAD(pf_poolqueue, pf_pool);
extern struct pf_poolqueue		  pf_pools[2];
TAILQ_HEAD(pf_altqqueue, pf_altq);
extern struct pf_altqqueue		  pf_altqs[2];

struct pf_altqqueue	 pf_altqs[2];
struct pf_palist	 pf_pabuf;
struct pf_altqqueue	*pf_altqs_active;
struct pf_altqqueue	*pf_altqs_inactive;
struct pf_status	 pf_status;

u_int32_t		 ticket_altqs_active;
u_int32_t		 ticket_altqs_inactive;
int			 altqs_inactive_open;

#endif /* NPF_ALTQ_H_ */