/*
 * Small, deterministic helpers shared by the live MO protocol and host tests.
 */

#ifndef MO_PROTOCOL_H_
#define MO_PROTOCOL_H_

#include <stdint.h>

namespace dsme {
namespace mo_protocol {

inline bool timeReached(uint32_t now, uint32_t deadline) {
    return (now - deadline) < 0x80000000UL;
}

inline bool isFutureWithin(uint32_t now, uint32_t deadline, uint32_t maximumLead) {
    const uint32_t lead = deadline - now;
    return lead != 0 && lead <= maximumLead && lead < 0x80000000UL;
}

inline bool isExpectedCoordinator(uint16_t source, uint16_t expectedCoordinator, uint16_t sourcePan, uint16_t expectedPan) {
    return expectedCoordinator != 0xffff && source == expectedCoordinator && sourcePan == expectedPan;
}

inline bool matchesTransaction(uint8_t proposal, uint8_t target, uint32_t activation,
                               uint8_t expectedProposal, uint8_t expectedTarget, uint32_t expectedActivation) {
    return proposal == expectedProposal && target == expectedTarget && activation == expectedActivation;
}

inline bool canShrink(bool hasAllocationOutsideRange, bool hasNeighborUseOutsideRange, bool hasPendingGTS) {
    return !hasAllocationOutsideRange && !hasNeighborUseOutsideRange && !hasPendingGTS;
}

inline bool blocksAllocation(bool reconfigurationPending, uint8_t activeMO, uint8_t targetMO, bool allocation) {
    return allocation && reconfigurationPending && targetMO < activeMO;
}

inline bool activationDue(bool committed, bool atBoundary, uint32_t now, uint32_t activation) {
    return committed && atBoundary && timeReached(now, activation);
}

inline bool networkApplied(bool localApplied, bool allPeersApplied) {
    return localApplied && allPeersApplied;
}

/* Once COMMIT is issued it is never safe to reverse the decision on timeout. */
inline bool mayAbort(bool commitDecisionMade) {
    return !commitDecisionMade;
}

/* A prepared participant that did not receive COMMIT must stop CFP/GTS operation. */
inline bool mustQuiesceAtBoundary(bool prepared, bool commitReceived) {
    return prepared && !commitReceived;
}

enum BoundaryAction { BOUNDARY_WAIT, BOUNDARY_QUIESCE, BOUNDARY_APPLY };
inline BoundaryAction participantBoundaryAction(bool prepared, bool commitReceived) {
    if(!prepared) return BOUNDARY_WAIT;
    return commitReceived ? BOUNDARY_APPLY : BOUNDARY_QUIESCE;
}

inline bool readyMatchesExpected(bool recovery, uint8_t expectedMo, uint8_t reportedMo,
                                 bool prepared, bool scheduleValid) {
    return prepared && scheduleValid && (recovery || expectedMo == reportedMo);
}

inline bool canAcceptRecovery(bool recovery, bool recoverableState, bool scheduleValid) {
    return recovery && recoverableState && scheduleValid;
}

inline bool canReleaseParticipant(bool transactionMatches, uint8_t targetMo, uint8_t actualMo,
                                  bool scheduleValid, bool appliedHoldOrActive) {
    return transactionMatches && targetMo == actualMo && scheduleValid && appliedHoldOrActive;
}

inline bool queryNonceMatches(uint32_t responseNonce, uint32_t outstandingNonce) {
    return outstandingNonce != 0 && responseNonce == outstandingNonce;
}

inline bool mayAnswerStatusQuery(bool authenticatedCoordinator, uint32_t nonce) {
    return authenticatedCoordinator && nonce != 0;
}

inline bool scheduleDimensionsMatch(uint16_t expectedSuperframes, uint16_t actualSuperframes,
                                    bool actValid, bool sabValid) {
    return expectedSuperframes == actualSuperframes && actValid && sabValid;
}

inline bool scheduleDimensionsAllMatch(uint16_t expectedSuperframes, uint16_t actSuperframes,
                                      uint16_t sabSuperframes, uint16_t runtimeSuperframes) {
    return expectedSuperframes == actSuperframes && expectedSuperframes == sabSuperframes &&
           expectedSuperframes == runtimeSuperframes;
}

inline bool recoveryScheduleMayProceed(bool oldScheduleValidated, bool rollbackSucceeded) {
    return oldScheduleValidated && rollbackSucceeded;
}

inline bool participantSetFits(uint16_t count, uint16_t capacity) {
    return count <= capacity;
}

inline uint8_t nextBackoffBI(uint8_t current, uint8_t maximum) {
    if(current >= maximum) return maximum;
    const uint16_t next = static_cast<uint16_t>(current) * 2;
    return next > maximum ? maximum : static_cast<uint8_t>(next);
}

inline bool recoveryDeadlineReached(uint32_t now, uint32_t deadline) {
    return timeReached(now, deadline);
}

inline bool sameEpoch(uint32_t session, uint32_t sequence,
                      uint32_t expectedSession, uint32_t expectedSequence) {
    return session == expectedSession && sequence == expectedSequence;
}

template <typename Resize>
bool applyOrder(uint8_t& activeMO, uint8_t targetMO, Resize resize) {
    const uint8_t oldMO = activeMO;
    if(oldMO == targetMO) {
        return true;
    }

    activeMO = targetMO;
    if(resize(targetMO)) {
        return true;
    }

    activeMO = oldMO;
    resize(oldMO);
    return false;
}

} // namespace mo_protocol
} // namespace dsme

#endif /* MO_PROTOCOL_H_ */
