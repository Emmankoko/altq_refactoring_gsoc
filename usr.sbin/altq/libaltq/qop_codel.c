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
#include <sys/socket.h>
#include <sys/sockio.h>
#include <sys/ioctl.h>
#include <sys/fcntl.h>
#include <net/if.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <stddef.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <syslog.h>
#include <netdb.h>

#include <altq/altq.h>
#include <altq/altq_codel.h>
#include "altq_qop.h"
#include "qop_codel.h"

/* internal function prototypes */
static int codel_attach(struct ifinfo *);
static int codel_detach(struct ifinfo *);
static int codel_enable(struct ifinfo *);
static int codel_disable(struct ifinfo *);

#define CODEL_DEVICE	"/dev/altq/codel"
/*
 * CoDel is an Active Queue Management mechanism which is not in immediate need
 * of an interface parser since it's going to be worked on priq, hfsc, or cbq
 * scheduled queues.
 */
static int codel_fd = -1;
static int codel_refcount = 0;

static struct qdisc_ops codel_qdisc = {
	ALTQT_CODEL,
	"codel",
	codel_attach,
	codel_detach,
	NULL,			/* clear */
	codel_enable,
	codel_disable,
	NULL,			/* add class */
	NULL,			/* modify class */
	NULL,			/* delete class */
	NULL,			/* add filter */
	NULL			/* delete filter */
};

/*
 * parser interface
 */
#define EQUAL(s1, s2)	(strcmp((s1), (s2)) == 0)

int
codel_interface_parser(const char *ifname, int argc, char **argv)
{
	u_int  	bandwidth = 100000000;	/* 100Mbps */
	u_int	tbrsize = 0;
	u_int64_t	target = 0;		/* 0: use default */
	u_int64_t	interval = 0;	/* 0: use default */
	int	qlimit = 60;
	int ecn = 0;

	/*
	 * process options
	 */
	while (argc > 0) {
		if (EQUAL(*argv, "bandwidth")) {
			argc--; argv++;
			if (argc > 0)
				bandwidth = atobps(*argv);
		} else if (EQUAL(*argv, "tbrsize")) {
			argc--; argv++;
			if (argc > 0)
				tbrsize = atobytes(*argv);
		} else if (EQUAL(*argv, "qlimit")) {
			argc--; argv++;
			if (argc > 0)
				qlimit = (int)strtol(*argv, NULL, 0);
		} else if (EQUAL(*argv, "target")) {
			argc--; argv++;
			if (argc > 0)
				target = strtoull(*argv, NULL, 0);
		} else if (EQUAL(*argv, "interval")) {
			argc--; argv++;
			if (argc > 0)
				interval = strtoull(*argv, NULL, 0);
		} else if (EQUAL(*argv, "codel")) {
			/* just skip */
		} else if (EQUAL(*argv, "ecn")) {
			ecn |= CODEL_ECN;
		} else {
			LOG(LOG_ERR, 0, "Unknown keyword '%s'", *argv);
			return (0);
		}
		argc--; argv++;
	}

	if (qcmd_tbr_register(ifname, bandwidth, tbrsize) != 0)
		return (0);

	if (qcmd_codel_add_if(ifname, bandwidth, target, interval,
			    qlimit, ecn) != 0)
		return (0);
	return (1);
}

/*
 * qcmd api
 */
int
qcmd_codel_add_if(const char *ifname, u_int bandwidth, u_int64_t target,
		u_int64_t interval, int qlimit, int ecn)
{
	int error;

	error = qop_codel_add_if(NULL, ifname, bandwidth, target, interval,
			       qlimit, ecn);
	if (error != 0)
		LOG(LOG_ERR, errno, "%s: can't add codel on interface '%s'",
		    qoperror(error), ifname);
	return (error);
}

/*
 * qop api
 */
int
qop_codel_add_if(struct ifinfo **rp, const char *ifname,
	        u_int bandwidth, u_int64_t target, u_int64_t interval,
		    int qlimit, int ecn)
{
	struct ifinfo *ifinfo = NULL;
	struct codel_ifinfo *codel_ifinfo;
	int error;

	if ((codel_ifinfo = calloc(1, sizeof(*codel_ifinfo))) == NULL)
		return (QOPERR_NOMEM);
	codel_ifinfo->target   = target;
	codel_ifinfo->interval = interval;
	codel_ifinfo->qlimit   = qlimit;
	codel_ifinfo->ecn   = ecn;

	error = qop_add_if(&ifinfo, ifname, bandwidth,
			   &codel_qdisc, codel_ifinfo);
	if (error != 0) {
		free(codel_ifinfo);
		return (error);
	}

	if (rp != NULL)
		*rp = ifinfo;
	return (0);
}

/*
 *  system call interfaces for qdisc_ops
 */
static int
codel_attach(struct ifinfo *ifinfo)
{
	struct codel_interface iface;
	struct codel_ifinfo *codel_ifinfo;
	struct codel_conf conf;

	if (codel_fd < 0 &&
	    (codel_fd = open(CODEL_DEVICE, O_RDWR)) < 0 &&
	    (codel_fd = open_module(CODEL_DEVICE, O_RDWR)) < 0) {
		LOG(LOG_ERR, errno, "CODEL open");
		return (QOPERR_SYSCALL);
	}

	codel_refcount++;
	memset(&iface, 0, sizeof(iface));
	strncpy(iface.codel_ifname, ifinfo->ifname, IFNAMSIZ);

	if (ioctl(codel_fd, CODEL_IF_ATTACH, &iface) < 0)
		return (QOPERR_SYSCALL);

	/* set codel parameters */
	codel_ifinfo = (struct codel_ifinfo *)ifinfo->private;
	memset(&conf, 0, sizeof(conf));
	strncpy(conf.iface.codel_ifname, ifinfo->ifname, IFNAMSIZ);
	conf.target	  = codel_ifinfo->target;
	conf.interval = codel_ifinfo->interval;
	conf.ecn    = codel_ifinfo->ecn;
	conf.limit    = codel_ifinfo->qlimit;
	if (ioctl(codel_fd, CODEL_CONFIG, &conf) < 0)
		return (QOPERR_SYSCALL);

#if 1
	LOG(LOG_INFO, 0, "codel attached to %s", iface.codel_ifname);
#endif
	return (0);
}

static int
codel_detach(struct ifinfo *ifinfo)
{
	struct codel_interface iface;

	memset(&iface, 0, sizeof(iface));
	strncpy(iface.codel_ifname, ifinfo->ifname, IFNAMSIZ);

	if (ioctl(codel_fd, CODEL_IF_DETACH, &iface) < 0)
		return (QOPERR_SYSCALL);

	if (--codel_refcount == 0) {
		close(codel_fd);
		codel_fd = -1;
	}
	return (0);
}

static int
codel_enable(struct ifinfo *ifinfo)
{
	struct codel_interface iface;

	memset(&iface, 0, sizeof(iface));
	strncpy(iface.codel_ifname, ifinfo->ifname, IFNAMSIZ);

	if (ioctl(codel_fd, CODEL_ENABLE, &iface) < 0)
		return (QOPERR_SYSCALL);
	return (0);
}

static int
codel_disable(struct ifinfo *ifinfo)
{
	struct codel_interface iface;

	memset(&iface, 0, sizeof(iface));
	strncpy(iface.codel_ifname, ifinfo->ifname, IFNAMSIZ);

	if (ioctl(codel_fd, CODEL_DISABLE, &iface) < 0)
		return (QOPERR_SYSCALL);
	return (0);
}



