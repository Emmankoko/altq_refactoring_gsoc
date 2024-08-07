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



