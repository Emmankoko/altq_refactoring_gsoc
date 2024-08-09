/*
 * Copyright (C) 1999-2000
 *	Sony Computer Science Laboratories, Inc.  All rights reserved.
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
 * THIS SOFTWARE IS PROVIDED BY SONY CSL AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL SONY CSL OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include <sys/param.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <net/if.h>
#include <netinet/in.h>
#include <altq/altq.h>
#include <altq/altq_codel.h>

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <signal.h>
#include <errno.h>
#include <err.h>

#include "altqstat.h"

/*
 * For Userland codel stats,
 * We use codel_stats for printing statistics
 * And use codel_ifstats for codel interfaces
 */

void
codel_stat_loop(int fd, const char *ifname, int count, int interval)
{
	struct codel_ifstats codel_stats;
	struct timeval cur_time, last_time;
	u_int64_t last_bytes;
	double sec;
	int cnt = count;
	sigset_t		omask;

	strlcpy(codel_stats.iface.codel_ifname, ifname,
		sizeof(codel_stats.iface.codel_ifname));

	gettimeofday(&last_time, NULL);
	last_time.tv_sec -= interval;
	last_bytes = 0;

	for (;;) {
		if (ioctl(fd, CODEL_GETSTATS, &codel_stats) < 0)
			err(1, "ioctl CODEL_GETSTATS");

		gettimeofday(&cur_time, NULL);
		sec = calc_interval(&cur_time, &last_time);

		printf(" q_len:%d , q_limit:%d , maxpacket:\n",
		       codel_stats.qlength,
		       codel_stats.qlimit, codel_stats.stats.maxpacket);
		printf(" xmit:%llu pkts, drop:%llu pkts \n",
		       (ull)codel_stats.stats.cl_xmitcnt.packets,
		       (ull)codel_stats.stats.cl_dropcnt.packets)
		if (codel_stats.stats.marked_packets != 0)
			printf(" marked: %u\n", codel_stats.stats.marked_packets);
		printf(" throughput: %sbps\n",
		       rate2str(calc_rate(codel_stats.stat.cl_xmitcnt.bytes,
					  last_bytes, sec)));
		}
		printf("\n");

		last_bytes = codel_stats.cl_xmitcnt.bytes;
		last_time = cur_time;

		if (count != 0 && --cnt == 0)
			break;

		/* wait for alarm signal */
		if (sigprocmask(SIG_BLOCK, NULL, &omask) == 0)
			sigsuspend(&omask);
	}
}

/* Codel stats to be used on other disciplines */
int
print_codelstats(struct codel_stats *cod)
{
	printf("     CoDel xmit:%llu (maxpacket:%u marked:%u)\n",
	       (ull)cod->cl_xmitcnt.packets,
	       cod->maxpacket,
	       cod->marked_packets);
	return 0;
}
