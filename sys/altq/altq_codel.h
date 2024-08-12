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
 */

#ifndef _ALTQ_ALTQ_CODEL_H_
#define	_ALTQ_ALTQ_CODEL_H_

#include <altq/altq_classq.h>

#ifdef ALTQ3_COMPAT
struct codel_interface {
	char codel_ifname[IFNAMSIZ];
};


/* configuration elemtents for CoDel */
struct codel_conf {
	struct codel_interface iface;
	u_int64_t target;		/* queueing delay target*/
	u_int64_t interval;	/* time period over delay */
	int ecn;		/*checks whether ecn is enabled */
	int limit;		/* maximum number of packets */
};

struct codel_stats {
	u_int32_t	maxpacket;
	u_int	marked_packets;
	struct pktcntr	cl_dropcnt;
	struct pktcntr	cl_xmitcnt;	/* transmitted packet counter */
};

/**
 * struct codel_params - contains codel parameters
 *  <at> target:	target queue size (in time units)
 *  <at> interval:	width of moving time window
 *  <at> ecn:	is Explicit Congestion Notification enabled
 */
struct codel_params {
	u_int64_t	target;
	u_int64_t	interval;
	int		ecn;
};

struct codel_ifstats {
	struct codel_interface iface;
	u_int			qlength;
	u_int			qlimit;
	struct codel_stats	stats;
	struct codel_params params;
};

#endif /* ALTQ3_COMPAT */

/*codel flags*/
#define CODEL_ECN	0x01	/* for marking packets*/

/*
 * CBQ_STATS_VERSION is defined in altq.h to work around issues stemming
 * from mixing of public-API and internal bits in each scheduler-specific
 * header.
 */

#ifdef ALTQ3_COMPAT
/*
 * IOCTLs for CoDel
 */

#define CODEL_IF_ATTACH			_IOW('Q', 11, struct codel_interface)
#define CODEL_IF_DETACH			_IOW('Q', 12, struct codel_interface)
#define CODEL_ENABLE			_IOW('Q', 13, struct codel_interface)
#define CODEL_DISABLE			_IOW('Q', 14, struct codel_interface)
#define CODEL_CONFIG			_IOWR('Q', 16, struct codel_conf)
#define CODEL_GETSTATS			_IOWR('Q', 22, struct codel_ifstats)
#define CODEL_SETDEFAULTS		_IOW('Q', 30, struct codel_params)
#endif /* ALTQ3_COMPAT*/

#ifdef _KERNEL


/**
 * struct codel_vars - contains codel variables
 *  <at> count:		how many drops we've done since the last time we
 *			entered dropping state
 *  <at> lastcount:	count at entry to dropping state
 *  <at> dropping:	set to true if in dropping state
 *  <at> rec_inv_sqrt:	reciprocal value of sqrt(count) >> 1
 *  <at> first_above_time:	when we went (or will go) continuously above
 *				target for interval
 *  <at> drop_next:	time to drop next packet, or when we dropped last
 *  <at> ldelay:	sojourn time of last dequeued packet
 */
struct codel_vars {
	u_int32_t	count;
	u_int32_t	lastcount;
	int		dropping;
	u_int16_t	rec_inv_sqrt;
	u_int64_t	first_above_time;
	u_int64_t	drop_next;
	u_int64_t	ldelay;
};

struct codel {
	int			last_pps;
	struct codel_params	params;
	struct codel_vars	vars;
	struct codel_stats	stats;
	struct timeval		last_log;
	u_int32_t		drop_overlimit;
};

/*
 * codel interface state
 */
#ifdef ALTQ3_COMPAT
struct codel_if {
	struct codel_if		*cif_next;	/* interface state list */
	struct ifaltq		*cif_ifq;	/* backpointer to ifaltq */
	u_int			cif_bandwidth;	/* link bandwidth in bps */

	class_queue_t	*cl_q;		/* class queue structure */
	struct codel	*codel;

	/* statistics */
	struct codel_ifstats cl_stats;
};
#endif /* ALTQ3_COMPAT */

struct codel	*codel_alloc(int, int, int);
void		 codel_destroy(struct codel *);
int		 codel_addq(struct codel *, class_queue_t *, struct mbuf *);
struct mbuf	*codel_getq(struct codel *, class_queue_t *);
void		 codel_getstats(struct codel *, struct codel_stats *);

#endif /* _KERNEL */

#endif /* _ALTQ_ALTQ_CODEL_H_ */