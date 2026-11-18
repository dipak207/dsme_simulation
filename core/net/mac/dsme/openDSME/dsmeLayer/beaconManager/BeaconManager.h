/*
 * openDSME
 *
 * Implementation of the Deterministic & Synchronous Multi-channel Extension (DSME)
 * introduced in the IEEE 802.15.4e-2012 standard
 *
 * Authors: Florian Meier <florian.meier@tuhh.de>
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

#ifndef BEACONMANAGER_H_
#define BEACONMANAGER_H_

#include "../../../dsme_settings.h"
#include "../../helper/Integers.h"
#include "../../mac_services/DSME_Common.h"
#include "../../mac_services/dataStructures/BeaconBitmap.h"
#include "../../mac_services/dataStructures/DSMEPANDescriptor.h"
#include "../../mac_services/dataStructures/IEEE802154MacAddress.h"
#include "../../mac_services/mlme_sap/MLME_SAP.h"
#include "../../mac_services/mlme_sap/SCAN.h"
#include "../ackLayer/AckLayer.h"

namespace dsme {

class DSMELayer;
class IDSMEMessage;
class IEEE802154MacAddress;
class MOCmd;

class BeaconManager {
    friend class mlme_sap::SCAN;

public:
    explicit BeaconManager(DSMELayer& dsme);

    void initialize();

    void reset();

    /**
     * Called on reception of an EnhancedBeacon
     */
    bool handleEnhancedBeacon(IDSMEMessage* msg, DSMEPANDescriptor& descr);

    uint32_t getLastKnownBeaconIntervalStart() const {
        return lastKnownBeaconIntervalStart;
    }

    void preSuperframeEvent(uint16_t nextSuperframe, uint16_t nextMultiSuperframe, uint32_t nextSlotTime);
    void superframeEvent(int32_t lateness, uint32_t currentSlotTime);

    void handleBeacon(IDSMEMessage* msg);

    bool isScanning() const;

    void startScanPassive(uint16_t scanDuration, const channelList_t& scanChannels);

    /**
     * Starts an enhanced active scan (See IEEE 802.15.4e-2012 6.2.10.1)
     */
    void startScanEnhancedActive(uint16_t scanDuration, const channelList_t& scanChannels);

    void handleStartOfCFP(uint16_t currentSuperframe, uint16_t currentMultiSuperframe);

    /**
     * Gets called when CSMA Message was sent down to the PHY
     */
    void onCSMASent(IDSMEMessage* msg, CommandFrameIdentifier cmdId, DataStatus::Data_Status status, uint8_t numBackoffs);

    /**
     * Handle reception of beacon collision notification.
     * Update beacon allocation
     */
    void handleBeaconCollision(IDSMEMessage*);

    /**
     * Called on reception of an BeaconAllocationNotification
     */
    void handleBeaconAllocation(IDSMEMessage*);

    /**
     * Called on reception of an BeaconRequest
     */
    void handleBeaconRequest(IDSMEMessage*);

    /**
     * Start a coordinated, network-wide multi-superframe-order change.
     * Only the PAN coordinator may start a reconfiguration.
     */
    bool startMOReconfiguration(uint8_t newMO, bool recovery = false);

    bool isMOReconfigurationPending() const {
        return moState != MO_IDLE && moState != MO_CLIENT_ACTIVE;
    }

    bool isGTSOperational() const {
        return moState == MO_IDLE || moState == MO_CLIENT_ACTIVE;
    }

    bool blocksGTSAllocation() const {
        return moState != MO_IDLE && moState != MO_CLIENT_ACTIVE;
    }

    uint8_t getActiveMO() const;

    /**
     * Emit a machine-readable snapshot of the current MO reconfiguration state.
     * Used by the Cooja regression application immediately before its scenario
     * timeout so the validator can check the actual terminal protocol state,
     * rather than infer it from historical lifecycle messages.
     */
    void logMOProtocolSnapshot() const;

    uint8_t getPendingMO() const {
        return moTarget;
    }

    /**
     * Called on reception of a vendor specific MO reconfiguration command.
     */
    void handleMOCommand(IDSMEMessage* msg);

protected:
    DSMELayer& dsme;

    bool isBeaconAllocationSent;
    bool isBeaconAllocated;

    uint32_t lastKnownBeaconIntervalStart;

    BeaconBitmap neighborOrOwnHeardBeacons;

    DSMEPANDescriptor dsmePANDescriptor;

    long numBeaconCollision;

    uint8_t missedBeacons;

    /**
     * Send an enhanced Beacon directly
     */
    void prepareEnhancedBeacon(uint32_t startSlotTime);

    /**
     * Send an enhanced Beacon request in scan primitive
     */
    void sendEnhancedBeaconRequest();

    /**
     * Send beacon allocation notification
     */
    void sendBeaconAllocationNotification(uint16_t beaconSDIndex);

    /**
     * Send beacon collision notification
     */
    void sendBeaconCollisionNotification(uint16_t beaconSDIndex, const IEEE802154MacAddress& addr);

    void sendDone(enum AckLayerResponse result, IDSMEMessage* msg);

    AckLayer::done_callback_t doneCallback;

private:
    /* HELPER METHODS FOR SCANNING */
    void scanCurrentChannel();
    void setScanDuration(uint16_t scanDuration);

    /* CALLBACKS FOR SCANNING */
    void channelScanEnhancedActiveComplete();
    void singleBeaconScanEnhancedActiveReceived(PANDescriptor& panDescr);

    void channelScanPassiveComplete();
    void singleBeaconScanPassiveReceived(PANDescriptor& panDescr);

    /* VARIABLES FOR SCANNING */
    uint16_t currentScanChannel;

    PanDescriptorList panDescriptorList;

    bool scanning;
    ScanType scanType;
    channelList_t scanChannels;

    uint16_t storedMacPANId;
    uint8_t currentScanChannelIndex;

    uint16_t superframesForEachChannel;
    uint16_t superframesLeftForScan;
    bool transmissionPending;

    /* ADAPTIVE MULTI-SUPERFRAME-ORDER COORDINATION */
    enum MOProtocolState {
        MO_IDLE = 0,
        MO_PREPARING,
        MO_COMMIT_DECIDED,
        MO_WAITING_APPLIED,
        MO_DEGRADED,
        MO_WAITING_RELEASE_ACK,
        MO_CLIENT_PREPARED,
        MO_CLIENT_COMMITTED,
        MO_QUIESCENT,
        MO_APPLIED_HOLD,
        MO_CLIENT_ACTIVE,
        MO_QUARANTINED,
        MO_PROPOSING = MO_PREPARING,
        MO_WAITING_COMMIT_ACK = MO_COMMIT_DECIDED,
        MO_COMMITTED = MO_COMMIT_DECIDED,
        MO_WAITING_COMPLETE_ACK = MO_WAITING_RELEASE_ACK,
        MO_CLIENT_APPLIED = MO_APPLIED_HOLD,
        MO_CLIENT_COMPLETE = MO_CLIENT_ACTIVE
    };
    enum {
        MO_ACTIVATION_LEAD_BI = 3
    };

    MOProtocolState moState;
    uint8_t moTarget;
    uint8_t moOldTarget;
    uint32_t moActivationSymbols;
    uint32_t moDeadlineSymbols;
    uint32_t moEpochSession;
    uint32_t moProposalId;
    uint32_t moNextProposalId;
    uint32_t moLastEpochSession;
    uint32_t moLastEpochSequence;
    uint32_t moParticipantHash;
    uint8_t moRecoveryAttempts;
    uint8_t moRecoveryIntervalBI;
    uint32_t moNextRecoveryQuerySymbols;
    uint32_t moCommandNonce;
    uint32_t moPeerQueryNonce[MAX_NEIGHBORS];
    bool moPeerStatusReceived[MAX_NEIGHBORS];
    uint16_t moExpectedPeers[MAX_NEIGHBORS];
    bool moPeerReady[MAX_NEIGHBORS];
    bool moPeerCommitted[MAX_NEIGHBORS];
    bool moPeerApplied[MAX_NEIGHBORS];
    bool moPeerComplete[MAX_NEIGHBORS];
    uint8_t moExpectedPeerCount;
    bool moCommitSent;
    bool moAnnounced;
    bool moPreparedLocal;
    bool moCommitDecisionMade;
    bool moLocalApplied;
    bool moRecoveryMode;
    uint32_t moPeerDigest[MAX_NEIGHBORS];
    uint8_t moPeerActualMO[MAX_NEIGHBORS];
    uint8_t moPeerStatus[MAX_NEIGHBORS];

    void resetMOProtocol();
    uint32_t getBeaconIntervalSymbols() const;
    uint16_t getMOCoordinatorAddress() const;
    int8_t findMOExpectedPeer(uint16_t address) const;
    bool fillMOExpectedPeers();
    bool allMOExpectedPeersReady() const;
    bool allMOExpectedPeersCommitted() const;
    bool allMOExpectedPeersApplied() const;
    bool allMOExpectedPeersComplete() const;
    bool canShrinkTo(uint8_t newMO) const;
    bool canPrepareMO(uint8_t newMO) const;
    uint32_t getScheduleDigest() const;
    bool validateScheduleState() const;
    uint32_t getParticipantSetHash() const;
    void sendMOStatus(uint8_t phase, uint16_t destination, const MOCmd* query = 0);
    bool applyMultiSuperframeOrder(uint8_t newMO, const char* reason);
    void sendMOCommand(uint8_t phase, uint8_t targetMo, uint32_t activationSymbols, uint32_t proposalId, uint16_t destination);
    void abortMOReconfiguration(const char* reason);
    void maybeSendMOCommand(uint16_t nextSuperframe, uint16_t nextMultiSuperframe, uint32_t startSlotTime);
    void maybeActivateMO(uint16_t nextSuperframe, uint16_t nextMultiSuperframe, uint32_t startSlotTime);
    void handleBeaconMOAdoption(uint8_t advertisedMO, uint32_t beaconStartSymbols);

#ifdef STATISTICS_BEACONS
    struct BeaconStat {
        uint32_t time;
        uint16_t sender;
        uint8_t lqi;
        int8_t rssi;
        uint8_t sdIndex;
    };
    static constexpr uint8_t STATS_NUM = 10;
    uint8_t statsIdx = 0;
    uint8_t statsValid = 0;
    BeaconStat beaconStatistics[STATS_NUM];

public:
    void printBeaconStatistics();
#endif
};

} /* namespace dsme */

#endif /* BEACONMANAGER_H_ */
