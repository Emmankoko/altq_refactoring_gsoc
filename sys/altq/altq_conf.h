/*	$NetBSD: altq_conf.h,v 1.10 2007/03/04 05:59:01 christos Exp $	*/
/*	$KAME: altq_conf.h,v 1.10 2005/04/13 03:44:24 suz Exp $	*/

/*
 * Copyright (C) 1998-2002
 *	Sony Computer Science Laboratories Inc.  All rights reserved.
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
#ifndef _ALTQ_ALTQ_CONF_H_
#define	_ALTQ_ALTQ_CONF_H_
#ifdef ALTQ3_COMPAT

#ifdef _KERNEL

#include <sys/param.h>
#include <sys/conf.h>
#include <sys/kernel.h>

#ifndef dev_decl
#ifdef __STDC__
#define	dev_decl(n,t)	d_ ## t ## _t n ## t
#else
#define	dev_decl(n,t)	d_/**/t/**/_t n/**/t
#endif
#endif

#if defined(__NetBSD__)
typedef int d_open_t(dev_t, int, int, struct lwp *);
typedef int d_close_t(dev_t, int, int, struct lwp *);
typedef int d_ioctl_t(dev_t, u_long, void *, int, struct lwp *);
#endif /* __NetBSD__ */

#if defined(__OpenBSD__)
typedef int d_open_t(dev_t, int, int, struct proc *);
typedef int d_close_t(dev_t, int, int, struct proc *);
typedef int d_ioctl_t(dev_t, u_long, void *, int, struct proc *);

#define	noopen	(dev_type_open((*))) enodev
#define	noclose	(dev_type_close((*))) enodev
#define	noioctl	(dev_type_ioctl((*))) enodev

int altqopen(dev_t, int, int, struct proc *);
int altqclose(dev_t, int, int, struct proc *);
int altqioctl(dev_t, u_long, void *, int, struct proc *);
#endif

enum device_routine; /* device routine types */
enum TBR; /* tbr manipulation list. mainly get and set*/

int altq_routine(enum device_routine, dev_t, int, int, struct lwp *);
int get_queue_type(void *addr);
int tbr(void *, enum TBR);
bool altq_auth(int *, struct lwp *);

/*
 * altq queueing discipline switch structure
 */
struct altqsw {
	const char	*d_name;
	d_open_t	*d_open;
	d_close_t	*d_close;
	d_ioctl_t	*d_ioctl;
};

#define	altqdev_decl(n) \
	dev_decl(n,open); dev_decl(n,close); dev_decl(n,ioctl)

#endif /* _KERNEL */
#endif /* ALTQ3_COMPAT */
#endif /* _ALTQ_ALTQ_CONF_H_ */
