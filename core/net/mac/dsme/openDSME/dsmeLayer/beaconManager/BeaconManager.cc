/*
 * openDSME
 *
 * Implementation of the Deterministic & Synchronous Multi-channel Extension (DSME)
 * described in the IEEE 802.15.4-2015 standard
 *
 * Authors: Florian Kauer <florian.kauer@tuhh.de>
 *          Maximilian Koestler <maximilian.koestler@tuhh.de>
 *          Sandrina Backhauss <sandrina.backhauss@tuhh.de>
 *
 * Based on
 *          DSME Implementation for the INET Framework
 *          Tobias Luebkert <tobias.luebkert@tuhh.de>
 *
 * Copyright (c) 2015, Institute of Telematics, Hamburg University of Technology
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
 * 3. Neither the name of the Institute nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE INSTITUTE AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE INSTITUTE OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#define DSME_BEACON_MANAGER

#include "./BeaconManager.h"

#include "../../../dsme_platform.h"
#include "../../helper/DSMEDelegate.h"
#include "../../interfaces/IDSMEMessage.h"
#include "../../interfaces/IDSMEPlatform.h"
#include "../../mac_services/dataStructures/DSMESABSpecification.h"
#include "../../mac_services/dataStructures/DSMEBitVector.h"
#include "../../mac_services/dataStructures/IEEE802154MacAddress.h"
#include "../../mac_services/dataStructures/PANDescriptor.h"
#include "../../mac_services/dataStructures/TimeSyncSpecification.h"
#include "../../mac_services/mlme_sap/BEACON_NOTIFY.h"
#include "../../mac_services/mlme_sap/MLME_SAP.h"
#include "../../mac_services/mlme_sap/SCAN.h"
#include "../../mac_services/mlme_sap/SYNC_LOSS.h"
#include "../../mac_services/pib/MAC_PIB.h"
#include "../../mac_services/pib/PIBHelper.h"
#include "../../mac_services/pib/dsme_mac_constants.h"
#include "../../mac_services/pib/dsme_phy_constants.h"
#include "../../helper/DSMEAtomic.h"
#include "../DSMELayer.h"
#include "../messageDispatcher/MessageDispatcher.h"
#include "../messages/BeaconNotificationCmd.h"
#include "../messages/IEEE802154eMACHeader.h"
#include "../messages/MACCommand.h"
#include "../messages/MOCmd.h"
#include "./MOProtocol.h"

namespace dsme {

BeaconManager::BeaconManager(DSMELayer& dsme)
    : dsme(dsme),

      isBeaconAllocationSent(false),
      isBeaconAllocated(false),
      lastKnownBeaconIntervalStart(0),

      numBeaconCollision(0),
      missedBeacons(0),
      doneCallback(DELEGATE(&BeaconManager::sendDone, *this)),

      currentScanChannel(0),
      scanning(false),
      scanType(ScanType::ED),
      storedMacPANId(0),
      currentScanChannelIndex(0),
      superframesForEachChannel(0),
      superframesLeftForScan(0),
      transmissionPending(false),

      moState(MO_IDLE),
      moTarget(0),
      moOldTarget(0),
      moActivationSymbols(0),
      moDeadlineSymbols(0),
      moEpochSession(0),
      moProposalId(0),
      moNextProposalId(1),
      moLastEpochSession(0),
      moLastEpochSequence(0),
      moParticipantHash(0),
      moRecoveryAttempts(0),
      moRecoveryIntervalBI(1),
      moNextRecoveryQuerySymbols(0),
      moCommandNonce(0),
      moExpectedPeerCount(0),
      moCommitSent(false),
      moAnnounced(false),
      moPreparedLocal(false),
      moCommitDecisionMade(false),
      moLocalApplied(false),
      moRecoveryMode(false) {
    resetMOProtocol();
}

void BeaconManager::initialize() {
    dsmePANDescriptor.getBeaconBitmap().setNumberOfBeaconSlots(dsme.getMAC_PIB().helper.getNumberSuperframesPerBeaconInterval(), false);
    this->dsme.getMAC_PIB().macSdBitmap.setNumberOfBeaconSlots(dsme.getMAC_PIB().helper.getNumberSuperframesPerBeaconInterval(), false);
    neighborOrOwnHeardBeacons.setNumberOfBeaconSlots(dsme.getMAC_PIB().helper.getNumberSuperframesPerBeaconInterval(), false);

    if(this->dsme.getMAC_PIB().macChannelDiversityMode == Channel_Diversity_Mode::CHANNEL_HOPPING) {
        this->dsme.getMAC_PIB().macChannelOffsetBitmapLength = BITVECTOR_BYTE_LENGTH(this->dsme.getMAC_PIB().helper.getNumChannels());
        this->dsme.getMAC_PIB().macChannelOffsetBitmap = new uint8_t[this->dsme.getMAC_PIB().macChannelOffsetBitmapLength];
        dsmePANDescriptor.channelHoppingSpecification.setHoppingSequenceId(dsme.getMAC_PIB().macHoppingSequenceId);
        dsmePANDescriptor.channelHoppingSpecification.setChannelOffsetBitmapLength(MAX_CHANNELS);
    }

    isBeaconAllocated = false;
    isBeaconAllocationSent = false;

    // PAN Coordinator starts network with beacon
    if(dsme.getMAC_PIB().macIsPANCoord) {
        dsmePANDescriptor.getBeaconBitmap().setSDIndex(0);
        dsmePANDescriptor.getBeaconBitmap().fill(false);
        dsmePANDescriptor.getBeaconBitmap().set(0, true);
    } else if(dsme.getMAC_PIB().macIsCoord) {
        dsmePANDescriptor.getBeaconBitmap().fill(false);
    }

    lastKnownBeaconIntervalStart = dsme.getPlatform().getSymbolCounter();
    resetMOProtocol();
}

void BeaconManager::reset() {
    isBeaconAllocated = false;
    isBeaconAllocationSent = false;
    missedBeacons = 0;
    resetMOProtocol();

    if(dsme.getMAC_PIB().macIsPANCoord) {
        dsmePANDescriptor.getBeaconBitmap().setSDIndex(0);
        dsmePANDescriptor.getBeaconBitmap().fill(false);
        dsmePANDescriptor.getBeaconBitmap().set(0, true);
    } else if(dsme.getMAC_PIB().macIsCoord) {
        dsmePANDescriptor.getBeaconBitmap().fill(false);
    }

    lastKnownBeaconIntervalStart = dsme.getPlatform().getSymbolCounter();

    this->dsme.getMAC_PIB().macSdBitmap.fill(false);
    this->neighborOrOwnHeardBeacons.fill(false);
}

void BeaconManager::preSuperframeEvent(uint16_t nextSuperframe, uint16_t nextMultiSuperframe, uint32_t startSlotTime) {
    maybeSendMOCommand(nextSuperframe, nextMultiSuperframe, startSlotTime);
    maybeActivateMO(nextSuperframe, nextMultiSuperframe, startSlotTime);

    uint16_t nextSDIndex = nextSuperframe + this->dsme.getMAC_PIB().helper.getNumberSuperframesPerMultiSuperframe() * nextMultiSuperframe;

    if((this->isBeaconAllocated || this->dsme.getMAC_PIB().macIsPANCoord) && nextSDIndex == this->dsmePANDescriptor.getBeaconBitmap().getSDIndex()) {
        // This node will transmit a beacon
        this->dsme.getPlatform().turnTransceiverOn();
        this->dsme.getPlatform().setChannelNumber(this->dsme.getPHY_PIB().phyCurrentChannel);
        prepareEnhancedBeacon(startSlotTime);
    } else if((!dsme.getMAC_PIB().macAssociatedPANCoord) || nextSDIndex == this->dsme.getMAC_PIB().macSyncParentSdIndex) {
        // This node expects a beacon, only if not associated or a beacon from the SYNC-parent is expected
        this->dsme.getPlatform().turnTransceiverOn();
        this->dsme.getPlatform().setChannelNumber(this->dsme.getPHY_PIB().phyCurrentChannel);
    }
    else {
        this->dsme.getPlatform().turnTransceiverOff();
    }
}

void BeaconManager::superframeEvent(int32_t lateness, uint32_t currentSlotTime) {
    if(transmissionPending) {
        if(lateness > 1) {
            dsme.getAckLayer().abortPreparedTransmission();
            LOG_ERROR("Beacon aborted");
        } else {
            dsme.getAckLayer().sendNowIfPending();
        }

        if(dsme.getMAC_PIB().macIsPANCoord) {
            lastKnownBeaconIntervalStart = currentSlotTime;
        }
    }
}

void BeaconManager::prepareEnhancedBeacon(uint32_t nextSlotTime) {
    DSME_ASSERT(!transmissionPending);
    IDSMEMessage* msg = dsme.getPlatform().getEmptyMessage();

    /* The enhanced beacon carries the live DSME schedule parameters. */
    dsmePANDescriptor.dsmeSuperframeSpec.multiSuperframeOrder = dsme.getMAC_PIB().macMultiSuperframeOrder;
    dsmePANDescriptor.dsmeSuperframeSpec.CAPReductionFlag = dsme.getMAC_PIB().macCapReduction;
    dsmePANDescriptor.dsmeSuperframeSpec.macChannelDiversityMode =
        (dsme.getMAC_PIB().macChannelDiversityMode == Channel_Diversity_Mode::CHANNEL_HOPPING);

    dsmePANDescriptor.getTimeSyncSpec().setBeaconTimestampMicroSeconds(nextSlotTime * aSymbolDuration);
    dsmePANDescriptor.getTimeSyncSpec().setBeaconOffsetTimestampMicroSeconds(0);
    dsmePANDescriptor.prependTo(msg); // TODO this should be implemented as IE

    msg->getHeader().setDstAddr(IEEE802154MacAddress(IEEE802154MacAddress::SHORT_BROADCAST_ADDRESS));
    msg->getHeader().setDstAddrMode(SHORT_ADDRESS);

    msg->getHeader().setSrcAddr(dsme.getMAC_PIB().macExtendedAddress);
    msg->getHeader().setFrameType(IEEE802154eMACHeader::BEACON);

    msg->getHeader().setSrcPANId(this->dsme.getMAC_PIB().macPANId);
    msg->getHeader().setDstPANId(this->dsme.getMAC_PIB().macPANId);

    transmissionPending = true;
    if(!dsme.getAckLayer().prepareSendingCopy(msg, doneCallback)) {
        // message could not be sent
        dsme.getPlatform().releaseMessage(msg);
    } else {
        LOG_DEBUG("Beacon sent");
    }

    return;
}

void BeaconManager::sendEnhancedBeaconRequest() {
    IDSMEMessage* msg = dsme.getPlatform().getEmptyMessage();

    MACCommand cmd;
    cmd.setCmdId(CommandFrameIdentifier::BEACON_REQUEST);
    cmd.prependTo(msg);

    // TODO Header IEEE802.15.4-2012 5.3.7.2.1 p.97
    msg->getHeader().setDstAddrMode(AddrMode::SHORT_ADDRESS);
    msg->getHeader().setDstAddr(IEEE802154MacAddress(IEEE802154MacAddress::SHORT_BROADCAST_ADDRESS));

    msg->getHeader().setSrcAddrMode(AddrMode::EXTENDED_ADDRESS);
    msg->getHeader().setSrcAddr(dsme.getMAC_PIB().macExtendedAddress);

    msg->getHeader().setFrameType(IEEE802154eMACHeader::COMMAND);

    msg->getHeader().setAckRequest(false);

    if(!dsme.getMessageDispatcher().sendInCAP(msg)) {
        // TODO
        dsme.getPlatform().releaseMessage(msg);
    }
}

#ifdef STATISTICS_BEACONS
void BeaconManager::printBeaconStatistics() {
    uint8_t j = statsIdx;
    uint32_t counter = dsme.getPlatform().getSymbolCounter();
    LOG_ERROR("BEACON STATS " << "now " << counter << " coord 0x" << HEXOUT << this->dsme.getMAC_PIB().macCoordShortAddress << DECOUT);

    for(uint8_t i = 0; i < statsValid; i++) {
        auto& stat = beaconStatistics[j];
        LOG_ERROR("BEACON STATS " << stat.time << " 0x" << HEXOUT << stat.sender << DECOUT << " " << (uint16_t)stat.lqi << " " << (int16_t)stat.rssi << " " << (uint16_t)stat.sdIndex);

        uint32_t beaconIntervalSymbols = dsme.getMAC_PIB().helper.getNumberSuperframesPerBeaconInterval();
        beaconIntervalSymbols *= aNumSuperframeSlots;
        beaconIntervalSymbols *= dsme.getMAC_PIB().helper.getSymbolsPerSlot();
        for(uint8_t k = 1; k <= 5; k++) {
            uint32_t nextExpectedBeacon = stat.time+k*beaconIntervalSymbols;
            LOG_ERROR("                      " << nextExpectedBeacon);
        }

        if(j == 0) {
            j = STATS_NUM;
        }
        j--;
    }   
}
#endif

bool BeaconManager::handleEnhancedBeacon(IDSMEMessage* msg, DSMEPANDescriptor& descr) {
#ifdef STATISTICS_BEACONS
    DSME_ATOMIC_BLOCK {
        statsIdx = (statsIdx+1)%STATS_NUM;
        auto& stat = beaconStatistics[statsIdx];
        stat.time = msg->getStartOfFrameDelimiterSymbolCounter();
        stat.sender = msg->getHeader().getSrcAddr().getShortAddress();
        stat.lqi = msg->getLQI();
        stat.rssi = msg->getRSSI();
        stat.sdIndex = descr.getBeaconBitmap().getSDIndex();
        if(statsValid < STATS_NUM) {
            statsValid++;
        }
    }
#endif

    if(dsme.getMAC_PIB().macIsPANCoord) {
        /* '-> This function should not be called for PAN-coordinators */
        DSME_ASSERT(false);
    }

    if(this->scanning) {
        /* '-> Do not handle beacons while scanning */
        LOG_INFO("Currently scanning for PANs -> don't handle as enhanced beacon.");
        return false;
    }

    LOG_DEBUG("Updating heard Beacons, index is " << descr.getBeaconBitmap().getSDIndex() << ".");
    this->dsme.getMAC_PIB().macSdIndex = descr.getBeaconBitmap().getSDIndex();
    this->dsme.getMAC_PIB().macSdBitmap.set(descr.getBeaconBitmap().getSDIndex(), true);
    neighborOrOwnHeardBeacons.set(descr.getBeaconBitmap().getSDIndex(), true);
    neighborOrOwnHeardBeacons.orWith(descr.getBeaconBitmap());

    /* Update channel offset bitmap and channel offset if another neighbor already uses it */
    if(this->dsme.getMAC_PIB().macChannelDiversityMode == Channel_Diversity_Mode::CHANNEL_HOPPING) {
        dsmePANDescriptor.channelHoppingSpecification.getChannelOffsetBitmap().set(descr.channelHoppingSpecification.getChannelOffset(), 1);

        if(descr.channelHoppingSpecification.getChannelOffset() == dsmePANDescriptor.channelHoppingSpecification.getChannelOffset()) {
            /* Find a new channel offset to use if the current one is already used by a neighbor */
            uint16_t rndOffsetIdx = dsme.getPlatform().getRandom() % dsmePANDescriptor.channelHoppingSpecification.getChannelOffsetBitmapLength();
            while(dsmePANDescriptor.channelHoppingSpecification.getChannelOffsetBitmap().get(rndOffsetIdx) == 1) {
                rndOffsetIdx = dsme.getPlatform().getRandom() % dsmePANDescriptor.channelHoppingSpecification.getChannelOffsetBitmapLength();
            }

            dsmePANDescriptor.channelHoppingSpecification.setChannelOffset(rndOffsetIdx);
            dsmePANDescriptor.channelHoppingSpecification.getChannelOffsetBitmap().set(rndOffsetIdx, 1);
            dsme.getMAC_PIB().macChannelOffset = dsmePANDescriptor.channelHoppingSpecification.getChannelOffset();
            LOG_INFO("Duplicate channel offset -> using " << dsme.getMAC_PIB().macChannelOffset << " now");
        }
    }

    if(msg->getHeader().getSrcAddr().getShortAddress() != this->dsme.getMAC_PIB().macSyncParentShortAddress) {
        LOG_DEBUG("Only synchronize to beacons by SYNC parent -> discard");
        return true;
    }

    /* Reset the number of missed beacons */
    this->missedBeacons = 0;

    // TODO do this on lower layer to gain accuracy and include offset in calculation
    uint16_t lastHeardBeaconSDIndex = descr.getBeaconBitmap().getSDIndex();

    // -8 symbols for preamble
    // -2 symbols for SFD
    uint32_t offset = descr.getTimeSyncSpec().getBeaconOffsetTimestampMicroSeconds() / aSymbolDuration;
    lastKnownBeaconIntervalStart = msg->getStartOfFrameDelimiterSymbolCounter() -
                                   lastHeardBeaconSDIndex * aNumSuperframeSlots * dsme.getMAC_PIB().helper.getSymbolsPerSlot() - 8 - 2 - offset;

    /*
     * The PAN coordinator advertises its live MO in the DSME superframe
     * specification. A node that missed the explicit commit command can still
     * recover at a beacon interval boundary, because the beacon is sent at
     * SDIndex 0.
     */
    if(lastHeardBeaconSDIndex == 0) {
        handleBeaconMOAdoption(descr.dsmeSuperframeSpec.multiSuperframeOrder, lastKnownBeaconIntervalStart);
    }

    // Coordinator device request free beacon slots
    LOG_DEBUG("Checking if beacon has to be allocated: "
              << "isCoordinator:" << dsme.getMAC_PIB().macIsCoord << ", isBeaconAllocated:" << isBeaconAllocated
              << ", isBeaconAllocationSent:" << isBeaconAllocationSent << ".");
    if(dsme.getMAC_PIB().macIsCoord && !isBeaconAllocated && !isBeaconAllocationSent && !dsme.getMAC_PIB().macIsPANCoord) {
        if(!(dsme.getMAC_PIB().macAssociatedPANCoord)) {
            LOG_INFO("Device is not associated, cannot reserve BEACON slot.");
        } else {
            // Lookup free slot within all neighbors and broadcast beacon allocation request
            int32_t i = neighborOrOwnHeardBeacons.getRandomFreeSlot(dsme.getPlatform().getRandom());
            if(i >= 0) {
                LOG_INFO("Reserve slot for own BEACON.");
                sendBeaconAllocationNotification(i);
            } else {
                LOG_INFO("No window for a BEACON could be found.");
            }
        }
    }

    return false;
}

void BeaconManager::sendBeaconAllocationNotification(uint16_t beaconSDIndex) {
    LOG_INFO("Attempting to allocate BEACON at index " << beaconSDIndex << ".");
    IDSMEMessage* msg = dsme.getPlatform().getEmptyMessage();

    BeaconNotificationCmd cmd;
    cmd.setBeaconSDIndex(beaconSDIndex);
    cmd.prependTo(msg);

    MACCommand macCmd;
    macCmd.setCmdId(CommandFrameIdentifier::DSME_BEACON_ALLOCATION_NOTIFICATION);
    macCmd.prependTo(msg);

    msg->getHeader().setFrameType(IEEE802154eMACHeader::COMMAND);
    msg->getHeader().setSrcAddr(dsme.getMAC_PIB().macExtendedAddress);
    msg->getHeader().setDstAddr(IEEE802154MacAddress(IEEE802154MacAddress::SHORT_BROADCAST_ADDRESS));
    msg->getHeader().setDstAddrMode(SHORT_ADDRESS);

    msg->getHeader().setSrcPANId(this->dsme.getMAC_PIB().macPANId);
    msg->getHeader().setDstPANId(this->dsme.getMAC_PIB().macPANId);

    msg->getHeader().setAckRequest(false);

    // Update PANDDescription
    dsmePANDescriptor.getBeaconBitmap().setSDIndex(beaconSDIndex);
    dsmePANDescriptor.getBeaconBitmap().copyBitsFrom(this->dsme.getMAC_PIB().macSdBitmap);

    isBeaconAllocationSent = true;

    if(!dsme.getMessageDispatcher().sendInCAP(msg)) {
        isBeaconAllocationSent = false;
        dsme.getPlatform().releaseMessage(msg);
    }
}

void BeaconManager::handleBeaconAllocation(IDSMEMessage* msg) {
    // TODO check if self has sent allocation to same slot -> abort CSMA transmission
    BeaconNotificationCmd beaconAlloc;
    beaconAlloc.decapsulateFrom(msg);

    uint16_t ownBeaconSDIndex = this->dsmePANDescriptor.getBeaconBitmap().getSDIndex();
    uint16_t heardBeaconSDIndex = beaconAlloc.getBeaconSDIndex();

    bool collidesWithOwnBeacon = this->isBeaconAllocated && (ownBeaconSDIndex == heardBeaconSDIndex);
    bool collidesWithHeardBeacon = this->dsme.getMAC_PIB().macSdBitmap.get(heardBeaconSDIndex);

    if(collidesWithOwnBeacon || collidesWithHeardBeacon) {
        sendBeaconCollisionNotification(heardBeaconSDIndex, msg->getHeader().getSrcAddr());
    } else {
        this->dsme.getMAC_PIB().macSdBitmap.set(heardBeaconSDIndex, true);
        this->neighborOrOwnHeardBeacons.set(heardBeaconSDIndex, true);
        // TODO when to remove heardBeacons in case of collision elsewhere?
    }
}

void BeaconManager::handleBeaconRequest(IDSMEMessage* msg) {
    // TODO
    if(!this->dsme.getMAC_PIB().macIsCoord && this->dsme.getMAC_PIB().macAssociatedPANCoord) {
        LOG_INFO("Received BEACON-REQUEST, turning into a coordinator now.");
        this->dsme.getMAC_PIB().macIsCoord = true;
        this->dsme.getPlatform().updateVisual();
    }
}

void BeaconManager::sendBeaconCollisionNotification(uint16_t beaconSDIndex, const IEEE802154MacAddress& addr) {
    LOG_INFO("Informing about BEACON-collision at index " << beaconSDIndex << ".");
    numBeaconCollision++;

    IDSMEMessage* msg = dsme.getPlatform().getEmptyMessage();

    BeaconNotificationCmd cmd;
    cmd.setBeaconSDIndex(beaconSDIndex);
    cmd.prependTo(msg);

    MACCommand macCmd;
    macCmd.setCmdId(CommandFrameIdentifier::DSME_BEACON_COLLISION_NOTIFICATION);
    macCmd.prependTo(msg);

    msg->getHeader().setFrameType(IEEE802154eMACHeader::FrameType::COMMAND);
    msg->getHeader().setDstAddr(addr);
    msg->getHeader().setSrcAddr(dsme.getMAC_PIB().macExtendedAddress);
    msg->getHeader().setAckRequest(true);

    msg->getHeader().setSrcPANId(this->dsme.getMAC_PIB().macPANId);
    msg->getHeader().setDstPANId(this->dsme.getMAC_PIB().macPANId);

    if(!dsme.getMessageDispatcher().sendInCAP(msg)) {
        // TODO
        dsme.getPlatform().releaseMessage(msg);
    }
}

void BeaconManager::handleBeaconCollision(IDSMEMessage* msg) {
    BeaconNotificationCmd cmd;
    cmd.decapsulateFrom(msg);

    LOG_INFO("Got informed about BEACON-collision at index " << cmd.getBeaconSDIndex() << ".");
    isBeaconAllocated = false;
    neighborOrOwnHeardBeacons.set(cmd.getBeaconSDIndex(), true);
}

void BeaconManager::onCSMASent(IDSMEMessage* msg, CommandFrameIdentifier cmdId, DataStatus::Data_Status status, uint8_t numBackoffs) {
    if(cmdId == DSME_BEACON_ALLOCATION_NOTIFICATION && status == DataStatus::SUCCESS) {
        if(dsme.getMAC_PIB().macIsCoord && isBeaconAllocationSent) {
            isBeaconAllocated = true;
            isBeaconAllocationSent = false;
        }
    }

    dsme.getPlatform().releaseMessage(msg);
}

void BeaconManager::sendDone(enum AckLayerResponse result, IDSMEMessage* msg) {
    dsme.getPlatform().releaseMessage(msg);
    transmissionPending = false;
    DSME_ASSERT(result == AckLayerResponse::NO_ACK_REQUESTED || result == AckLayerResponse::SEND_FAILED);
    DSME_SIM_ASSERT(result == AckLayerResponse::NO_ACK_REQUESTED);
}

void BeaconManager::handleBeacon(IDSMEMessage* msg) {
    if(dsme.getMAC_PIB().macIsPANCoord) {
        //* '-> do not handle beacon as PAN coordinator */
        LOG_INFO("A PAN-coordinator does not handle BEACONS -> discard");
        return;
    }

    if((!msg->hasPayload()) && (this->dsme.getMAC_PIB().macAutoRequest)) {
        /* '-> Nothing to do for this beacon */
        LOG_INFO("This BEACON does not have to be indicated -> discard");
        return;
    }

    if(!msg->getHeader().isEnhancedBeacon()) {
        LOG_INFO("We only want enhanced beacons -> discard");
        return;
    }

    /* Data exist or no macAutoRequest -> create indication */

    mlme_sap::BEACON_NOTIFY_indication_parameters params;
    params.panDescriptor.dsmePANDescriptor.decapsulateFrom(msg);

    bool beaconDiscarded = handleEnhancedBeacon(msg, params.panDescriptor.dsmePANDescriptor);

    if(!beaconDiscarded) {
        params.bsn = msg->getHeader().getSequenceNumber();
        params.panDescriptor.coordAddrMode = msg->getHeader().getSrcAddrMode();
        params.panDescriptor.coordPANId = msg->getHeader().getDstPANId();
        params.panDescriptor.coordAddress = msg->getHeader().getSrcAddr();
        params.panDescriptor.channelNumber = this->dsme.getPHY_PIB().phyCurrentChannel;
        params.panDescriptor.channelPage = this->dsme.getPHY_PIB().phyCurrentPage;
        params.panDescriptor.timestamp = msg->getStartOfFrameDelimiterSymbolCounter();
        params.panDescriptor.linkQuality = msg->getLQI();
        params.panDescriptor.rssi = msg->getRSSI();
        //  TODO fill in the other indication_parameters,
        //  some of the info is already included in the PANDesriptor.
        //    params.pendAddrSpec;
        //    params.addrList;
        //    params.sduLength;
        //    params.sdu;
        params.ebsn = msg->getHeader().getSequenceNumber();
        params.beaconType = msg->getHeader().isEnhancedBeacon();

        this->dsme.getMAC_PIB().macPanCoordinatorBsn = params.ebsn;
        this->dsme.getMLME_SAP().getBEACON_NOTIFY().notify_indication(params);
    }

    if(this->scanning) {
        switch(this->scanType) {
            case PASSIVE:
                singleBeaconScanPassiveReceived(params.panDescriptor);
                break;
            case ENHANCEDACTIVESCAN:
                singleBeaconScanEnhancedActiveReceived(params.panDescriptor);
                break;
            default:
                break;
        }
    }
}

bool BeaconManager::isScanning() const {
    return this->scanning;
}

void BeaconManager::setScanDuration(uint16_t scanDuration) {
    /*
     * Calculate time to scan each channel
     *  T_scan = aBaseSuperframeDuration * (2^n + 1) (IEEE 802.15.4e-2012 6.2.10.1)
     *         = 16 * aBaseSlotDuration * (2^n + 1)
     *
     *  T_superframe = 16 * aBaseSlotDuration * 2^(SO)
     *
     *  S = T_scan / T_superframe = (2^n + 1) / (2^(SO))
     *  2^(n-SO) < S < 2^(n-SO)+1
     */
    uint16_t superframes = 1;

    // Scanning less than one superframe does not make any sense
    if(scanDuration > this->dsme.getMAC_PIB().macSuperframeOrder) {
        superframes = (1 << (scanDuration - this->dsme.getMAC_PIB().macSuperframeOrder)) + 1;
    }

    this->superframesForEachChannel = superframes;
}

void BeaconManager::startScanEnhancedActive(uint16_t scanDuration, const channelList_t& scanChannels) {
    DSME_ASSERT(!this->scanning);
    DSME_ASSERT(scanChannels.size() > 0);

    this->scanType = ScanType::ENHANCEDACTIVESCAN;
    this->scanning = true;

    this->scanChannels = scanChannels;
    this->storedMacPANId = this->dsme.getMAC_PIB().macPANId;
    this->dsme.getMAC_PIB().macPANId = 0xffff;

    setScanDuration(scanDuration);

    this->panDescriptorList.clear();

    /*
     * Channels are scanned in order from the lowest channel number to the highest. (IEEE802.15.4-2011 5.1.2.1)
     */
    this->currentScanChannelIndex = 0;

    this->dsme.getPlatform().turnTransceiverOn();
    this->dsme.getPlatform().setChannelNumber(this->scanChannels[this->currentScanChannelIndex]);
    this->sendEnhancedBeaconRequest();
    this->superframesLeftForScan = this->superframesForEachChannel;
    return;
}

void BeaconManager::startScanPassive(uint16_t scanDuration, const channelList_t& scanChannels) {
    DSME_ASSERT(!this->scanning);
    DSME_ASSERT(scanChannels.size() > 0);

    this->scanType = ScanType::PASSIVE;
    this->scanning = true;

    this->scanChannels = scanChannels;
    this->storedMacPANId = this->dsme.getMAC_PIB().macPANId;
    this->dsme.getMAC_PIB().macPANId = 0xffff;

    setScanDuration(scanDuration);

    this->panDescriptorList.clear();

    /*
     * Channels are scanned in order from the lowest channel number to the highest. (IEEE802.15.4-2011 5.1.2.1)
     */
    this->currentScanChannelIndex = 0;

    this->dsme.getPlatform().turnTransceiverOn();
    this->dsme.getPlatform().setChannelNumber(this->scanChannels[this->currentScanChannelIndex]);
    this->superframesLeftForScan = this->superframesForEachChannel;
}

void BeaconManager::handleStartOfCFP(uint16_t currentSuperframe, uint16_t currentMultiSuperframe) {
    if(this->scanning) {
        this->superframesLeftForScan--;

        if(this->superframesLeftForScan == 0) {
            switch(this->scanType) {
                case PASSIVE:
                    channelScanPassiveComplete();
                    break;
                case ENHANCEDACTIVESCAN:
                    channelScanEnhancedActiveComplete();
                    break;
                default:
                    LOG_ERROR("Event during unknown scan!");
                    break;
            }
        }

        this->missedBeacons = 0;
    }

    if(this->dsme.isTrackingBeacons() && currentMultiSuperframe == 0 && currentSuperframe == 0) {
        /* Increment the number of missed beacons. This gets reset whenever a beacon is received */
        ++(this->missedBeacons);
        if(this->missedBeacons > aMaxLostBeacons) {
            mlme_sap::SYNC_LOSS_indication_parameters params;
            MAC_PIB& mac_pip = this->dsme.getMAC_PIB();
            PHY_PIB& phy_pip = this->dsme.getPHY_PIB();

            params.lossReason = LossReason::BEACON_LOST;
            params.panId = mac_pip.macPANId;
            params.channelNumber = phy_pip.phyCurrentChannel;
            params.channelPage = phy_pip.phyCurrentPage;
            params.securityLevel = 0;
            params.keyIdMode = 0;
            params.keySource = nullptr;
            params.keyIndex = 0;

            this->dsme.stopTrackingBeacons();

#ifdef STATISTICS_BEACONS
            printBeaconStatistics();
#endif

            this->dsme.getMLME_SAP().getSYNC_LOSS().notify_indication(params);
        }
    }

    return;
}

uint8_t BeaconManager::getActiveMO() const {
    return dsme.getMAC_PIB().macMultiSuperframeOrder;
}

void BeaconManager::logMOProtocolSnapshot() const {
    const MAC_PIB& pib = dsme.getMAC_PIB();
    const bool coordinator = pib.macIsPANCoord;
    const bool transactionPending = isMOReconfigurationPending();
    const bool activationPending = moCommitDecisionMade && !moLocalApplied;
    const bool releasePending = coordinator
        ? moState == MO_WAITING_RELEASE_ACK
        : moState == MO_QUIESCENT || moState == MO_APPLIED_HOLD;
    uint8_t unreleasedPeers = 0;
    for(uint8_t i = 0; i < moExpectedPeerCount; ++i) {
        if(!moPeerComplete[i]) ++unreleasedPeers;
    }

    const char* stateName = "UNKNOWN";
    switch(moState) {
        case MO_IDLE: stateName = "IDLE"; break;
        case MO_PREPARING: stateName = "PREPARING"; break;
        case MO_COMMIT_DECIDED: stateName = "COMMIT_DECIDED"; break;
        case MO_WAITING_APPLIED: stateName = "WAITING_APPLIED"; break;
        case MO_DEGRADED: stateName = "DEGRADED"; break;
        case MO_WAITING_RELEASE_ACK: stateName = "WAITING_RELEASE_ACK"; break;
        case MO_CLIENT_PREPARED: stateName = "CLIENT_PREPARED"; break;
        case MO_CLIENT_COMMITTED: stateName = "CLIENT_COMMITTED"; break;
        case MO_QUIESCENT: stateName = "QUIESCENT"; break;
        case MO_APPLIED_HOLD: stateName = "APPLIED_HOLD"; break;
        case MO_CLIENT_ACTIVE: stateName = "CLIENT_ACTIVE"; break;
        case MO_QUARANTINED: stateName = "QUARANTINED"; break;
    }

    /* A coordinator resets to IDLE after all RELEASE_ACKs. Participants stay
     * in CLIENT_ACTIVE after receiving RELEASE. In minimal mode, participants
     * correctly remain IDLE because no adaptive transition was requested. */
    const bool stableCoordinator = coordinator && moState == MO_IDLE &&
        moExpectedPeerCount == 0 && !moLocalApplied && !moCommitDecisionMade && !moRecoveryMode;
    const bool stableParticipant = !coordinator &&
        moExpectedPeerCount == 0 && !moRecoveryMode &&
        ((moState == MO_IDLE && !moLocalApplied && !moCommitDecisionMade) ||
         (moState == MO_CLIENT_ACTIVE && moLocalApplied && moCommitDecisionMade));
    const bool terminalOk = !transactionPending && unreleasedPeers == 0 &&
        (stableCoordinator || stableParticipant);

    DSME_LOG("MO_FINAL_STATE_SNAPSHOT node=" << pib.macShortAddress
             << " role=" << (coordinator ? "coordinator" : "participant")
             << " state=" << stateName
             << " active_mo=" << (uint16_t)pib.macMultiSuperframeOrder
             << " target_mo=" << (uint16_t)moTarget
             << " so=" << (uint16_t)pib.macSuperframeOrder
             << " bo=" << (uint16_t)pib.macBeaconOrder
             << " transaction_pending=" << (transactionPending ? 1 : 0)
             << " pending_session=" << (transactionPending ? moEpochSession : 0)
             << " pending_sequence=" << (transactionPending ? moProposalId : 0)
             << " expected_peers=" << (uint16_t)moExpectedPeerCount
             << " unreleased_peers=" << (uint16_t)unreleasedPeers
             << " prepared=" << (moPreparedLocal ? 1 : 0)
             << " committed=" << (moCommitDecisionMade ? 1 : 0)
             << " applied=" << (moLocalApplied ? 1 : 0)
             << " activation_pending=" << (activationPending ? 1 : 0)
             << " activation_symbols=" << moActivationSymbols
             << " release_pending=" << (releasePending ? 1 : 0)
             << " resources_released=" << ((!transactionPending && unreleasedPeers == 0 && !releasePending) ? 1 : 0)
             << " quarantined=" << (moState == MO_QUARANTINED ? 1 : 0)
             << " degraded=" << (moState == MO_DEGRADED ? 1 : 0)
             << " local_applied=" << (moLocalApplied ? 1 : 0)
             << " commit_decision=" << (moCommitDecisionMade ? 1 : 0)
             << " recovery=" << (moRecoveryMode ? 1 : 0)
             << " epoch_sequence=" << moProposalId
             << " terminal_ok=" << (terminalOk ? 1 : 0));
}

void BeaconManager::resetMOProtocol() {
    moState = MO_IDLE;
    moTarget = 0;
    moOldTarget = 0;
    moActivationSymbols = 0;
    moDeadlineSymbols = 0;
    moProposalId = 0;
    moExpectedPeerCount = 0;
    moCommitSent = false;
    moAnnounced = false;
    moPreparedLocal = false;
    moCommitDecisionMade = false;
    moLocalApplied = false;
    moRecoveryMode = false;
    moRecoveryAttempts = 0;
    moRecoveryIntervalBI = 1;
    moNextRecoveryQuerySymbols = 0;
    for(uint8_t i = 0; i < MAX_NEIGHBORS; ++i) {
        moExpectedPeers[i] = 0;
        moPeerReady[i] = false;
        moPeerCommitted[i] = false;
        moPeerApplied[i] = false;
        moPeerComplete[i] = false;
        moPeerStatusReceived[i] = false;
        moPeerQueryNonce[i] = 0;
        moPeerDigest[i] = 0;
        moPeerActualMO[i] = 0;
        moPeerStatus[i] = 0;
        moPeerStatusReceived[i] = false;
        moPeerQueryNonce[i] = 0;
    }
}

uint16_t BeaconManager::getMOCoordinatorAddress() const {
    const MAC_PIB& pib = dsme.getMAC_PIB();
    return pib.macSyncParentShortAddress != IEEE802154MacAddress::SHORT_BROADCAST_ADDRESS
        ? pib.macSyncParentShortAddress : pib.macCoordShortAddress;
}

int8_t BeaconManager::findMOExpectedPeer(uint16_t address) const {
    for(uint8_t i = 0; i < moExpectedPeerCount; ++i) {
        if(moExpectedPeers[i] == address) {
            return static_cast<int8_t>(i);
        }
    }
    return -1;
}

uint32_t BeaconManager::getBeaconIntervalSymbols() const {
    uint32_t symbols = dsme.getMAC_PIB().helper.getNumberSuperframesPerBeaconInterval();
    symbols *= aNumSuperframeSlots;
    symbols *= dsme.getMAC_PIB().helper.getSymbolsPerSlot();
    return symbols;
}

bool BeaconManager::fillMOExpectedPeers() {
    moExpectedPeerCount = 0;
    for(DSMEAllocationCounterTable::iterator it = dsme.getMAC_PIB().macDSMEACT.begin(); it != dsme.getMAC_PIB().macDSMEACT.end(); ++it) {
        const uint16_t address = it->getAddress();
        bool known = false;
        for(uint8_t i = 0; i < moExpectedPeerCount; ++i) {
            if(moExpectedPeers[i] == address) {
                known = true;
                break;
            }
        }
        if(!known) {
            if(moExpectedPeerCount >= MAX_NEIGHBORS) {
                moExpectedPeerCount = 0;
                return false;
            }
            moExpectedPeers[moExpectedPeerCount] = address;
            moPeerReady[moExpectedPeerCount] = false;
            ++moExpectedPeerCount;
        }
    }
    return true;
}

bool BeaconManager::allMOExpectedPeersReady() const {
    for(uint8_t i = 0; i < moExpectedPeerCount; ++i) {
        if(!moPeerReady[i]) {
            return false;
        }
    }
    return true;
}

bool BeaconManager::allMOExpectedPeersCommitted() const {
    for(uint8_t i = 0; i < moExpectedPeerCount; ++i) {
        if(!moPeerCommitted[i]) {
            return false;
        }
    }
    return true;
}

bool BeaconManager::allMOExpectedPeersApplied() const {
    for(uint8_t i = 0; i < moExpectedPeerCount; ++i) {
        if(!moPeerApplied[i]) {
            return false;
        }
    }
    return true;
}

bool BeaconManager::allMOExpectedPeersComplete() const {
    for(uint8_t i = 0; i < moExpectedPeerCount; ++i) {
        if(!moPeerComplete[i]) {
            return false;
        }
    }
    return true;
}

bool BeaconManager::canShrinkTo(uint8_t newMO) const {
    MAC_PIB& pib = dsme.getMAC_PIB();
    if(newMO >= pib.macMultiSuperframeOrder || newMO < pib.macSuperframeOrder ||
       dsme.getGTSManager().hasPendingTransactions()) {
        return false;
    }
    const uint16_t newNumSuperframes = 1 << (uint16_t)(newMO - pib.macSuperframeOrder);

    bool hasAllocationOutsideRange = false;
    for(DSMEAllocationCounterTable::iterator it = pib.macDSMEACT.begin(); it != pib.macDSMEACT.end(); ++it) {
        if(it->getSuperframeID() >= newNumSuperframes) {
            DSME_LOG("MO_SHRINK_BLOCKED node=" << pib.macShortAddress << " target_mo=" << (uint16_t)newMO
                     << " active_entry_peer=" << it->getAddress() << " active_entry_sf=" << it->getSuperframeID()
                     << " reason=allocation_in_removed_superframe");
            hasAllocationOutsideRange = true;
        }
    }

    const uint16_t currentNumSuperframes = pib.helper.getNumberSuperframesPerMultiSuperframe();
    const uint8_t subBlockLengthBytes = pib.helper.getSubBlockLengthBytes();
    bool hasNeighborUseOutsideRange = false;
    for(uint16_t sf = newNumSuperframes; sf < currentNumSuperframes; ++sf) {
        DSMESABSpecification sab;
        sab.setSubBlockLengthBytes(subBlockLengthBytes);
        sab.setSubBlockIndex(sf);
        pib.macDSMESAB.getOccupiedSubBlock(sab, sf);
        if(!sab.getSubBlock().isZero()) {
            DSME_LOG("MO_SHRINK_BLOCKED node=" << pib.macShortAddress << " target_mo=" << (uint16_t)newMO
                     << " sab_sf=" << sf << " reason=neighbor_allocation_in_removed_superframe");
            hasNeighborUseOutsideRange = true;
        }
    }
    return mo_protocol::canShrink(hasAllocationOutsideRange, hasNeighborUseOutsideRange, false);
}

bool BeaconManager::canPrepareMO(uint8_t newMO) const {
    MAC_PIB& pib = dsme.getMAC_PIB();
    if(newMO > MAX_MO || newMO < pib.macSuperframeOrder || newMO > pib.macBeaconOrder) {
        return false;
    }
    const uint16_t superframes = 1U << (newMO - pib.macSuperframeOrder);
    return pib.macDSMEACT.canResizeSuperframes(superframes) &&
           pib.macDSMESAB.canResizeSuperframes(superframes) &&
           (newMO >= pib.macMultiSuperframeOrder || canShrinkTo(newMO));
}

uint32_t BeaconManager::getScheduleDigest() const {
    const MAC_PIB& pib = dsme.getMAC_PIB();
    uint32_t hash = 2166136261UL;
    const uint16_t sfCount = pib.helper.getNumberSuperframesPerMultiSuperframe();
    const uint8_t channelCount = pib.helper.getNumChannels();
    const uint8_t slotCount = MAX_GTSLOTS;
#define MO_HASH_BYTE(v) do { hash ^= static_cast<uint8_t>(v); hash *= 16777619UL; } while(0)
    MO_HASH_BYTE(pib.macMultiSuperframeOrder);
    MO_HASH_BYTE(sfCount);
    for(DSMEAllocationCounterTable::iterator it = const_cast<MAC_PIB&>(pib).macDSMEACT.begin();
        it != const_cast<MAC_PIB&>(pib).macDSMEACT.end(); ++it) {
        MO_HASH_BYTE(it->getSuperframeID());
        MO_HASH_BYTE(it->getGTSlotID());
        MO_HASH_BYTE(it->getChannel());
        MO_HASH_BYTE(it->getDirection());
        MO_HASH_BYTE(it->getAddress());
        MO_HASH_BYTE(it->getState());
    }
    for(uint16_t sf = 0; sf < sfCount; ++sf) {
        DSMESABSpecification sab;
        sab.setSubBlockLengthBytes(pib.helper.getSubBlockLengthBytes());
        sab.setSubBlockIndex(sf);
        pib.macDSMESAB.getOccupiedSubBlock(sab, sf);
        MO_HASH_BYTE(sf);
        for(uint16_t bit = 0; bit < static_cast<uint16_t>(slotCount) * channelCount; ++bit) {
            MO_HASH_BYTE(sab.getSubBlock().get(bit));
        }
    }
#undef MO_HASH_BYTE
    return hash;
}

bool BeaconManager::validateScheduleState() const {
    const MAC_PIB& pib = dsme.getMAC_PIB();
    if(pib.macMultiSuperframeOrder < pib.macSuperframeOrder ||
       pib.macMultiSuperframeOrder > pib.macBeaconOrder ||
       pib.macMultiSuperframeOrder > MAX_MO) return false;
    const uint16_t expectedSuperframes = 1U << (pib.macMultiSuperframeOrder - pib.macSuperframeOrder);
    if(!mo_protocol::scheduleDimensionsMatch(expectedSuperframes,
           pib.macDSMEACT.getNumSuperframesPerMultiSuperframe(),
           pib.macDSMESAB.getNumSuperframesPerMultiSuperframe(),
           pib.helper.getNumberSuperframesPerMultiSuperframe()) ||
       !pib.macDSMEACT.canResizeSuperframes(expectedSuperframes) ||
       !pib.macDSMESAB.canResizeSuperframes(expectedSuperframes)) return false;
    const uint8_t channels = pib.helper.getNumChannels();
    for(DSMEAllocationCounterTable::iterator it = const_cast<MAC_PIB&>(pib).macDSMEACT.begin();
        it != const_cast<MAC_PIB&>(pib).macDSMEACT.end(); ++it) {
        const ACTState state = it->getState();
        if(it->getSuperframeID() >= expectedSuperframes || it->getGTSlotID() >= MAX_GTSLOTS ||
           it->getChannel() >= channels ||
           (it->getDirection() != Direction::TX && it->getDirection() != Direction::RX) ||
           state == ACTState::DEALLOCATED || state == ACTState::REMOVED ||
           !pib.macDSMEACT.isAllocated(it->getSuperframeID(), it->getGTSlotID())) return false;
    }
    for(uint16_t sf = 0; sf < expectedSuperframes; ++sf) {
        DSMESABSpecification sab;
        sab.setSubBlockLengthBytes(pib.helper.getSubBlockLengthBytes());
        sab.setSubBlockIndex(sf);
        pib.macDSMESAB.getOccupiedSubBlock(sab, sf);
        if(sab.getSubBlock().length() != static_cast<uint16_t>(pib.helper.getSubBlockLengthBytes()) * 8) return false;
    }
    return true;
}

uint32_t BeaconManager::getParticipantSetHash() const {
    uint32_t hash = 2166136261UL;
    hash ^= moExpectedPeerCount;
    hash *= 16777619UL;
    for(uint8_t i = 0; i < moExpectedPeerCount; ++i) {
        hash ^= static_cast<uint8_t>(moExpectedPeers[i] & 0xff);
        hash *= 16777619UL;
        hash ^= static_cast<uint8_t>(moExpectedPeers[i] >> 8);
        hash *= 16777619UL;
    }
    return hash;
}

void BeaconManager::sendMOStatus(uint8_t phase, uint16_t destination, const MOCmd* query) {
    if(query == nullptr) {
        sendMOCommand(phase, moTarget, moActivationSymbols, moProposalId, destination);
        return;
    }
    const uint8_t oldTarget = moTarget, oldOldTarget = moOldTarget;
    const uint32_t oldActivation = moActivationSymbols, oldDeadline = moDeadlineSymbols;
    const uint32_t oldSequence = moProposalId, oldSession = moEpochSession, oldHash = moParticipantHash;
    const uint32_t oldNonce = moCommandNonce;
    const bool oldRecovery = moRecoveryMode;
    moTarget = query->getTargetMo(); moOldTarget = query->getOldMo();
    moActivationSymbols = query->getActivationSymbols(); moDeadlineSymbols = query->getDeadlineSymbols();
    moProposalId = query->getEpochSequence(); moEpochSession = query->getEpochSession();
    moParticipantHash = query->getParticipantHash(); moRecoveryMode = query->getRecovery();
    moCommandNonce = query->getQueryNonce();
    sendMOCommand(phase, moTarget, moActivationSymbols, moProposalId, destination);
    moTarget = oldTarget; moOldTarget = oldOldTarget;
    moActivationSymbols = oldActivation; moDeadlineSymbols = oldDeadline;
    moProposalId = oldSequence; moEpochSession = oldSession; moParticipantHash = oldHash;
    moCommandNonce = oldNonce; moRecoveryMode = oldRecovery;
}

bool BeaconManager::applyMultiSuperframeOrder(uint8_t newMO, const char* reason) {
    MAC_PIB& pib = dsme.getMAC_PIB();
    const uint8_t oldMO = pib.macMultiSuperframeOrder;

    if(newMO > MAX_MO || newMO < pib.macSuperframeOrder || newMO > pib.macBeaconOrder) {
        DSME_LOG("MO_ACTIVATION_ABORTED node=" << pib.macShortAddress << " old_mo=" << (uint16_t)oldMO
                 << " new_mo=" << (uint16_t)newMO << " reason=invalid_bounds so=" << (uint16_t)pib.macSuperframeOrder
                 << " bo=" << (uint16_t)pib.macBeaconOrder);
        return false;
    }

    if(newMO == oldMO) {
        const bool valid = validateScheduleState();
        DSME_LOG("MO_ACTIVE_STATE_VERIFIED node=" << pib.macShortAddress << " active_mo=" << (uint16_t)newMO
                 << " schedule_valid=" << valid << " reason=same_mo_integrity_check");
        return valid;
    }

    if(newMO < oldMO && !canShrinkTo(newMO)) {
        DSME_LOG("MO_ACTIVATION_ABORTED node=" << pib.macShortAddress << " old_mo=" << (uint16_t)oldMO
                 << " new_mo=" << (uint16_t)newMO << " reason=shrink_would_lose_allocations");
        return false;
    }

    const uint16_t oldSuperframes = pib.helper.getNumberSuperframesPerMultiSuperframe();
    const uint16_t newSuperframes = 1U << (newMO - pib.macSuperframeOrder);
    if(!pib.macDSMEACT.canResizeSuperframes(newSuperframes) ||
       !pib.macDSMESAB.canResizeSuperframes(newSuperframes)) {
        DSME_LOG("MO_ACTIVATION_ABORTED node=" << pib.macShortAddress << " old_mo=" << (uint16_t)oldMO
                 << " new_mo=" << (uint16_t)newMO << " reason=unprepared_schedule_capacity");
        return false;
    }

    bool actResized = false;
    bool sabResized = false;
    bool rollbackSucceeded = true;
    DSME_ATOMIC_BLOCK {
        actResized = pib.macDSMEACT.resizeSuperframes(newSuperframes);
#if defined(ADAPTIVE_MO_FAIL_APPLY_MOTE)
        static uint8_t applyFaultCount = 0;
#ifndef ADAPTIVE_MO_FAIL_APPLY_COUNT
#define ADAPTIVE_MO_FAIL_APPLY_COUNT 1
#endif
        if(pib.macShortAddress == ADAPTIVE_MO_FAIL_APPLY_MOTE && actResized &&
           applyFaultCount < ADAPTIVE_MO_FAIL_APPLY_COUNT) {
            ++applyFaultCount;
            DSME_LOG("MO_FAULT_INJECT_APPLY_FAILURE node=" << pib.macShortAddress
                     << " stage=after_act_before_sab mo=" << (uint16_t)newMO
                     << " injection=" << (uint16_t)applyFaultCount);
            sabResized = false;
        } else
#endif
        {
            sabResized = pib.macDSMESAB.resizeSuperframes(newSuperframes);
        }
        if(actResized && sabResized) {
            pib.macMultiSuperframeOrder = newMO;
        } else {
            bool actRestored = false;
            bool sabRestored = false;
#if defined(ADAPTIVE_MO_FAIL_ACT_ROLLBACK_MOTE)
            if(pib.macShortAddress == ADAPTIVE_MO_FAIL_ACT_ROLLBACK_MOTE) {
                DSME_LOG("MO_FAULT_INJECT_ROLLBACK_FAILURE node=" << pib.macShortAddress << " structure=ACT");
            } else
#endif
            {
                actRestored = pib.macDSMEACT.resizeSuperframes(oldSuperframes);
            }
#if defined(ADAPTIVE_MO_FAIL_SAB_ROLLBACK_MOTE)
            if(pib.macShortAddress == ADAPTIVE_MO_FAIL_SAB_ROLLBACK_MOTE) {
                DSME_LOG("MO_FAULT_INJECT_ROLLBACK_FAILURE node=" << pib.macShortAddress << " structure=SAB");
            } else
#endif
            {
                sabRestored = pib.macDSMESAB.resizeSuperframes(oldSuperframes);
            }
            rollbackSucceeded = actRestored && sabRestored;
        }
    }
    if(!actResized || !sabResized) {
        DSME_LOG("MO_APPLY_FAILED node=" << pib.macShortAddress << " old_mo=" << (uint16_t)oldMO
                 << " target_mo=" << (uint16_t)newMO << " act=" << actResized << " sab=" << sabResized
                 << " local_rollback=" << rollbackSucceeded << " reason=schedule_resize_failed");
        return false;
    }

    const uint16_t numSuperframes = pib.helper.getNumberSuperframesPerMultiSuperframe();
    DSME_LOG("MO_ACTIVATED node=" << pib.macShortAddress << " old_mo=" << (uint16_t)oldMO
             << " new_mo=" << (uint16_t)newMO << " reason=" << reason
             << " sf_per_msf=" << numSuperframes
             << " msf_per_bi=" << pib.helper.getNumberMultiSuperframesPerBeaconInterval()
             << " bi_symbols=" << getBeaconIntervalSymbols()
             << " apply_call_symbols=" << dsme.getPlatform().getSymbolCounter());
    DSME_LOG("MO_ACTIVE_STATE_VERIFIED node=" << pib.macShortAddress << " active_mo=" << (uint16_t)pib.macMultiSuperframeOrder
             << " so=" << (uint16_t)pib.macSuperframeOrder << " bo=" << (uint16_t)pib.macBeaconOrder
             << " sf_per_msf=" << numSuperframes
             << " msf_per_bi=" << pib.helper.getNumberMultiSuperframesPerBeaconInterval());
    return true;
}

void BeaconManager::sendMOCommand(uint8_t phase, uint8_t targetMo, uint32_t activationSymbols, uint32_t proposalId, uint16_t destination) {
    MAC_PIB& pib = dsme.getMAC_PIB();
    IDSMEMessage* msg = dsme.getPlatform().getEmptyMessage();

    MOCmd moCmd;
    moCmd.setPhase(phase);
    moCmd.setTargetMo(targetMo);
    moCmd.setOldMo(moOldTarget);
    moCmd.setEpochSession(moEpochSession);
    moCmd.setEpochSequence(proposalId);
    moCmd.setActivationSymbols(activationSymbols);
    moCmd.setDeadlineSymbols(moDeadlineSymbols);
    moCmd.setParticipantHash(moParticipantHash);
    moCmd.setActualMo(pib.macMultiSuperframeOrder);
    moCmd.setParticipantState(static_cast<uint8_t>(moState));
    moCmd.setScheduleValid(validateScheduleState());
    moCmd.setRecovery(moRecoveryMode);
    moCmd.setQueryNonce(moCommandNonce);
    moCmd.setScheduleDigest(getScheduleDigest());
    moCmd.prependTo(msg);

    MACCommand cmd;
    cmd.setCmdId(CommandFrameIdentifier::DSME_MO_RECONFIGURATION);
    cmd.prependTo(msg);

    msg->getHeader().setFrameType(IEEE802154eMACHeader::FrameType::COMMAND);
    msg->getHeader().setSrcAddrMode(AddrMode::SHORT_ADDRESS);
    msg->getHeader().setSrcAddr(IEEE802154MacAddress(pib.macShortAddress));
    msg->getHeader().setDstAddrMode(AddrMode::SHORT_ADDRESS);
    msg->getHeader().setDstAddr(IEEE802154MacAddress(destination));
    msg->getHeader().setSrcPANId(pib.macPANId);
    msg->getHeader().setDstPANId(pib.macPANId);
    msg->getHeader().setAckRequest(false);

    if(!dsme.getMessageDispatcher().sendInCAP(msg)) {
        DSME_LOG("MO_SEND_FAILED node=" << pib.macShortAddress << " phase=" << (uint16_t)phase
                 << " new_mo=" << (uint16_t)targetMo << " destination=" << destination);
        dsme.getPlatform().releaseMessage(msg);
    }
}

void BeaconManager::abortMOReconfiguration(const char* reason) {
    MAC_PIB& pib = dsme.getMAC_PIB();
    if(!mo_protocol::mayAbort(moCommitDecisionMade) || moState == MO_COMMIT_DECIDED || moState == MO_WAITING_APPLIED ||
       moState == MO_WAITING_RELEASE_ACK || moState == MO_DEGRADED || moState == MO_QUARANTINED) {
        DSME_LOG("MO_NETWORK_DEGRADED node=" << pib.macShortAddress
                 << " epoch_session=" << moEpochSession << " epoch_sequence=" << moProposalId
                 << " actual_mo=" << (uint16_t)pib.macMultiSuperframeOrder
                 << " target_mo=" << (uint16_t)moTarget << " reason=irrevocable_transaction_" << reason);
        moState = MO_DEGRADED;
        return;
    }

    if(pib.macIsPANCoord && moState != MO_IDLE) {
        sendMOCommand(MO_CMD_ABORT, moTarget, moActivationSymbols, moProposalId,
                      IEEE802154MacAddress::SHORT_BROADCAST_ADDRESS);
    }
    DSME_LOG("MO_PREPARE_ABORTED node=" << pib.macShortAddress
             << " epoch_session=" << moEpochSession << " epoch_sequence=" << moProposalId
             << " target_mo=" << (uint16_t)moTarget << " reason=" << reason);
    resetMOProtocol();
}

bool BeaconManager::startMOReconfiguration(uint8_t newMO, bool recovery) {
    MAC_PIB& pib = dsme.getMAC_PIB();
    if(!pib.macIsPANCoord) return false;
    const bool recoveringState = moState == MO_DEGRADED || moState == MO_QUARANTINED;
    if((recoveringState && !recovery) || (!recoveringState && moState != MO_IDLE) ||
       (recovery && !validateScheduleState())) {
        DSME_LOG("MO_PROPOSAL_DEFERRED node=" << pib.macShortAddress
                 << " target_mo=" << (uint16_t)newMO << " state=" << (uint16_t)moState);
        return false;
    }

    const uint8_t oldMO = pib.macMultiSuperframeOrder;
    if(newMO == oldMO && !recovery) return false;
    if(!canPrepareMO(newMO)) {
        DSME_LOG("MO_PREPARE_FAILED node=" << pib.macShortAddress
                 << " old_mo=" << (uint16_t)oldMO << " target_mo=" << (uint16_t)newMO
                 << " reason=local_schedule_not_preparable");
        return false;
    }

    const uint32_t beaconInterval = getBeaconIntervalSymbols();
    if(beaconInterval == 0) return false;
    const uint32_t now = dsme.getPlatform().getSymbolCounter();
    const uint32_t base = lastKnownBeaconIntervalStart;
    const uint32_t elapsed = now - base;
    if(elapsed >= 0x80000000UL) return false;
    const uint32_t beaconIntervals = elapsed / beaconInterval + MO_ACTIVATION_LEAD_BI;
    const uint32_t activation = base + beaconIntervals * beaconInterval;
    if(!mo_protocol::isFutureWithin(now, activation, 0x7fffffffUL)) return false;

    if(moEpochSession == 0) {
        moEpochSession = now ^ (static_cast<uint32_t>(pib.macPANId) << 16) ^ pib.macShortAddress ^ 0x6d6f7632UL;
        if(moEpochSession == 0) moEpochSession = 1;
    }
    if(moNextProposalId == 0) {
        ++moEpochSession;
        if(moEpochSession == 0) moEpochSession = 1;
        moNextProposalId = 1;
    }
    moProposalId = moNextProposalId++;
    moOldTarget = oldMO;
    moTarget = newMO;
    moActivationSymbols = activation;
    moDeadlineSymbols = activation + 4 * beaconInterval;
    moCommitSent = false;
    moAnnounced = true;
    moPreparedLocal = true;
    moCommitDecisionMade = false;
    moLocalApplied = false;
    moRecoveryMode = recovery;
    moRecoveryAttempts = 0;
    moRecoveryIntervalBI = 1;
    moNextRecoveryQuerySymbols = activation + beaconInterval;
    if(!fillMOExpectedPeers()) {
        DSME_LOG("MO_PREPARE_FAILED node=" << pib.macShortAddress << " reason=participant_set_exceeds_max"
                 << " max_peers=" << (uint16_t)MAX_NEIGHBORS);
        moExpectedPeerCount = 0;
        return false;
    }
    moParticipantHash = getParticipantSetHash();
    for(uint8_t i = 0; i < moExpectedPeerCount; ++i) {
        moPeerReady[i] = false;
        moPeerCommitted[i] = false;
        moPeerApplied[i] = false;
        moPeerComplete[i] = false;
        moPeerStatus[i] = 0;
        moPeerStatusReceived[i] = false;
        moPeerQueryNonce[i] = 0;
        moPeerActualMO[i] = 0;
        moPeerDigest[i] = 0;
    }
    moState = MO_PREPARING;
    DSME_LOG("MO_PREPARE_STARTED node=" << pib.macShortAddress
             << " epoch_session=" << moEpochSession << " epoch_sequence=" << moProposalId
             << " old_mo=" << (uint16_t)oldMO << " target_mo=" << (uint16_t)newMO
             << " activation_symbols=" << activation << " deadline_symbols=" << moDeadlineSymbols
             << " participants_hash=" << moParticipantHash
             << " expected_peers=" << (uint16_t)moExpectedPeerCount
             << " schedule_digest=" << getScheduleDigest());
    sendMOCommand(MO_CMD_PREPARE, moTarget, moActivationSymbols, moProposalId,
                  IEEE802154MacAddress::SHORT_BROADCAST_ADDRESS);
    DSME_LOG("MO_CHANGE_PROPOSED node=" << pib.macShortAddress
             << " old_mo=" << (uint16_t)oldMO << " new_mo=" << (uint16_t)newMO
             << " activation_symbols=" << activation << " epoch_session=" << moEpochSession
             << " epoch_sequence=" << moProposalId << " expected_peers=" << (uint16_t)moExpectedPeerCount);

    if(moExpectedPeerCount == 0) {
        moCommitDecisionMade = true;
        moCommitSent = true;
        moState = MO_COMMIT_DECIDED;
        sendMOCommand(MO_CMD_COMMIT, moTarget, moActivationSymbols, moProposalId,
                      IEEE802154MacAddress::SHORT_BROADCAST_ADDRESS);
        DSME_LOG("MO_COMMIT_DECIDED node=" << pib.macShortAddress
                 << " epoch_session=" << moEpochSession << " epoch_sequence=" << moProposalId
                 << " expected_peers=0");
    }
    return true;
}

void BeaconManager::handleMOCommand(IDSMEMessage* msg) {
    MOCmd command;
    command.decapsulateFrom(msg);
    MAC_PIB& pib = dsme.getMAC_PIB();
    const IEEE802154eMACHeader& header = msg->getHeader();
    if(header.getSrcAddrMode() != AddrMode::SHORT_ADDRESS || header.getSrcPANId() != pib.macPANId) {
        DSME_LOG("MO_COMMAND_IGNORED node=" << pib.macShortAddress << " reason=invalid_source_or_pan");
        return;
    }
    const uint16_t source = header.getSrcAddr().getShortAddress();
    const uint8_t phase = command.getPhase();
    const uint8_t target = command.getTargetMo();
    const uint8_t oldMO = command.getOldMo();
    const uint32_t session = command.getEpochSession();
    const uint32_t sequence = command.getEpochSequence();
    const uint32_t activation = command.getActivationSymbols();
    const uint32_t deadline = command.getDeadlineSymbols();
    const uint32_t participantHash = command.getParticipantHash();
    const uint32_t now = dsme.getPlatform().getSymbolCounter();
    const bool validTarget = target <= MAX_MO && target >= pib.macSuperframeOrder && target <= pib.macBeaconOrder;
    const bool future = mo_protocol::isFutureWithin(now, activation, 0x7fffffffUL);
    const bool validDeadline = deadline == activation + 4 * getBeaconIntervalSymbols();
    /* Test-only receive-path drop: an actual decoded command is withheld from state handling. */
#if defined(ADAPTIVE_MO_FAULT_PHASE) && defined(ADAPTIVE_MO_FAULT_PEER)
    static uint16_t injectedDrops = 0;
#ifndef ADAPTIVE_MO_FAULT_COUNT
#define ADAPTIVE_MO_FAULT_COUNT 255
#endif
    if(phase == ADAPTIVE_MO_FAULT_PHASE && source == ADAPTIVE_MO_FAULT_PEER &&
       injectedDrops < ADAPTIVE_MO_FAULT_COUNT) {
        ++injectedDrops;
        DSME_LOG("MO_FAULT_INJECT_DROP node=" << pib.macShortAddress << " peer=" << source
                 << " phase=" << (uint16_t)phase << " epoch_session=" << session
                 << " epoch_sequence=" << sequence << " target_mo=" << (uint16_t)target
                 << " activation_symbols=" << activation << " drop_index=" << injectedDrops);
        return;
    }
#endif

    if(pib.macIsPANCoord) {
        const int8_t peer = findMOExpectedPeer(source);
        if(peer < 0 || !validTarget || participantHash != moParticipantHash ||
           session != moEpochSession || sequence != moProposalId || target != moTarget ||
           activation != moActivationSymbols || command.getRecovery() != moRecoveryMode) {
            DSME_LOG("MO_COORD_COMMAND_IGNORED node=" << pib.macShortAddress << " peer=" << source
                     << " phase=" << (uint16_t)phase << " epoch_session=" << session
                     << " epoch_sequence=" << sequence << " reason=peer_or_epoch_mismatch");
            return;
        }
        if(phase == MO_CMD_READY && moState == MO_PREPARING && future && validDeadline) {
            if(command.getParticipantState() != MO_CLIENT_PREPARED ||
               !mo_protocol::readyMatchesExpected(moRecoveryMode, moOldTarget,
                                                  command.getActualMo(), true,
                                                  command.getScheduleValid())) {
                DSME_LOG("MO_READY_REJECTED node=" << pib.macShortAddress << " peer=" << source
                         << " expected_old_mo=" << (uint16_t)moOldTarget
                         << " reported_mo=" << (uint16_t)command.getActualMo()
                         << " participant_state=" << (uint16_t)command.getParticipantState()
                         << " reason=peer_not_prepared_at_expected_mo");
                sendMOCommand(MO_CMD_ABORT, moTarget, moActivationSymbols, moProposalId,
                              IEEE802154MacAddress::SHORT_BROADCAST_ADDRESS);
                moState = MO_DEGRADED;
                return;
            }
            moPeerReady[peer] = true;
            moPeerActualMO[peer] = command.getActualMo();
            moPeerDigest[peer] = command.getScheduleDigest();
            moPeerStatus[peer] = command.getParticipantState();
            DSME_LOG("MO_PREPARED node=" << pib.macShortAddress << " peer=" << source
                     << " epoch_session=" << session << " epoch_sequence=" << sequence
                     << " old_mo=" << (uint16_t)oldMO << " target_mo=" << (uint16_t)target
                     << " schedule_digest=" << moPeerDigest[peer]);
            if(allMOExpectedPeersReady()) {
                /* The durable-in-memory commit decision is recorded before COMMIT transmission. */
                moCommitDecisionMade = true;
                moCommitSent = true;
                moState = MO_COMMIT_DECIDED;
                DSME_LOG("MO_COMMIT_DECIDED node=" << pib.macShortAddress
                         << " epoch_session=" << session << " epoch_sequence=" << sequence
                         << " target_mo=" << (uint16_t)target << " activation_symbols=" << activation);
                sendMOCommand(MO_CMD_COMMIT, target, activation, sequence,
                              IEEE802154MacAddress::SHORT_BROADCAST_ADDRESS);
                DSME_LOG("MO_COMMIT_SENT node=" << pib.macShortAddress
                         << " epoch_session=" << session << " epoch_sequence=" << sequence);
                if(moExpectedPeerCount == 0) {
                    moState = MO_WAITING_APPLIED;
                    maybeActivateMO(0, 0, activation);
                }
            }
            return;
        }
        if(phase == MO_CMD_COMMIT_ACK && moCommitDecisionMade) {
            moPeerCommitted[peer] = true;
            DSME_LOG("MO_COMMIT_ACK_RECEIVED node=" << pib.macShortAddress << " peer=" << source
                     << " epoch_session=" << session << " epoch_sequence=" << sequence
                     << " ack_is_not_application_proof=1");
            return;
        }
        if(phase == MO_CMD_APPLIED && (moState == MO_WAITING_APPLIED || moState == MO_DEGRADED)) {
            if(!mo_protocol::canReleaseParticipant(true, target, command.getActualMo(),
                                                   command.getScheduleValid(),
                                                   command.getParticipantState() == MO_APPLIED_HOLD)) {
                DSME_LOG("MO_APPLIED_REJECTED node=" << pib.macShortAddress << " peer=" << source
                         << " actual_mo=" << (uint16_t)command.getActualMo()
                         << " participant_state=" << (uint16_t)command.getParticipantState());
                return;
            }
            moPeerApplied[peer] = true;
            moPeerActualMO[peer] = command.getActualMo();
            moPeerDigest[peer] = command.getScheduleDigest();
            moPeerStatus[peer] = command.getParticipantState();
            DSME_LOG("MO_APPLIED node=" << pib.macShortAddress << " peer=" << source
                     << " epoch_session=" << session << " epoch_sequence=" << sequence
                     << " actual_mo=" << (uint16_t)command.getActualMo()
                     << " schedule_digest=" << command.getScheduleDigest());
            if(moLocalApplied && allMOExpectedPeersApplied()) {
                moState = MO_WAITING_RELEASE_ACK;
                sendMOCommand(MO_CMD_RELEASE, target, activation, sequence,
                              IEEE802154MacAddress::SHORT_BROADCAST_ADDRESS);
                DSME_LOG("MO_ACTIVATION_RELEASED node=" << pib.macShortAddress
                         << " epoch_session=" << session << " epoch_sequence=" << sequence
                         << " target_mo=" << (uint16_t)pib.macMultiSuperframeOrder);
                if(moExpectedPeerCount == 0) {
                    DSME_LOG("MO_NETWORK_ACTIVE node=" << pib.macShortAddress
                             << " epoch_session=" << session << " epoch_sequence=" << sequence
                             << " active_mo=" << (uint16_t)pib.macMultiSuperframeOrder);
                    resetMOProtocol();
                }
            }
            return;
        }
        if(phase == MO_CMD_STATUS_RESPONSE &&
           (moState == MO_WAITING_APPLIED || moState == MO_DEGRADED || moState == MO_WAITING_RELEASE_ACK) &&
           mo_protocol::queryNonceMatches(command.getQueryNonce(), moPeerQueryNonce[peer])) {
            moPeerStatus[peer] = command.getParticipantState();
            moPeerStatusReceived[peer] = true;
            moPeerActualMO[peer] = command.getActualMo();
            moPeerDigest[peer] = command.getScheduleDigest();
            if(mo_protocol::canReleaseParticipant(true, pib.macMultiSuperframeOrder,
                                                   command.getActualMo(), command.getScheduleValid(),
                                                   command.getParticipantState() == MO_APPLIED_HOLD ||
                                                   command.getParticipantState() == MO_CLIENT_ACTIVE)) {
                moPeerApplied[peer] = true;
            } else {
                moPeerApplied[peer] = false;
            }
            DSME_LOG("MO_STATUS_RESPONSE node=" << pib.macShortAddress << " peer=" << source
                     << " epoch_session=" << session << " epoch_sequence=" << sequence
                     << " actual_mo=" << (uint16_t)command.getActualMo()
                     << " participant_state=" << (uint16_t)command.getParticipantState()
                     << " schedule_digest=" << command.getScheduleDigest());
            if(allMOExpectedPeersApplied() && moLocalApplied && moState != MO_WAITING_RELEASE_ACK) {
                moState = MO_WAITING_RELEASE_ACK;
                sendMOCommand(MO_CMD_RELEASE, moTarget, moActivationSymbols, moProposalId,
                              IEEE802154MacAddress::SHORT_BROADCAST_ADDRESS);
            } else if(moState == MO_DEGRADED && !moCommitDecisionMade && allMOExpectedPeersApplied()) {
                bool allPeersOperational = true;
                for(uint8_t i = 0; i < moExpectedPeerCount; ++i) {
                    allPeersOperational = allPeersOperational &&
                        (moPeerStatus[i] == MO_CLIENT_ACTIVE || moPeerStatus[i] == MO_IDLE);
                }
                if(allPeersOperational) {
                    DSME_LOG("MO_RECONCILIATION_COMPLETED node=" << pib.macShortAddress
                             << " epoch_session=" << session << " epoch_sequence=" << sequence
                             << " result=precommit_abort_state_verified active_mo="
                             << (uint16_t)pib.macMultiSuperframeOrder);
                    resetMOProtocol();
                    return;
                }
            } else if(moState == MO_DEGRADED) {
                bool allStatus = true;
                for(uint8_t i = 0; i < moExpectedPeerCount; ++i) allStatus = allStatus && moPeerStatusReceived[i];
                if(allStatus && !allMOExpectedPeersApplied()) {
                    const uint8_t reconcileTarget = pib.macMultiSuperframeOrder;
                    DSME_LOG("MO_RECONCILIATION_STARTED node=" << pib.macShortAddress
                             << " epoch_session=" << session << " epoch_sequence=" << sequence
                             << " coordinator_mo=" << (uint16_t)reconcileTarget);
                    startMOReconfiguration(reconcileTarget, true);
                }
            }
            return;
        }
        if(phase == MO_CMD_RELEASE_ACK && moState == MO_WAITING_RELEASE_ACK) {
            moPeerComplete[peer] = true;
            if(allMOExpectedPeersComplete()) {
                DSME_LOG("MO_NETWORK_ACTIVE node=" << pib.macShortAddress
                         << " epoch_session=" << session << " epoch_sequence=" << sequence
                         << " active_mo=" << (uint16_t)pib.macMultiSuperframeOrder
                         << " released_peers=" << (uint16_t)moExpectedPeerCount);
                resetMOProtocol();
            }
            return;
        }
        if(phase == MO_CMD_REJECT && moState == MO_PREPARING) {
            DSME_LOG("MO_PREPARE_REJECTED node=" << pib.macShortAddress << " peer=" << source
                     << " epoch_session=" << session << " epoch_sequence=" << sequence);
            moState = MO_DEGRADED;
            return;
        }
        return;
    }

    const uint16_t coordinator = getMOCoordinatorAddress();
    if(!mo_protocol::isExpectedCoordinator(source, coordinator, header.getSrcPANId(), pib.macPANId)) {
        DSME_LOG("MO_CLIENT_COMMAND_IGNORED node=" << pib.macShortAddress << " source=" << source
                 << " phase=" << (uint16_t)phase << " reason=wrong_coordinator");
        return;
    }

    if(phase == MO_CMD_PREPARE) {
        const bool duplicate = session == moEpochSession && sequence == moProposalId &&
                               target == moTarget && activation == moActivationSymbols &&
                               participantHash == moParticipantHash && command.getRecovery() == moRecoveryMode;
        const bool normalState = moState == MO_IDLE || moState == MO_CLIENT_ACTIVE;
        const bool recoverableState = moState == MO_QUIESCENT || moState == MO_APPLIED_HOLD ||
            moState == MO_QUARANTINED || (moState == MO_CLIENT_PREPARED && !moCommitDecisionMade);
        const bool recoveryState = mo_protocol::canAcceptRecovery(command.getRecovery(), recoverableState,
                                                                   validateScheduleState());
        const bool allowedState = normalState || recoveryState || duplicate;
        const bool stale = session == moLastEpochSession && sequence <= moLastEpochSequence && !duplicate;
        if(!validTarget || !future || !validDeadline || participantHash == 0 || !allowedState || stale ||
           (!duplicate && ((!command.getRecovery() && oldMO != pib.macMultiSuperframeOrder) ||
                           !validateScheduleState() || !canPrepareMO(target)))) {
            DSME_LOG("MO_PREPARE_REJECTED node=" << pib.macShortAddress << " coordinator=" << source
                     << " epoch_session=" << session << " epoch_sequence=" << sequence
                     << " target_mo=" << (uint16_t)target << " active_mo=" << (uint16_t)pib.macMultiSuperframeOrder
                     << " reason=" << (stale ? "stale_epoch" : (!allowedState ? "busy_state" : "not_preparable")));
            if(moState == MO_IDLE || moState == MO_CLIENT_ACTIVE) {
                moEpochSession = session; moProposalId = sequence; moTarget = target;
                moActivationSymbols = activation; moDeadlineSymbols = deadline;
                moParticipantHash = participantHash; moOldTarget = oldMO;
                sendMOCommand(MO_CMD_REJECT, target, activation, sequence, coordinator);
                resetMOProtocol();
            }
            return;
        }
        if(!duplicate) {
            moOldTarget = oldMO;
            moTarget = target;
            moEpochSession = session;
            moProposalId = sequence;
            moActivationSymbols = activation;
            moDeadlineSymbols = deadline;
            moParticipantHash = participantHash;
            moRecoveryMode = command.getRecovery();
            moPreparedLocal = true;
            moCommitDecisionMade = false;
            moLocalApplied = false;
            moState = MO_CLIENT_PREPARED;
            moLastEpochSession = session;
            moLastEpochSequence = sequence;
        }
        DSME_LOG("MO_PREPARED node=" << pib.macShortAddress << " peer=" << source
                 << " epoch_session=" << session << " epoch_sequence=" << sequence
                 << " old_mo=" << (uint16_t)pib.macMultiSuperframeOrder
                 << " target_mo=" << (uint16_t)target << " activation_symbols=" << activation
                 << " schedule_digest=" << getScheduleDigest());
        sendMOCommand(MO_CMD_READY, target, activation, sequence, coordinator);
        return;
    }

    const bool clientMatch = session == moEpochSession && sequence == moProposalId && target == moTarget &&
                             activation == moActivationSymbols && participantHash == moParticipantHash &&
                             command.getRecovery() == moRecoveryMode;
    if(phase == MO_CMD_COMMIT && clientMatch && validTarget &&
       (moState == MO_CLIENT_PREPARED || moState == MO_CLIENT_COMMITTED) && future) {
        moState = MO_CLIENT_COMMITTED;
        moCommitDecisionMade = true;
        DSME_LOG("MO_COMMIT_RECEIVED node=" << pib.macShortAddress << " coordinator=" << source
                 << " epoch_session=" << session << " epoch_sequence=" << sequence
                 << " target_mo=" << (uint16_t)target << " activation_symbols=" << activation);
        sendMOCommand(MO_CMD_COMMIT_ACK, target, activation, sequence, coordinator);
        return;
    }
    if(phase == MO_CMD_COMMIT && clientMatch && moState == MO_CLIENT_COMMITTED) {
        sendMOCommand(MO_CMD_COMMIT_ACK, target, activation, sequence, coordinator);
        return;
    }
    if(phase == MO_CMD_ABORT && clientMatch && moState == MO_CLIENT_PREPARED) {
        DSME_LOG("MO_PREPARE_ABORTED node=" << pib.macShortAddress
                 << " epoch_session=" << session << " epoch_sequence=" << sequence);
        resetMOProtocol();
        return;
    }
    if(phase == MO_CMD_RELEASE && clientMatch &&
       (moState == MO_APPLIED_HOLD || moState == MO_CLIENT_ACTIVE)) {
        moState = MO_CLIENT_ACTIVE;
        DSME_LOG("MO_ACTIVATION_RELEASED node=" << pib.macShortAddress
                 << " epoch_session=" << session << " epoch_sequence=" << sequence
                 << " actual_mo=" << (uint16_t)pib.macMultiSuperframeOrder);
        sendMOCommand(MO_CMD_RELEASE_ACK, target, activation, sequence, coordinator);
        return;
    }
    if(phase == MO_CMD_STATUS_QUERY && command.getQueryNonce() != 0) {
        DSME_LOG("MO_STATUS_QUERY node=" << pib.macShortAddress << " peer=" << source
                 << " epoch_session=" << session << " epoch_sequence=" << sequence
                 << " matching_local_tx=" << clientMatch << " query_nonce=" << command.getQueryNonce());
        sendMOStatus(MO_CMD_STATUS_RESPONSE, coordinator, &command);
    }
}

void BeaconManager::maybeSendMOCommand(uint16_t nextSuperframe, uint16_t nextMultiSuperframe, uint32_t startSlotTime) {
    (void)nextMultiSuperframe;
    if(nextSuperframe != 0) return;
    MAC_PIB& pib = dsme.getMAC_PIB();
    const uint32_t bi = getBeaconIntervalSymbols();
    const bool due = mo_protocol::timeReached(startSlotTime, moActivationSymbols);

    if(pib.macIsPANCoord) {
        if(moState == MO_PREPARING && !due) {
            sendMOCommand(MO_CMD_PREPARE, moTarget, moActivationSymbols, moProposalId,
                          IEEE802154MacAddress::SHORT_BROADCAST_ADDRESS);
        } else if(moState == MO_COMMIT_DECIDED && !due) {
            /* COMMIT is irrevocable. Retransmission is idempotent; missing ACK never aborts. */
            sendMOCommand(MO_CMD_COMMIT, moTarget, moActivationSymbols, moProposalId,
                          IEEE802154MacAddress::SHORT_BROADCAST_ADDRESS);
        } else if(moState == MO_WAITING_APPLIED || moState == MO_DEGRADED) {
            if(moState == MO_WAITING_APPLIED && mo_protocol::recoveryDeadlineReached(startSlotTime, moDeadlineSymbols)) {
                moState = MO_DEGRADED;
                moNextRecoveryQuerySymbols = startSlotTime;
                DSME_LOG("MO_NETWORK_DEGRADED node=" << pib.macShortAddress
                         << " epoch_session=" << moEpochSession << " epoch_sequence=" << moProposalId
                         << " actual_mo=" << (uint16_t)pib.macMultiSuperframeOrder
                         << " target_mo=" << (uint16_t)moTarget << " reason=applied_deadline_expired");
            }
            if(mo_protocol::timeReached(startSlotTime, moNextRecoveryQuerySymbols)) {
                for(uint8_t i = 0; i < moExpectedPeerCount; ++i) {
                    ++moCommandNonce;
                    if(moCommandNonce == 0) ++moCommandNonce;
                    moPeerQueryNonce[i] = moCommandNonce;
                    sendMOCommand(MO_CMD_STATUS_QUERY, moTarget, moActivationSymbols, moProposalId, moExpectedPeers[i]);
                    DSME_LOG("MO_STATUS_QUERY node=" << pib.macShortAddress << " peer=" << moExpectedPeers[i]
                             << " epoch_session=" << moEpochSession << " epoch_sequence=" << moProposalId
                             << " attempt=" << (uint16_t)(moRecoveryAttempts + 1)
                             << " interval_bi=" << (uint16_t)moRecoveryIntervalBI);
                }
                if(moRecoveryAttempts < 255) ++moRecoveryAttempts;
                moNextRecoveryQuerySymbols = startSlotTime + moRecoveryIntervalBI * bi;
                moRecoveryIntervalBI = mo_protocol::nextBackoffBI(moRecoveryIntervalBI, 16);
            }
        } else if(moState == MO_WAITING_RELEASE_ACK) {
            sendMOCommand(MO_CMD_RELEASE, moTarget, moActivationSymbols, moProposalId,
                          IEEE802154MacAddress::SHORT_BROADCAST_ADDRESS);
        }
        (void)bi;
    } else {
        const uint16_t coordinator = getMOCoordinatorAddress();
        if(moState == MO_CLIENT_PREPARED && !due) {
            sendMOCommand(MO_CMD_READY, moTarget, moActivationSymbols, moProposalId, coordinator);
        } else if(moState == MO_CLIENT_COMMITTED && !due) {
            sendMOCommand(MO_CMD_COMMIT_ACK, moTarget, moActivationSymbols, moProposalId, coordinator);
        } else if(moState == MO_APPLIED_HOLD) {
            sendMOCommand(MO_CMD_APPLIED, moTarget, moActivationSymbols, moProposalId, coordinator);
        } else if(moState == MO_CLIENT_ACTIVE) {
            sendMOCommand(MO_CMD_RELEASE_ACK, moTarget, moActivationSymbols, moProposalId, coordinator);
        }
    }
}

void BeaconManager::maybeActivateMO(uint16_t nextSuperframe, uint16_t nextMultiSuperframe, uint32_t startSlotTime) {
    if(moState == MO_IDLE || nextSuperframe != 0 || nextMultiSuperframe != 0 ||
       !mo_protocol::timeReached(startSlotTime, moActivationSymbols)) return;

    MAC_PIB& pib = dsme.getMAC_PIB();
    const uint8_t target = moTarget;
    const uint32_t activation = moActivationSymbols;

    if(pib.macIsPANCoord) {
        if(moState == MO_COMMIT_DECIDED && moCommitDecisionMade) {
            DSME_LOG("MO_ACTIVATION_BOUNDARY node=" << pib.macShortAddress
                     << " epoch_session=" << moEpochSession << " epoch_sequence=" << moProposalId
                     << " old_mo=" << (uint16_t)pib.macMultiSuperframeOrder << " target_mo=" << (uint16_t)target
                     << " activation_symbols=" << activation << " actual_boundary=" << startSlotTime);
            if(applyMultiSuperframeOrder(target, "adaptive_protocol")) {
                moLocalApplied = true;
                moState = MO_WAITING_APPLIED;
                moNextRecoveryQuerySymbols = activation + getBeaconIntervalSymbols();
                DSME_LOG("MO_APPLY_SUCCEEDED node=" << pib.macShortAddress << " epoch_session=" << moEpochSession
                         << " epoch_sequence=" << moProposalId << " actual_mo=" << (uint16_t)pib.macMultiSuperframeOrder
                         << " schedule_digest=" << getScheduleDigest());
                if(moExpectedPeerCount == 0) {
                    moState = MO_WAITING_RELEASE_ACK;
                    DSME_LOG("MO_NETWORK_ACTIVE node=" << pib.macShortAddress << " epoch_session=" << moEpochSession
                             << " epoch_sequence=" << moProposalId << " active_mo=" << (uint16_t)pib.macMultiSuperframeOrder);
                    resetMOProtocol();
                }
            } else if(validateScheduleState()) {
                moState = MO_DEGRADED;
                moNextRecoveryQuerySymbols = startSlotTime;
                DSME_LOG("MO_NETWORK_DEGRADED node=" << pib.macShortAddress
                         << " epoch_session=" << moEpochSession << " epoch_sequence=" << moProposalId
                         << " actual_mo=" << (uint16_t)pib.macMultiSuperframeOrder
                         << " reason=coordinator_apply_failed_local_schedule_verified");
            } else {
                moState = MO_QUARANTINED;
                DSME_LOG("MO_PARTICIPANT_QUARANTINED node=" << pib.macShortAddress
                         << " epoch_session=" << moEpochSession << " epoch_sequence=" << moProposalId
                         << " actual_mo=" << (uint16_t)pib.macMultiSuperframeOrder
                         << " reason=coordinator_apply_failed_schedule_invalid");
            }
        } else if(moState == MO_PREPARING) {
            /* Participants that prepared but missed COMMIT fence themselves at this boundary. */
            moState = MO_DEGRADED;
            DSME_LOG("MO_NETWORK_DEGRADED node=" << pib.macShortAddress
                     << " epoch_session=" << moEpochSession << " epoch_sequence=" << moProposalId
                     << " actual_mo=" << (uint16_t)pib.macMultiSuperframeOrder << " target_mo=" << (uint16_t)target
                     << " reason=prepare_deadline_without_commit");
        }
    } else if(moState == MO_CLIENT_COMMITTED || moState == MO_CLIENT_PREPARED) {
        DSME_LOG("MO_ACTIVATION_BOUNDARY node=" << pib.macShortAddress
                 << " epoch_session=" << moEpochSession << " epoch_sequence=" << moProposalId
                 << " old_mo=" << (uint16_t)pib.macMultiSuperframeOrder << " target_mo=" << (uint16_t)target
                 << " activation_symbols=" << activation << " actual_boundary=" << startSlotTime);
        const mo_protocol::BoundaryAction boundaryAction = mo_protocol::participantBoundaryAction(
            moState == MO_CLIENT_PREPARED || moState == MO_CLIENT_COMMITTED,
            moState == MO_CLIENT_COMMITTED);
        if(boundaryAction == mo_protocol::BOUNDARY_QUIESCE) {
            moState = MO_QUIESCENT;
            DSME_LOG("MO_QUIESCENT node=" << pib.macShortAddress << " epoch_session=" << moEpochSession
                     << " epoch_sequence=" << moProposalId << " actual_mo=" << (uint16_t)pib.macMultiSuperframeOrder
                     << " reason=commit_not_received");
        } else if(applyMultiSuperframeOrder(target, "adaptive_protocol")) {
            moLocalApplied = true;
            moState = MO_APPLIED_HOLD;
            moLastEpochSession = moEpochSession;
            moLastEpochSequence = moProposalId;
            DSME_LOG("MO_APPLY_SUCCEEDED node=" << pib.macShortAddress << " epoch_session=" << moEpochSession
                     << " epoch_sequence=" << moProposalId << " actual_mo=" << (uint16_t)pib.macMultiSuperframeOrder
                     << " schedule_digest=" << getScheduleDigest());
            sendMOCommand(MO_CMD_APPLIED, target, activation, moProposalId, getMOCoordinatorAddress());
        } else {
            moState = MO_QUARANTINED;
            DSME_LOG("MO_APPLY_FAILED node=" << pib.macShortAddress << " epoch_session=" << moEpochSession
                     << " epoch_sequence=" << moProposalId << " actual_mo=" << (uint16_t)pib.macMultiSuperframeOrder
                     << " reason=client_local_application_failed");
            sendMOCommand(MO_CMD_REJECT, target, activation, moProposalId, getMOCoordinatorAddress());
            DSME_LOG("MO_PARTICIPANT_QUARANTINED node=" << pib.macShortAddress
                     << " epoch_session=" << moEpochSession << " epoch_sequence=" << moProposalId);
        }
    }
}

void BeaconManager::handleBeaconMOAdoption(uint8_t advertisedMO, uint32_t beaconStartSymbols) {
    MAC_PIB& pib = dsme.getMAC_PIB();
    if(advertisedMO > MAX_MO || advertisedMO < pib.macSuperframeOrder || advertisedMO > pib.macBeaconOrder) {
        return;
    }
    if(advertisedMO == pib.macMultiSuperframeOrder) {
        return;
    }

    if(moState == MO_COMMITTED && advertisedMO == moTarget &&
       mo_protocol::timeReached(beaconStartSymbols, moActivationSymbols)) {
        return;
    }
    DSME_LOG("MO_BEACON_MISMATCH node=" << pib.macShortAddress
             << " active_mo=" << (uint16_t)pib.macMultiSuperframeOrder
             << " advertised_mo=" << (uint16_t)advertisedMO
             << " pending_mo=" << (uint16_t)moTarget
             << " beacon_start=" << beaconStartSymbols << " reason=beacon_is_not_commit");
}

void BeaconManager::scanCurrentChannel() {
    this->dsme.getPlatform().setChannelNumber(this->currentScanChannel);

    sendEnhancedBeaconRequest();

    this->superframesLeftForScan = this->superframesForEachChannel;
    return;
}

void BeaconManager::channelScanPassiveComplete() {
    DSME_ASSERT(this->scanning);

    LOG_INFO("Scan complete, chan " << (uint16_t)this->scanChannels[this->currentScanChannelIndex]);
    if(this->panDescriptorList.full() || this->currentScanChannelIndex >= this->scanChannels.size() - 1) {
        this->scanning = false;
        this->dsme.getMAC_PIB().macPANId = this->storedMacPANId;

        mlme_sap::SCAN_confirm_parameters params;

        if(this->dsme.getMAC_PIB().macAutoRequest) {
            params.panDescriptorList = panDescriptorList;
            params.resultListSize = panDescriptorList.size();
        } else {
            params.resultListSize = 0;
        }

        params.status = ScanStatus::SUCCESS;
        params.scanType = this->scanType;

        LOG_INFO("Notify confirm");
        this->dsme.getMLME_SAP().getSCAN().notify_confirm(params);
    } else {
        LOG_INFO("Check next");
        this->currentScanChannelIndex++;
        this->dsme.getPlatform().setChannelNumber(this->scanChannels[this->currentScanChannelIndex]);
        this->superframesLeftForScan = this->superframesForEachChannel;
    }
    return;
}

void BeaconManager::singleBeaconScanPassiveReceived(PANDescriptor& panDescr) {
    if(this->dsme.getMAC_PIB().macAutoRequest) {
        LOG_INFO("Beacon registered during passive scan.");
        this->panDescriptorList.add(panDescr);
        if(this->panDescriptorList.full()) {
            channelScanPassiveComplete();
        }
    }
    return;
}

void BeaconManager::channelScanEnhancedActiveComplete() {
    DSME_ASSERT(this->scanning);

    LOG_INFO("Scan complete, chan " << (uint16_t)this->scanChannels[this->currentScanChannelIndex]);
    if(this->panDescriptorList.full() || this->currentScanChannelIndex >= this->scanChannels.size() - 1) {
        this->scanning = false;
        this->dsme.getMAC_PIB().macPANId = this->storedMacPANId;

        mlme_sap::SCAN_confirm_parameters params;

        if(this->dsme.getMAC_PIB().macAutoRequest) {
            params.panDescriptorList = panDescriptorList;
            params.resultListSize = panDescriptorList.size();
        } else {
            params.resultListSize = 0;
        }

        params.status = ScanStatus::SUCCESS;
        params.scanType = this->scanType;

        this->dsme.getMLME_SAP().getSCAN().notify_confirm(params);
    } else {
        this->currentScanChannelIndex++;
        this->dsme.getPlatform().setChannelNumber(this->scanChannels[this->currentScanChannelIndex]);
        this->sendEnhancedBeaconRequest();
        this->superframesLeftForScan = this->superframesForEachChannel;
    }
    return;
}

void BeaconManager::singleBeaconScanEnhancedActiveReceived(PANDescriptor& panDescr) {
    if(this->dsme.getMAC_PIB().macAutoRequest) {
        LOG_INFO("Beacon registered during enhanced active scan.");
        this->panDescriptorList.add(panDescr);
        if(this->panDescriptorList.full()) {
            channelScanEnhancedActiveComplete();
        }
    }
}

} /* namespace dsme */
