/*
 * NPF layer 2 ruleset tests.
 *
 * Public Domain.
 */

#ifdef _KERNEL
#include <sys/types.h>
#endif

#include "npf_impl.h"
#include "npf_test.h"

#define	RESULT_PASS	0
#define	RESULT_BLOCK	ENETUNREACH

/*
 * in this module, we run tests on layer 2 packets for configs that has only layer 3 rules
 * All incoming frames at layer 2 should pass so we ensure that
 * npf config with no layer 2 rules should for no chance be blocked by npf
 * at layer 2
 * config to be loaded is ../npfl3test.conf
 */

static const struct test_case {
	const char *	src;
	const char *	dst;
	uint16_t		etype;
	const char *	ifname;
	int		di;
	int		ret;
} test_cases[] = {
	{
		/* pass ether in final from $mac1 to $mac2 type $E_IPv6 */
		.src = "00:00:5E:00:53:00",	.dst = "00:00:5E:00:53:01",
		.ifname = IFNAME_INT,		.etype = ETHERTYPE_IPV6,
		.di = PFIL_IN,			.ret = RESULT_PASS
	},
	{
		/* block ether in final from $mac2 */
		.src = "00:00:5E:00:53:01",	.dst = "00:00:5E:00:53:02",
		.ifname = IFNAME_INT,		.etype = ETHERTYPE_IP,
		.di = PFIL_IN,			.ret = RESULT_PASS
	},
	{
		/* goto default: block all (since direction is not matching ) */
		.src = "00:00:5E:00:53:00",	.dst = "00:00:5E:00:53:02",
		.ifname = IFNAME_INT,		.etype = ETHERTYPE_IP,
		.di = PFIL_IN,			.ret = RESULT_PASS
	},
};

static int
run_raw_testcase(unsigned i)
{
	const struct test_case *t = &test_cases[i];
	npf_t *npf = npf_getkernctx();
	npf_cache_t *npc;
	struct mbuf *m;
	npf_rule_t *rl;
	int slock, error;

	m = mbuf_get_frame(t->src, t->dst, htons(t->etype));
	npc = get_cached_pkt(m, t->ifname, NPF_RULE_LAYER_2);

	slock = npf_config_read_enter(npf);
	rl = npf_ruleset_inspect(npc, npf_config_ruleset(npf), t->di, NPF_RULE_LAYER_2);
	if (rl) {
		npf_match_info_t mi;
		error = npf_rule_conclude(rl, &mi);
	} else {
		error = ENOENT;
	}
	npf_config_read_exit(npf, slock);

	put_cached_pkt(npc);
	return error;
}

static bool
test_static(bool verbose)
{
	for (unsigned i = 0; i < __arraycount(test_cases); i++) {
		const struct test_case *t = &test_cases[i];
		int error;

		if (npf_test_getif(t->ifname) == NULL) {
			printf("Interface %s is not configured.\n", t->ifname);
			return false;
		}

		error = run_raw_testcase(i);

		if (verbose) {
			printf("rule test %d:\texpected %d\n"
				"\t\t-> returned %d\n",
				i + 1, t->ret, error);
		}
		CHECK_TRUE(error == t->ret);
	}
	return true;
}

/* sorry for long function name */
bool
npf_layer2only_test(bool verbose)
{
	bool ok;

	ok = test_static(verbose);
	CHECK_TRUE(ok);

	return true;
}
