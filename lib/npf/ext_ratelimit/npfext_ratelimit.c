/*-
 * Copyright (c) 2026 The NetBSD Foundation, Inc.
 * All rights reserved.
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

#include <sys/cdefs.h>
__RCSID("$NetBSD: npfext_rndblock.c,v 1.2 2018/09/29 14:41:37 rmind Exp $");

#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <assert.h>
#include <errno.h>
#include <err.h>
#include <math.h>

#include <npf.h>

int		npfext_ratelimit_init(void);
nl_ext_t *	npfext_ratelimit_construct(const char *);
int		npfext_ratelimit_param(nl_ext_t *, const char *, const char *);

int
npfext_ratelimit_init(void)
{
	/* Nothing to initialise. */
	return 0;
}

static uint64_t
convert_to_bits(const char *val, const char *s)
{
	char *cp;
	double bps = strtod(val, &cp);

	if (bps < 0) {
		errx(EXIT_FAILURE, "Negative %s not allowed\n", s);
	}

	if (bps > UINT64_MAX) {
		errx(EXIT_FAILURE, "Invalid %s: Too Big\n", s);
	}

	uint64_t uval = (uint64_t)bps;

	if (*cp == '\0')
		return uval;

	/* unit appended assumed here */
	/* fine if you pass 100B -> 100 bits per sec */
	if (!strcmp(cp, "b"))
		; /* nothing */
	else if (!strcmp(cp, "K"))
		uval = bps * 1000;
	else if (!strcmp(cp, "M"))
		uval = bps * 1000 * 1000;
	else if (!strcmp(cp, "G"))
		uval = bps * 1000 * 1000 * 1000;
	else if (!strcmp(cp, "T"))
		uval = bps * 1000 * 1000 * 1000 * 1000;
	else
		errx(EXIT_FAILURE, "invalid %s unit %s\n",s, cp);

	return uval;
}

nl_ext_t *
npfext_ratelimit_construct(const char *name)
{
	assert(strcmp(name, "ratelimit") == 0);
	return npf_ext_construct(name);
}

int
npfext_ratelimit_param(nl_ext_t *ext, const char *param, const char *val)
{
	const char *params[3] = {
		"bitrate", "normal-burst", "extended-burst"
	};

	if (val == NULL) {
		return EINVAL;
	}
	for (uint32_t i = 0; i < __arraycount(params); i++) {
		const char *name = params[i];

		if (strcmp(name, param) != 0) {
			continue;
		}

		/*
		 * we are accpeting human friendly values for rates. eg: 2M => 2Mbps are accepted
		 * else, raw figures are processed as bit values.
		 */
		uint64_t bits = convert_to_bits(val, name);

		npf_ext_param_u64(ext, name, bits);

		return 0;
	}

	/* Invalid parameter, if not found. */
	return EINVAL;
}
