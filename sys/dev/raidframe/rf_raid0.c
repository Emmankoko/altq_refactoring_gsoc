/*	$NetBSD: rf_raid0.c,v 1.16 2019/02/09 03:34:00 christos Exp $	*/
/*
 * Copyright (c) 1995 Carnegie-Mellon University.
 * All rights reserved.
 *
 * Author: Mark Holland
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

/***************************************
 *
 * rf_raid0.c -- implements RAID Level 0
 *
 ***************************************/

#include <sys/cdefs.h>
__KERNEL_RCSID(0, "$NetBSD: rf_raid0.c,v 1.16 2019/02/09 03:34:00 christos Exp $");

#include <dev/raidframe/raidframevar.h>

#include "rf_raid.h"
#include "rf_raid0.h"
#include "rf_dag.h"
#include "rf_dagffrd.h"
#include "rf_dagffwr.h"
#include "rf_dagutils.h"
#include "rf_dagfuncs.h"
#include "rf_general.h"
#include "rf_parityscan.h"
#include "rf_mcpair.h"
#include "rf_utils.h"
#include "rf_engine.h"
#include "rf_map.h"

typedef struct RF_Raid0ConfigInfo_s {
	RF_RowCol_t *stripeIdentifier;
}       RF_Raid0ConfigInfo_t;

int
rf_ConfigureRAID0(RF_ShutdownList_t **listp, RF_Raid_t *raidPtr,
		  RF_Config_t *cfgPtr)
{
	RF_RaidLayout_t *layoutPtr = &raidPtr->Layout;
	RF_Raid0ConfigInfo_t *info;
	RF_RowCol_t i;

	/* create a RAID level 0 configuration structure */
	info = RF_MallocAndAdd(sizeof(*info), raidPtr->cleanupList);
	if (info == NULL)
		return (ENOMEM);
	layoutPtr->layoutSpecificInfo = (void *) info;

	info->stripeIdentifier = RF_MallocAndAdd(raidPtr->numCol
	    * sizeof(*info->stripeIdentifier), raidPtr->cleanupList);
	if (info->stripeIdentifier == NULL)
		return (ENOMEM);
	for (i = 0; i < raidPtr->numCol; i++)
		info->stripeIdentifier[i] = i;

	raidPtr->totalSectors = layoutPtr->stripeUnitsPerDisk * raidPtr->numCol * layoutPtr->sectorsPerStripeUnit;
	layoutPtr->numStripe = layoutPtr->stripeUnitsPerDisk;
	layoutPtr->dataSectorsPerStripe = raidPtr->numCol * layoutPtr->sectorsPerStripeUnit;
	layoutPtr->numDataCol = raidPtr->numCol;
	layoutPtr->numParityCol = 0;
	return (0);
}

void
rf_MapSectorRAID0(RF_Raid_t *raidPtr, RF_RaidAddr_t raidSector,
	      RF_RowCol_t *col, RF_SectorNum_t *diskSector, int remap)
{
	RF_StripeNum_t SUID = raidSector / raidPtr->Layout.sectorsPerStripeUnit;
	*col = SUID % raidPtr->numCol;
	*diskSector = (SUID / raidPtr->numCol) * raidPtr->Layout.sectorsPerStripeUnit +
	    (raidSector % raidPtr->Layout.sectorsPerStripeUnit);
}

void
rf_MapParityRAID0(RF_Raid_t *raidPtr,
    RF_RaidAddr_t raidSector, RF_RowCol_t *col,
    RF_SectorNum_t *diskSector, int remap)
{
	*col = 0;
	*diskSector = 0;
}

void
rf_IdentifyStripeRAID0(RF_Raid_t *raidPtr, RF_RaidAddr_t addr,
		       RF_RowCol_t **diskids)
{
	RF_Raid0ConfigInfo_t *info;

	info = raidPtr->Layout.layoutSpecificInfo;
	*diskids = info->stripeIdentifier;
}

void
rf_MapSIDToPSIDRAID0(RF_RaidLayout_t *layoutPtr,
    RF_StripeNum_t stripeID, RF_StripeNum_t *psID, RF_ReconUnitNum_t *which_ru)
{
	*which_ru = 0;
	*psID = stripeID;
}

void
rf_RAID0DagSelect(
    RF_Raid_t * raidPtr,
    RF_IoType_t type,
    RF_AccessStripeMap_t * asmap,
    RF_VoidFuncPtr * createFunc)
{
	if (raidPtr->numFailures > 0) {
		*createFunc = NULL;
		return;
	}
	*createFunc = ((type == RF_IO_TYPE_READ) ?
	    (RF_VoidFuncPtr) rf_CreateFaultFreeReadDAG : (RF_VoidFuncPtr) rf_CreateRAID0WriteDAG);
}

int
rf_VerifyParityRAID0(RF_Raid_t *raidPtr,
    RF_RaidAddr_t raidAddr, RF_PhysDiskAddr_t *parityPDA,
    int correct_it, RF_RaidAccessFlags_t flags)
{
	/*
         * No parity is always okay.
         */
	return (RF_PARITY_OKAY);
}

int
rf_RAID0Scrub(RF_Raid_t *raidPtr,
    RF_RaidAddr_t raidAddr, RF_PhysDiskAddr_t *parityPDA,
    int correct_it, RF_RaidAccessFlags_t flags)
{
	RF_RaidLayout_t *layoutPtr = &(raidPtr->Layout);
	RF_RaidAddr_t startAddr = rf_RaidAddressOfPrevStripeBoundary(layoutPtr,
								     raidAddr);
	RF_SectorCount_t numsector = layoutPtr->sectorsPerStripeUnit;;
	int     numbytes = rf_RaidAddressToByte(raidPtr, numsector);
	RF_DagHeader_t *rd_dag_h;	/* read dag */
	RF_DagNode_t *blockNode;
	RF_AccessStripeMapHeader_t *asm_h;
	RF_AccessStripeMap_t *asmap;
	RF_AllocListElem_t *alloclist;
	RF_PhysDiskAddr_t *pda;
	char *bf, *buf1;
	int i, ret;
	RF_ReconUnitNum_t which_ru;
	RF_StripeNum_t psID = rf_RaidAddressToParityStripeID(layoutPtr,
							     raidAddr,
							     &which_ru);
	int     stripeWidth = layoutPtr->numDataCol;
#if RF_ACC_TRACE > 0
	RF_AccTraceEntry_t tracerec;
#endif
	RF_MCPair_t *mcpair;

	RF_ASSERT(parityPDA == NULL);

	mcpair = rf_AllocMCPair(raidPtr);
	rf_MakeAllocList(alloclist);
	bf = RF_MallocAndAdd(numbytes
	    * (layoutPtr->numDataCol), alloclist);

	buf1 = bf;

	rd_dag_h = rf_MakeSimpleDAG(raidPtr, stripeWidth, numbytes, bf, rf_DiskReadFunc, rf_DiskReadUndoFunc,
	    "Rod", alloclist, flags, RF_IO_NORMAL_PRIORITY);
	blockNode = rd_dag_h->succedents[0];

	/* map the stripe and fill in the PDAs in the dag */
	asm_h = rf_MapAccess(raidPtr, startAddr, layoutPtr->dataSectorsPerStripe, bf, RF_DONT_REMAP);
	asmap = asm_h->stripeMap;

	for (pda = asmap->physInfo, i = 0; i < layoutPtr->numDataCol; i++, pda = pda->next) {
		RF_ASSERT(pda);
		RF_ASSERT(pda->numSector != 0);

		pda->bufPtr = buf1;
		blockNode->succedents[i]->params[0].p = pda;
		blockNode->succedents[i]->params[2].v = psID;
		blockNode->succedents[i]->params[3].v = RF_CREATE_PARAM3(RF_IO_NORMAL_PRIORITY, which_ru);
		buf1 += numbytes;
	}

	RF_ASSERT(!asmap->parityInfo);

	/* fire off the DAG */
#if RF_ACC_TRACE > 0
	memset(&tracerec, 0, sizeof(tracerec));
	rd_dag_h->tracerec = &tracerec;
#endif
#if 0
	if (rf_verifyParityDebug) {
		printf("Parity verify read dag:\n");
		rf_PrintDAGList(rd_dag_h);
	}
#endif
	RF_LOCK_MCPAIR(mcpair);
	mcpair->flag = 0;
	RF_UNLOCK_MCPAIR(mcpair);

	rf_DispatchDAG(rd_dag_h, (void (*) (void *)) rf_MCPairWakeupFunc,
	    (void *) mcpair);

	RF_LOCK_MCPAIR(mcpair);
	while (!mcpair->flag)
		RF_WAIT_MCPAIR(mcpair);
	RF_UNLOCK_MCPAIR(mcpair);

	if (rd_dag_h->status != rf_enable)
		ret = RF_PARITY_COULD_NOT_VERIFY;
	else
		ret =RF_PARITY_OKAY; /* return for return sake, but no parity check necessarily */

	/*
	* All done.
	*/
	if (asm_h)
		rf_FreeAccessStripeMap(raidPtr, asm_h);
	if (rd_dag_h)
		rf_FreeDAG(rd_dag_h);
	if (mcpair)
		rf_FreeMCPair(raidPtr, mcpair);
	rf_FreeAllocList(alloclist);

	return ret;
}