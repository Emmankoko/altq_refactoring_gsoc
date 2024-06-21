/*
 * starting point with integrating ALTQ with npf. functions and elements brought here will be dispersed accordingly
 * operation is derived from the understanding of how ALTQ is integrated in pf packet filter
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


struct pf_altq {
	char			 ifname[IFNAMSIZ];

	void			*altq_disc;	/* discipline-specific state */
	TAILQ_ENTRY(pf_altq)	 entries;

	/* scheduler spec */
	u_int8_t		 scheduler;	/* scheduler type */
	u_int16_t		 tbrsize;	/* tokenbucket regulator size */
	u_int32_t		 ifbandwidth;	/* interface bandwidth */

	/* queue spec */
	char			 qname[PF_QNAME_SIZE];	/* queue name */
	char			 parent[PF_QNAME_SIZE];	/* parent name */
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

struct pfioc_altq {
	u_int32_t	 action;
	u_int32_t	 ticket;
	u_int32_t	 nr;
	struct pf_altq	 altq;
};


/*
 * ioctl operations
 */

#define DIOCSTARTALTQ	_IO  ('D', 42)
#define DIOCSTOPALTQ	_IO  ('D', 43)
#define DIOCADDALTQ	_IOWR('D', 45, struct pfioc_altq)
#define DIOCGETALTQS	_IOWR('D', 47, struct pfioc_altq)
#define DIOCGETALTQ	_IOWR('D', 48, struct pfioc_altq)
#define DIOCCHANGEALTQ	_IOWR('D', 49, struct pfioc_altq)

#define PF_RULESET_ALTQ		(PF_RULESET_MAX)


/*
 * routine used by PF to mark packet with classification tag so it joins the due queue based on the tag
 * used when sending on tcp and icmp protocols 
 */

#ifdef ALTQ
	if (r != NULL && r->qid) {
#ifdef __NetBSD__
		struct m_tag	*mtag;
		struct altq_tag	*atag;

		mtag = m_tag_get(PACKET_TAG_ALTQ_QID, sizeof(*atag), M_NOWAIT);
		if (mtag != NULL) {
			atag = (struct altq_tag *)(mtag + 1);
			atag->qid = r->qid;
			/* add hints for ecn */
			atag->af = af;
			atag->hdr = mtod(m, struct ip *);
			m_tag_prepend(m, mtag);
		}
#else
		m->m_pkthdr.pf.qid = r->qid;
		/* add hints for ecn */
		m->m_pkthdr.pf.hdr = mtod(m, struct ip *);
#endif /* !__NetBSD__ */
	}
#endif /* ALTQ */


/*
 * pf_altqqueue 
 */

struct pf_altqqueue	 pf_altqs[2];
struct pf_palist	 pf_pabuf;
struct pf_altqqueue	*pf_altqs_active;
struct pf_altqqueue	*pf_altqs_inactive;
struct pf_status	 pf_status;