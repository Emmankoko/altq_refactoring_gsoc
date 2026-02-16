
/*
 * Copyright (c) 2026 The NetBSD Foundation.
 * All rights reserved.
 *
 * Author: Emmanuel Nyarko
 *
 * Permission to use, copy, modify and distribute this software and
 * its documentation is hereby granted, provided that both the copyright
 * notice and this permission notice appear in all copies of the
 * software, derivative works or modified versions, and any portions
 * thereof, and that both notices appear in supporting documentation.
 *
 * CARNEGIE MELLON ALLOWS FREE USE OF THIS SOFTWARE IN ITS "AS IS"
 * CONDITION.  CARNEGIE MELLON DISCLAIMS ANY LIABILITY OF ANY KIND
 * FOR ANY DAMAGES WHATSOEVER RESULTING FROM THE USE OF THIS SOFTWARE.
 *
 * Carnegie Mellon requests users of this software to return to
 *
 *  Software Distribution Coordinator  or  Software.Distribution@CS.CMU.EDU
 *  School of Computer Science
 *  Carnegie Mellon University
 *  Pittsburgh PA 15213-3890
 *
 * any improvements or extensions that they make and grant Carnegie the
 * rights to redistribute these changes.
 */

/*****************************************************************************
 *
 * rf_raidn.c -- implements n-way RAID Level 1
 *
 *****************************************************************************/

#include <sys/cdefs.h>
__KERNEL_RCSID(0, "$NetBSD: rf_raid1.c,v 1.39 2021/07/23 22:34:12 oster Exp $");

#include "rf_raid.h"
#include "rf_raid1.h"
#include "rf_raidn.h"
#include "rf_dag.h"
#include "rf_dagffrd.h"
#include "rf_dagffwr.h"
#include "rf_dagdegrd.h"
#include "rf_dagutils.h"
#include "rf_dagfuncs.h"
#include "rf_diskqueue.h"
#include "rf_general.h"
#include "rf_utils.h"
#include "rf_parityscan.h"
#include "rf_mcpair.h"
#include "rf_layout.h"
#include "rf_map.h"
#include "rf_engine.h"
#include "rf_reconbuffer.h"

static int
checkForReconstruction(RF_AccessStripeMap_t *, RF_Raid_t *,
	int, int *);

typedef struct RF_Raid1ConfigInfo_s {
	RF_RowCol_t *stripeIdentifier;
} RF_RaidNConfigInfo_t;


/* start of day code specific to n-way RAID level 1 */
int
rf_ConfigureRAIDN(RF_ShutdownList_t **listp, RF_Raid_t *raidPtr,
		  RF_Config_t *cfgPtr)
{
	RF_RaidLayout_t *layoutPtr = &raidPtr->Layout;
	RF_RaidNConfigInfo_t *info;
	RF_RowCol_t i;
	int cols = raidPtr->numCol;

	/*
	 * Sanity check the number of columns...
	 */
	if (raidPtr->numCol < 2) {
		return EINVAL;
	}

	/* create the RAID configuration structure */
	info = RF_MallocAndAdd(sizeof(*info), raidPtr->cleanupList);
	if (info == NULL)
		return ENOMEM;
	layoutPtr->layoutSpecificInfo = (void *) info;

	/* ... make a 1d array for n-way  */
	info->stripeIdentifier = rf_make_1d_array(cols, raidPtr->cleanupList);
	if (info->stripeIdentifier == NULL)
		return ENOMEM;

    for (i = 0; i < cols; i++) {
        info->stripeIdentifier[i] = i;
    }

	/*
     * this implementation of RAID level 1 uses one row of numCol disks
	 * a stripe will be a single data unit and rest used as parity unit
	 * stripe id = raidAddr / stripeUnitSize */
	raidPtr->totalSectors = layoutPtr->stripeUnitsPerDisk * layoutPtr->sectorsPerStripeUnit;
	layoutPtr->numStripe = layoutPtr->stripeUnitsPerDisk;
	layoutPtr->dataSectorsPerStripe = layoutPtr->sectorsPerStripeUnit;
	layoutPtr->numDataCol = 1;
	layoutPtr->numParityCol = cols - 1;
	return 0;
}

/* returns the physical disk location of the primary copy in the disk layout */
void
rf_MapSectorRAIDN(RF_Raid_t *raidPtr, RF_RaidAddr_t raidSector,
		  RF_RowCol_t *col, RF_SectorNum_t *diskSector,
		  int remap)
{
	*col = 0; /* data disk is always the fist */
	*diskSector = raidSector;
}

/* Map Parity
 *
 * returns the physical disk location of the secondary copies in the setup
 */
void
rf_MapParityRAIDN(RF_Raid_t *raidPtr, RF_RaidAddr_t raidSector,
		  RF_RowCol_t *col, RF_SectorNum_t *diskSector,
		  int remap)
{
	*col = 1;
	*diskSector = raidSector;
}

/* IdentifyStripeRAIDN
 *
 * returns a list of disks for a given redundancy group
 * for n-way RAID 1, only one redundancy group
 */
void
rf_IdentifyStripeRAIDN(RF_Raid_t *raidPtr, RF_RaidAddr_t addr,
		       RF_RowCol_t **diskids)
{
	RF_RaidNConfigInfo_t *info = raidPtr->Layout.layoutSpecificInfo;
	RF_ASSERT(addr >= 0);
	*diskids = info->stripeIdentifier;
	RF_ASSERT(*diskids);
}

/* MapSIDToPSIDRAID1
 *
 * maps a logical stripe to a stripe in the redundant array
 */
void
rf_MapSIDToPSIDRAIDN(RF_RaidLayout_t *layoutPtr,
		     RF_StripeNum_t stripeID,
		     RF_StripeNum_t *psID, RF_ReconUnitNum_t *which_ru)
{
	*which_ru = 0;
	*psID = stripeID;
}

int
rf_VerifyParityRAIDN(RF_Raid_t *raidPtr, RF_RaidAddr_t raidAddr,
		     RF_PhysDiskAddr_t *parityPDA, int correct_it,
		     RF_RaidAccessFlags_t flags)
{
	int nbytes, bcount, stripeWidth, ret, i, j, nbad, *bbufs;
	RF_DagNode_t *blockNode, *wrBlock;
	RF_DagHeader_t *rd_dag_h, *wr_dag_h;
	RF_AccessStripeMapHeader_t *asm_h;
	RF_AllocListElem_t *allocList;
#if RF_ACC_TRACE > 0
	RF_AccTraceEntry_t tracerec;
#endif
	RF_ReconUnitNum_t which_ru;
	RF_RaidLayout_t *layoutPtr;
	RF_AccessStripeMap_t *aasm;
	RF_SectorCount_t nsector;
	RF_RaidAddr_t startAddr;
	char   *bf, *buf1, *buf2;
	RF_PhysDiskAddr_t *pda;
	RF_StripeNum_t psID;
	RF_MCPair_t *mcpair;

	layoutPtr = &raidPtr->Layout;
	startAddr = rf_RaidAddressOfPrevStripeBoundary(layoutPtr, raidAddr);
	nsector = parityPDA->numSector;
	nbytes = rf_RaidAddressToByte(raidPtr, nsector);
	psID = rf_RaidAddressToParityStripeID(layoutPtr, raidAddr, &which_ru);

	asm_h = NULL;
	rd_dag_h = wr_dag_h = NULL;
	mcpair = NULL;

	ret = RF_PARITY_COULD_NOT_VERIFY;

	rf_MakeAllocList(allocList);
	if (allocList == NULL)
		return (RF_PARITY_COULD_NOT_VERIFY);
	mcpair = rf_AllocMCPair(raidPtr);
	if (mcpair == NULL)
		goto done;
	RF_ASSERT(layoutPtr->numDataCol == 1);
	RF_ASSERT(layoutPtr->numParityCol == raidPtr->numCol - layoutPtr->numDataCol);
	stripeWidth = layoutPtr->numDataCol + layoutPtr->numParityCol;
	bcount = nbytes * stripeWidth;
	bf = RF_MallocAndAdd(bcount, allocList);
	if (bf == NULL)
		goto done;
#if RF_DEBUG_VERIFYPARITY
	if (rf_verifyParityDebug) {
		printf("raid%d: RAIDN parity verify: buf=%lx bcount=%d (%lx - %lx)\n",
		       raidPtr->raidid, (long) bf, bcount, (long) bf,
		       (long) bf + bcount);
	}
#endif
	/*
         * Generate a DAG which will read the entire stripe- then we can
         * just compare data chunks versus "parity" chunks.
         */

	rd_dag_h = rf_MakeSimpleDAG(raidPtr, stripeWidth, nbytes, bf,
	    rf_DiskReadFunc, rf_DiskReadUndoFunc, "Rod", allocList, flags,
	    RF_IO_NORMAL_PRIORITY);
	if (rd_dag_h == NULL)
		goto done;

	blockNode = rd_dag_h->succedents[0];

	/*
         * Map the access to physical disk addresses (PDAs)- this will
         * get us both a list of data addresses, and "parity" addresses
         * (which are really mirror copies).
         */
	asm_h = rf_MapAccess(raidPtr, startAddr, layoutPtr->dataSectorsPerStripe,
	    bf, RF_DONT_REMAP);
	aasm = asm_h->stripeMap;

	buf1 = bf;
	/*
         * Loop through the data blocks, setting up read nodes for each.
         */
	for (pda = aasm->physInfo, i = 0; i < layoutPtr->numDataCol; i++, pda = pda->next) {
		RF_ASSERT(pda);

		rf_RangeRestrictPDA(raidPtr, parityPDA, pda, 0, 1);

		RF_ASSERT(pda->numSector != 0);
		if (rf_TryToRedirectPDA(raidPtr, pda, 0)) {
			/* cannot verify parity with dead disk */
			goto done;
		}
		pda->bufPtr = buf1;
		blockNode->succedents[i]->params[0].p = pda;
		blockNode->succedents[i]->params[1].p = buf1;
		blockNode->succedents[i]->params[2].v = psID;
		blockNode->succedents[i]->params[3].v = RF_CREATE_PARAM3(RF_IO_NORMAL_PRIORITY, which_ru);
		buf1 += nbytes;
	}
	RF_ASSERT(pda == NULL);
	/*
         * keep i, buf1 running
         *
         * Loop through parity blocks, setting up read nodes for each.
         */
	for (pda = aasm->parityInfo; i < layoutPtr->numDataCol + layoutPtr->numParityCol; i++) {
		RF_ASSERT(pda);
		RF_PhysDiskAddr_t *t_pda = pda;

		if (i > 1) {
			RF_PhysDiskAddr_t *next_pda = pda->next;
			memcpy(t_pda, aasm->parityInfo, sizeof(*t_pda));
			t_pda->col = i;
			t_pda->next = next_pda;
		}

		rf_RangeRestrictPDA(raidPtr, parityPDA, t_pda, 0, 1);
		RF_ASSERT(t_pda->numSector != 0);
		if (rf_TryToRedirectPDA(raidPtr, t_pda, 0)) {
			/* cannot verify parity with dead disk */
			goto done;
		}
		t_pda->bufPtr = buf1;
		blockNode->succedents[i]->params[0].p = t_pda;
		blockNode->succedents[i]->params[1].p = buf1;
		blockNode->succedents[i]->params[2].v = psID;
		blockNode->succedents[i]->params[3].v = RF_CREATE_PARAM3(RF_IO_NORMAL_PRIORITY, which_ru);
		buf1 += nbytes;
		pda = pda->next;
	}
	RF_ASSERT(pda == NULL);

#if RF_ACC_TRACE > 0
	memset(&tracerec, 0, sizeof(tracerec));
	rd_dag_h->tracerec = &tracerec;
#endif
#if 0
	if (rf_verifyParityDebug > 1) {
		printf("raid%d: RAID1 parity verify read dag:\n",
		       raidPtr->raidid);
		rf_PrintDAGList(rd_dag_h);
	}
#endif
	RF_LOCK_MCPAIR(mcpair);
	mcpair->flag = 0;
	RF_UNLOCK_MCPAIR(mcpair);

	rf_DispatchDAG(rd_dag_h, (void (*) (void *)) rf_MCPairWakeupFunc,
	    (void *) mcpair);

	RF_LOCK_MCPAIR(mcpair);
	while (mcpair->flag == 0) {
		RF_WAIT_MCPAIR(mcpair);
	}
	RF_UNLOCK_MCPAIR(mcpair);

	if (rd_dag_h->status != rf_enable) {
		RF_ERRORMSG("Unable to verify raidn parity: can't read stripe\n");
		ret = RF_PARITY_COULD_NOT_VERIFY;
		goto done;
	}
	/*
         * buf1 is the beginning of the data blocks chunk
         * buf2 is the beginning of the parity blocks chunk
         */
	buf1 = bf;
	buf2 = bf + (nbytes * layoutPtr->numDataCol);
	ret = RF_PARITY_OKAY;
	/*
         * bbufs is "bad bufs"- an array whose entries are the data
         * column numbers where we had miscompares. (That is, column 0
         * and column 1 of the array are mirror copies, and are considered
         * "data column 0" for this purpose).
         */
	bbufs = RF_MallocAndAdd(layoutPtr->numParityCol * sizeof(*bbufs),
	    allocList);
	nbad = 0;
	/*
         * Check data vs "parity" (mirror copy).
         */
	for (i = 0; i < layoutPtr->numParityCol; i++) {
#if RF_DEBUG_VERIFYPARITY
		if (rf_verifyParityDebug) {
			printf("raid%d: RAIDN parity verify %d bytes: i=%d buf1=%lx buf2=%lx buf=%lx\n",
			       raidPtr->raidid, nbytes, i, (long) buf1,
			       (long) buf2, (long) bf);
		}
#endif
		ret = memcmp(buf1, buf2, nbytes);
		if (ret) {
#if RF_DEBUG_VERIFYPARITY
			if (rf_verifyParityDebug > 1) {
				for (j = 0; j < nbytes; j++) {
					if (buf1[j] != buf2[j])
						break;
				}
				printf("psid=%ld j=%d\n", (long) psID, j);
				printf("buf1 %02x %02x %02x %02x %02x\n", buf1[0] & 0xff,
				    buf1[1] & 0xff, buf1[2] & 0xff, buf1[3] & 0xff, buf1[4] & 0xff);
				printf("buf2 %02x %02x %02x %02x %02x\n", buf2[0] & 0xff,
				    buf2[1] & 0xff, buf2[2] & 0xff, buf2[3] & 0xff, buf2[4] & 0xff);
			}
			if (rf_verifyParityDebug) {
				printf("raid%d: RAIDN: found bad parity, i=%d\n", raidPtr->raidid, i);
			}
#endif
			/*
		         * Parity is bad. Keep track of which columns were bad.
		         */
			if (bbufs)
				bbufs[nbad] = i + 1;
			nbad++;
			ret = RF_PARITY_BAD;
		}
		//buf1 += nbytes;
		buf2 += nbytes;
	}

	if ((ret != RF_PARITY_OKAY) && correct_it) {
		ret = RF_PARITY_COULD_NOT_CORRECT;
#if RF_DEBUG_VERIFYPARITY
		if (rf_verifyParityDebug) {
			printf("raid%d: RAIDN parity verify: parity not correct\n", raidPtr->raidid);
		}
#endif
		if (bbufs == NULL)
			goto done;
		/*
	         * Make a DAG with one write node for each bad unit. We'll simply
	         * write the contents of the data unit onto the parity unit for
	         * correction. (It's possible that the mirror copy was the correct
	         * copy, and that we're spooging good data by writing bad over it,
	         * but there's no way we can know that.
	         */
		wr_dag_h = rf_MakeSimpleDAG(raidPtr, nbad, nbytes, bf,
		    rf_DiskWriteFunc, rf_DiskWriteUndoFunc, "Wnp", allocList, flags,
		    RF_IO_NORMAL_PRIORITY);
		if (wr_dag_h == NULL)
			goto done;
		wrBlock = wr_dag_h->succedents[0];
		/*
	         * Fill in a write node for each bad compare.
	         */
		for (i = 0; i < nbad; i++) {
			j = bbufs[i];
			pda = blockNode->succedents[j]->params[0].p;
			pda->bufPtr = blockNode->succedents[0]->params[1].p;
			wrBlock->succedents[i]->params[0].p = pda;
			wrBlock->succedents[i]->params[1].p = pda->bufPtr;
			wrBlock->succedents[i]->params[2].v = psID;
			wrBlock->succedents[i]->params[3].v = RF_CREATE_PARAM3(RF_IO_NORMAL_PRIORITY, which_ru);
		}
#if RF_ACC_TRACE > 0
		memset(&tracerec, 0, sizeof(tracerec));
		wr_dag_h->tracerec = &tracerec;
#endif
#if 0
		if (rf_verifyParityDebug > 1) {
			printf("Parity verify write dag:\n");
			rf_PrintDAGList(wr_dag_h);
		}
#endif
		RF_LOCK_MCPAIR(mcpair);
		mcpair->flag = 0;
		RF_UNLOCK_MCPAIR(mcpair);

		/* fire off the write DAG */
		rf_DispatchDAG(wr_dag_h, (void (*) (void *)) rf_MCPairWakeupFunc,
		    (void *) mcpair);

		RF_LOCK_MCPAIR(mcpair);
		while (!mcpair->flag) {
			RF_WAIT_MCPAIR(mcpair);
		}
		RF_UNLOCK_MCPAIR(mcpair);
		if (wr_dag_h->status != rf_enable) {
			RF_ERRORMSG("Unable to correct RAIDN parity in VerifyParity\n");
			goto done;
		}
		ret = RF_PARITY_CORRECTED;
	}
done:
	/*
         * All done. We might've gotten here without doing part of the function,
         * so cleanup what we have to and return our running status.
         */
	if (asm_h)
		rf_FreeAccessStripeMap(raidPtr, asm_h);
	if (rd_dag_h)
		rf_FreeDAG(rd_dag_h);
	if (wr_dag_h)
		rf_FreeDAG(wr_dag_h);
	if (mcpair)
		rf_FreeMCPair(raidPtr, mcpair);
	rf_FreeAllocList(allocList);
#if RF_DEBUG_VERIFYPARITY
	if (rf_verifyParityDebug) {
		printf("raid%d: RAID1 parity verify, returning %d\n",
		       raidPtr->raidid, ret);
	}
#endif
	return ret;
}


/* DAG select from RAID 1 code to raidn */
void
rf_RAIDNDagSelect(RF_Raid_t *raidPtr, RF_IoType_t type, RF_AccessStripeMap_t *asmap, RF_VoidFuncPtr *createFunc)
{
	RF_RowCol_t fcol;

	RF_ASSERT(RF_IO_IS_R_OR_W(type));

	if (asmap->numDataFailed + asmap->numParityFailed > raidPtr->numCol - 1) { /* if all failed */
#if RF_DEBUG_DAG
		if (rf_dagDebug)
			RF_ERRORMSG("All disks failed!  Aborting I/O operation.\n");
#endif
		*createFunc = NULL;
		return;
	}
	if (asmap->numDataFailed + asmap->numParityFailed) { /* there's failure */
		int nfailed = asmap->numDataFailed + asmap->numParityFailed;
		for (int i = 0; i < nfailed; i++) {
			if (checkForReconstruction(asmap, raidPtr, i, &fcol)) { /* prior reconstruction going on */
				if (fcol == 0)
					asmap->numDataFailed--; /* fixed failed data disk */
				else
					asmap->numParityFailed--; /* fixed one of the failed mirrors */
			}
		}
	}

	if (type == RF_IO_TYPE_READ) {
		*createFunc = (RF_VoidFuncPtr) rf_CreateMirrorIdleReadDAG;
	} else {
		*createFunc = (RF_VoidFuncPtr) rf_CreateRaidNWriteDAG;
	}
}


/* returns either prior recon went on or not */
static int
checkForReconstruction(RF_AccessStripeMap_t *asmap, RF_Raid_t *raidPtr, int index, int *failcol)
{
	RF_RowCol_t fcol, oc __unused;
	RF_PhysDiskAddr_t *failedPDA;
	int     prior_recon;
	RF_RowStatus_t rstat;
	RF_SectorNum_t oo __unused;

	failedPDA = asmap->failedPDAs[index];
	fcol = failedPDA->col;
	rstat = raidPtr->status;
	prior_recon = (rstat == rf_rs_reconfigured) || (
		(rstat == rf_rs_reconstructing) ?
		rf_CheckRUReconstructed(raidPtr->reconControl->reconMap, failedPDA->startSector) : 0
		);
	if (prior_recon) {
		oc = fcol;
		oo = failedPDA->startSector;
		/*
			* If we did distributed sparing, we'd monkey with that here.
			* But we don't, so we'll
			*/
		failedPDA->col = raidPtr->Disks[fcol].spareCol;

#if RF_DEBUG_DAG > 0 || RF_DEBUG_MAP > 0
		if (rf_dagDebug || rf_mapDebug) {
			printf("raid%d: Redirected type '%c' c %d o %ld -> c %d o %ld\n",
				raidPtr->raidid, type, oc,
				(long) oo,
				failedPDA->col,
				(long) failedPDA->startSector);
		}
#endif
	}
	*failcol = fcol;
	return prior_recon;
}
