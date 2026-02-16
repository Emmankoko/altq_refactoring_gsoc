/*
 * Copyright (c) 2026 The NetBSD Foundation Inc
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
 *
 */

/* header file for n-way RAID Level 1 */

#ifndef _RF__RF_RAIDN_H_
#define _RF__RF_RAIDN_H_

#include <dev/raidframe/raidframevar.h>

int rf_ConfigureRAIDN(RF_ShutdownList_t **, RF_Raid_t *, RF_Config_t *);
void rf_MapSectorRAIDN(RF_Raid_t *, RF_RaidAddr_t, RF_RowCol_t *,
		       RF_SectorNum_t *, int);
void rf_MapParityRAIDN(RF_Raid_t *, RF_RaidAddr_t, RF_RowCol_t *,
		       RF_SectorNum_t *, int);
void rf_IdentifyStripeRAIDN(RF_Raid_t *, RF_RaidAddr_t, RF_RowCol_t **);
void rf_MapSIDToPSIDRAIDN(RF_RaidLayout_t *, RF_StripeNum_t, RF_StripeNum_t *,
			  RF_ReconUnitNum_t *);
void rf_RAIDNDagSelect(RF_Raid_t *, RF_IoType_t, RF_AccessStripeMap_t *,
		       RF_VoidFuncPtr *);
int rf_VerifyParityRAIDN(RF_Raid_t *, RF_RaidAddr_t, RF_PhysDiskAddr_t *,
			 int, RF_RaidAccessFlags_t);

#endif				/* !_RF__RF_RAID1_H_ */