#include <assert.h>
#include <stdio.h>

#include "../../core/net/mac/dsme/AdaptiveMOController.h"
#include "../../core/net/mac/dsme/openDSME/dsmeLayer/beaconManager/MOProtocol.h"

using namespace dsme;

static AdaptiveMOController makeController(uint8_t initialMo = 5, uint8_t startupGraceWindows = 0) {
    AdaptiveMOController controller;
    AdaptiveMOConfig config;
    config.minMo = 4;
    config.maxMo = 7;
    config.overloadDemandUnits = 4;
    config.lowDemandUnits = 1;
    config.overloadWindows = 3;
    config.lowDemandWindows = 4;
    config.cooldownWindows = 2;
    config.startupGraceWindows = startupGraceWindows;
    assert(controller.configure(initialMo, config));
    return controller;
}

static void testSustainedOverloadGrowsOneStep() {
    AdaptiveMOController controller = makeController();
    for(unsigned i = 0; i < 2; ++i) {
        controller.recordDemand(ADAPTIVE_MO_LOW_PRIORITY, 4);
        assert(controller.evaluate(false) == ADAPTIVE_MO_NO_CHANGE);
    }
    controller.recordDemand(ADAPTIVE_MO_LOW_PRIORITY, 4);
    assert(controller.evaluate(false) == ADAPTIVE_MO_INCREASE);
    assert(controller.getPolicyMO() == 6);
}

static void testTemporaryOverloadDoesNotGrow() {
    AdaptiveMOController controller = makeController();
    controller.recordDemand(ADAPTIVE_MO_LOW_PRIORITY, 4);
    assert(controller.evaluate(false) == ADAPTIVE_MO_NO_CHANGE);
    assert(controller.evaluate(false) == ADAPTIVE_MO_NO_CHANGE);
    controller.recordDemand(ADAPTIVE_MO_LOW_PRIORITY, 1);
    assert(controller.evaluate(false) == ADAPTIVE_MO_NO_CHANGE);
    controller.recordDemand(ADAPTIVE_MO_LOW_PRIORITY, 4);
    assert(controller.evaluate(false) == ADAPTIVE_MO_NO_CHANGE);
    assert(controller.getPolicyMO() == 5);
}

static void testSustainedLowDemandReducesMO() {
    AdaptiveMOController controller = makeController(6);
    for(unsigned i = 0; i < 3; ++i) {
        assert(controller.evaluate(false) == ADAPTIVE_MO_NO_CHANGE);
    }
    assert(controller.evaluate(false) == ADAPTIVE_MO_DECREASE);
    assert(controller.getPolicyMO() == 5);
}

static void testStartupInactivityDoesNotTriggerPrematureReduction() {
    AdaptiveMOController controller = makeController(6, 10);
    for(unsigned i = 0; i < 10; ++i) {
        assert(controller.evaluate(false) == ADAPTIVE_MO_NO_CHANGE);
    }
    assert(controller.getPolicyMO() == 6);

    for(unsigned i = 0; i < 3; ++i) {
        assert(controller.evaluate(false) == ADAPTIVE_MO_NO_CHANGE);
    }
    assert(controller.evaluate(false) == ADAPTIVE_MO_DECREASE);
    assert(controller.getPolicyMO() == 5);
}

static void testHighPriorityDemandHasGreaterWeight() {
    AdaptiveMOController high = makeController();
    AdaptiveMOController low = makeController();
    for(unsigned i = 0; i < 3; ++i) {
        high.recordDemand(ADAPTIVE_MO_HIGH_PRIORITY, 2);
        low.recordDemand(ADAPTIVE_MO_LOW_PRIORITY, 2);
        const AdaptiveMODecision highDecision = high.evaluate(false);
        const AdaptiveMODecision lowDecision = low.evaluate(false);
        if(i < 2) {
            assert(highDecision == ADAPTIVE_MO_NO_CHANGE);
        } else {
            assert(highDecision == ADAPTIVE_MO_INCREASE);
        }
        assert(lowDecision == ADAPTIVE_MO_NO_CHANGE);
    }
    assert(high.getLastEvaluatedWindow().highPriorityRequests == 1);
}

static void testCooldownPreventsRepeatedChanges() {
    AdaptiveMOController controller = makeController();
    for(unsigned i = 0; i < 3; ++i) {
        controller.recordDemand(ADAPTIVE_MO_LOW_PRIORITY, 4);
        const AdaptiveMODecision decision = controller.evaluate(false);
        if(i < 2) {
            assert(decision == ADAPTIVE_MO_NO_CHANGE);
        } else {
            assert(decision == ADAPTIVE_MO_INCREASE);
        }
    }

    for(unsigned i = 0; i < 2; ++i) {
        controller.recordDemand(ADAPTIVE_MO_LOW_PRIORITY, 4);
        assert(controller.evaluate(false) == ADAPTIVE_MO_NO_CHANGE);
    }
    for(unsigned i = 0; i < 3; ++i) {
        controller.recordDemand(ADAPTIVE_MO_LOW_PRIORITY, 4);
        const AdaptiveMODecision decision = controller.evaluate(false);
        if(i < 2) {
            assert(decision == ADAPTIVE_MO_NO_CHANGE);
        } else {
            assert(decision == ADAPTIVE_MO_INCREASE);
        }
    }
    assert(controller.getPolicyMO() == 7);
}

static void testMOminAndMaxLimits() {
    AdaptiveMOController atMax = makeController(7);
    for(unsigned i = 0; i < 6; ++i) {
        atMax.recordDemand(ADAPTIVE_MO_LOW_PRIORITY, 4);
        assert(atMax.evaluate(false) == ADAPTIVE_MO_NO_CHANGE);
    }
    assert(atMax.getPolicyMO() == 7);

    AdaptiveMOController atMin = makeController(4);
    for(unsigned i = 0; i < 8; ++i) {
        assert(atMin.evaluate(false) == ADAPTIVE_MO_NO_CHANGE);
    }
    assert(atMin.getPolicyMO() == 4);
}

static void testPendingReconfigurationBlocksMOChange() {
    AdaptiveMOController controller = makeController();
    for(unsigned i = 0; i < 3; ++i) {
        controller.recordDemand(ADAPTIVE_MO_HIGH_PRIORITY, 4);
        assert(controller.evaluate(true) == ADAPTIVE_MO_NO_CHANGE);
    }
    assert(controller.getPolicyMO() == 5);
}

static void testRejectedRequestCountsAsOverloadNotAllocation() {
    AdaptiveMOController controller = makeController();
    controller.recordRejectedRequest(ADAPTIVE_MO_HIGH_PRIORITY, 1);
    assert(controller.evaluate(false) == ADAPTIVE_MO_NO_CHANGE);
    const AdaptiveMOWindow window = controller.getLastEvaluatedWindow();
    assert(window.rejectedRequests == 1);
    assert(window.weightedDemandUnits == 2);
    assert(controller.getPolicyMO() == 5);
}

static void testOverflowRejectionWithoutIndicationCountsAsOverload() {
    AdaptiveMOController controller = makeController();
    for(unsigned i = 0; i < 3; ++i) {
        controller.recordRejection();
        const AdaptiveMODecision decision = controller.evaluate(false);
        if(i < 2) {
            assert(decision == ADAPTIVE_MO_NO_CHANGE);
        } else {
            assert(decision == ADAPTIVE_MO_INCREASE);
        }
    }
    assert(controller.getLastEvaluatedWindow().rejectedRequests == 1);
    assert(controller.getPolicyMO() == 6);
}

static void testInvalidConfigurationIsRejected() {
    AdaptiveMOController controller;
    AdaptiveMOConfig config;
    config.minMo = 8;
    config.maxMo = 7;
    assert(!controller.configure(7, config));
    assert(controller.evaluate(false) == ADAPTIVE_MO_NO_CHANGE);
}

static void testProposalAndReadyRequireExpectedSourceAndExactTransaction() {
    assert(mo_protocol::isExpectedCoordinator(1, 1, 0x1234, 0x1234));
    assert(!mo_protocol::isExpectedCoordinator(2, 1, 0x1234, 0x1234));
    assert(!mo_protocol::isExpectedCoordinator(1, 1, 0x4321, 0x1234));
    assert(mo_protocol::matchesTransaction(7, 6, 1000, 7, 6, 1000));
    assert(!mo_protocol::matchesTransaction(8, 6, 1000, 7, 6, 1000));
    assert(!mo_protocol::matchesTransaction(7, 5, 1000, 7, 6, 1000));
    assert(!mo_protocol::matchesTransaction(7, 6, 1001, 7, 6, 1000));
}

static void testCommitRequiresMatchingProposal() {
    assert(mo_protocol::matchesTransaction(4, 6, 5000, 4, 6, 5000));
    assert(!mo_protocol::matchesTransaction(4, 6, 5000, 3, 6, 5000));
    assert(!mo_protocol::matchesTransaction(4, 7, 5000, 4, 6, 5000));
}

static void testShrinkAndAllocationSafetyPredicates() {
    assert(mo_protocol::canShrink(false, false, false));
    assert(!mo_protocol::canShrink(true, false, false));
    assert(!mo_protocol::canShrink(false, true, false));
    assert(!mo_protocol::canShrink(false, false, true));
    assert(mo_protocol::blocksAllocation(true, 6, 5, true));
    assert(!mo_protocol::blocksAllocation(true, 6, 5, false));
    assert(!mo_protocol::blocksAllocation(true, 6, 6, true));
    assert(!mo_protocol::blocksAllocation(false, 6, 5, true));
}

static void testMissingCommitCannotActivate() {
    assert(!mo_protocol::activationDue(false, true, 1000, 1000));
    assert(!mo_protocol::activationDue(true, false, 1000, 1000));
    assert(mo_protocol::activationDue(true, true, 1000, 1000));
}

static void testActivationTimeWraparound() {
    assert(mo_protocol::isFutureWithin(0xfffffff0UL, 0x00000010UL, 64));
    assert(mo_protocol::timeReached(0x00000010UL, 0xfffffff0UL));
    assert(!mo_protocol::isFutureWithin(100, 99, 64));
    assert(!mo_protocol::isFutureWithin(100, 0x80000064UL, 0x7fffffffUL));
}

static void testMOApplicationFailureRollsBackLocalState() {
    uint8_t activeMO = 5;
    const bool applied = mo_protocol::applyOrder(activeMO, 6, [](uint8_t order) {
        return order != 6;
    });
    assert(!applied);
    assert(activeMO == 5);
}

static void testNetworkCompletionRequiresEveryPeerApplied() {
    assert(mo_protocol::networkApplied(true, true));
    assert(!mo_protocol::networkApplied(true, false));
    assert(!mo_protocol::networkApplied(false, true));
}

static void testOverloadThresholdEqualityTriggersAfterPersistence() {
    AdaptiveMOController controller = makeController();
    for(unsigned i = 0; i < 2; ++i) {
        controller.recordDemand(ADAPTIVE_MO_LOW_PRIORITY, 4);
        assert(controller.evaluate(false) == ADAPTIVE_MO_NO_CHANGE);
    }
    controller.recordDemand(ADAPTIVE_MO_LOW_PRIORITY, 4);
    assert(controller.evaluate(false) == ADAPTIVE_MO_INCREASE);
}

static void testLowDemandThresholdEqualityTriggersAfterPersistence() {
    AdaptiveMOController controller = makeController(6);
    for(unsigned i = 0; i < 3; ++i) {
        controller.recordDemand(ADAPTIVE_MO_LOW_PRIORITY, 1);
        assert(controller.evaluate(false) == ADAPTIVE_MO_NO_CHANGE);
    }
    controller.recordDemand(ADAPTIVE_MO_LOW_PRIORITY, 1);
    assert(controller.evaluate(false) == ADAPTIVE_MO_DECREASE);
}

static void testDemandOscillationResetsPersistenceStreaks() {
    AdaptiveMOController controller = makeController();
    controller.recordDemand(ADAPTIVE_MO_LOW_PRIORITY, 4);
    assert(controller.evaluate(false) == ADAPTIVE_MO_NO_CHANGE);
    controller.recordDemand(ADAPTIVE_MO_LOW_PRIORITY, 3);
    assert(controller.evaluate(false) == ADAPTIVE_MO_NO_CHANGE);
    controller.recordDemand(ADAPTIVE_MO_LOW_PRIORITY, 4);
    assert(controller.evaluate(false) == ADAPTIVE_MO_NO_CHANGE);
    controller.recordDemand(ADAPTIVE_MO_LOW_PRIORITY, 4);
    assert(controller.evaluate(false) == ADAPTIVE_MO_NO_CHANGE);
    assert(controller.getPolicyMO() == 5);
}

static void testDemandCountersSaturateInsteadOfWrapping() {
    AdaptiveMOController controller = makeController();
    for(unsigned i = 0; i < 129; ++i) {
        controller.recordDemand(ADAPTIVE_MO_HIGH_PRIORITY, 255);
    }
    assert(controller.evaluate(false) == ADAPTIVE_MO_NO_CHANGE);
    const AdaptiveMOWindow window = controller.getLastEvaluatedWindow();
    assert(window.weightedDemandUnits == 0xffffU);
    assert(window.highPriorityRequests == 129);
}

static void testConfigurationRejectsUnsupportedMaximumAndZeroPersistence() {
    AdaptiveMOController controller;
    AdaptiveMOConfig config;
    config.minMo = 4;
    config.maxMo = 15;
    assert(!controller.configure(5, config));
    config.minMo = 4;
    config.maxMo = 7;
    config.overloadWindows = 0;
    assert(!controller.configure(5, config));
    config.overloadWindows = 3;
    config.lowDemandWindows = 0;
    assert(!controller.configure(5, config));
}

static void testCommittedDecisionCannotBeAbortedOnAckLoss() {
    assert(mo_protocol::mayAbort(false));
    assert(!mo_protocol::mayAbort(true));
}

static void testPreparedClientWithoutCommitMustQuiesce() {
    const bool prepared = true;
    const bool committed = false;
    assert(mo_protocol::mustQuiesceAtBoundary(prepared, committed));
    assert(!mo_protocol::mustQuiesceAtBoundary(true, true));
    assert(!mo_protocol::mustQuiesceAtBoundary(false, false));
}

static void testCommittedClientAppliesAtBoundary() {
    const bool prepared = true;
    const bool committed = true;
    assert(!mo_protocol::mustQuiesceAtBoundary(prepared, committed));
}

static void testRecoveryDeadlineUsesRolloverSafeClock() {
    assert(!mo_protocol::recoveryDeadlineReached(99, 100));
    assert(mo_protocol::recoveryDeadlineReached(100, 100));
    assert(mo_protocol::recoveryDeadlineReached(0x00000010UL, 0xfffffff0UL));
}

static void testSessionAndSequenceBothQualifyTransaction() {
    assert(mo_protocol::sameEpoch(12, 0xffffffffUL, 12, 0xffffffffUL));
    assert(!mo_protocol::sameEpoch(13, 1, 12, 1));
    assert(!mo_protocol::sameEpoch(12, 2, 12, 1));
}

static void testBoundaryActionForCommittedAndUncommittedClients() {
    assert(mo_protocol::participantBoundaryAction(true, true) == mo_protocol::BOUNDARY_APPLY);
    assert(mo_protocol::participantBoundaryAction(true, false) == mo_protocol::BOUNDARY_QUIESCE);
    assert(mo_protocol::participantBoundaryAction(false, false) == mo_protocol::BOUNDARY_WAIT);
}

static void testReadyOldMOAndScheduleValidation() {
    assert(mo_protocol::readyMatchesExpected(false, 5, 5, true, true));
    assert(!mo_protocol::readyMatchesExpected(false, 5, 6, true, true));
    assert(!mo_protocol::readyMatchesExpected(false, 5, 5, true, false));
    assert(mo_protocol::readyMatchesExpected(true, 5, 6, true, true));
}

static void testInvalidSameMOStateCannotBeReleased() {
    assert(mo_protocol::canReleaseParticipant(true, 6, 6, true, true));
    assert(!mo_protocol::canReleaseParticipant(true, 6, 6, false, true));
    assert(!mo_protocol::canReleaseParticipant(false, 6, 6, true, true));
    assert(!mo_protocol::canReleaseParticipant(true, 6, 5, true, true));
}

static void testRecoveryRequiresExplicitFlagAndValidatedState() {
    assert(mo_protocol::canAcceptRecovery(true, true, true));
    assert(!mo_protocol::canAcceptRecovery(false, true, true));
    assert(!mo_protocol::canAcceptRecovery(true, false, true));
    assert(!mo_protocol::canAcceptRecovery(true, true, false));
}

static void testStatusQueryFromUnpreparedPeerRequiresCoordinatorAndNonce() {
    assert(mo_protocol::mayAnswerStatusQuery(true, 1));
    assert(!mo_protocol::mayAnswerStatusQuery(false, 1));
    assert(!mo_protocol::mayAnswerStatusQuery(true, 0));
}

static void testStatusResponseMustMatchOutstandingNonce() {
    assert(mo_protocol::queryNonceMatches(17, 17));
    assert(!mo_protocol::queryNonceMatches(16, 17));
    assert(!mo_protocol::queryNonceMatches(17, 0));
}

static void testBoundedRecoveryBackoff() {
    assert(mo_protocol::nextBackoffBI(1, 16) == 2);
    assert(mo_protocol::nextBackoffBI(2, 16) == 4);
    assert(mo_protocol::nextBackoffBI(8, 16) == 16);
    assert(mo_protocol::nextBackoffBI(16, 16) == 16);
}

static void testParticipantSetOverflowRejected() {
    assert(mo_protocol::participantSetFits(25, 25));
    assert(!mo_protocol::participantSetFits(26, 25));
}

static void testScheduleDimensionsAndRollbackMustValidate() {
    assert(mo_protocol::scheduleDimensionsMatch(8, 8, true, true));
    assert(!mo_protocol::scheduleDimensionsMatch(8, 4, true, true));
    assert(!mo_protocol::scheduleDimensionsMatch(8, 8, false, true));
    assert(!mo_protocol::scheduleDimensionsMatch(8, 8, true, false));
    assert(mo_protocol::recoveryScheduleMayProceed(true, true));
    assert(!mo_protocol::recoveryScheduleMayProceed(true, false));
    assert(!mo_protocol::recoveryScheduleMayProceed(false, true));
}

static void testACTRollbackFailureIsDetectedByDimensions() {
    assert(mo_protocol::scheduleDimensionsAllMatch(4, 4, 4, 4));
    assert(!mo_protocol::scheduleDimensionsAllMatch(4, 8, 4, 4));
}

static void testSABRollbackFailureIsDetectedByDimensions() {
    assert(!mo_protocol::scheduleDimensionsAllMatch(4, 4, 8, 4));
}

static void testRejectedPrecommitCanAbortButCommittedCannot() {
    assert(mo_protocol::mayAbort(false));
    assert(!mo_protocol::mayAbort(true));
}

static void testLostAbortLeavesPreparedParticipantQuiescent() {
    assert(mo_protocol::participantBoundaryAction(true, false) == mo_protocol::BOUNDARY_QUIESCE);
    assert(mo_protocol::participantBoundaryAction(true, true) == mo_protocol::BOUNDARY_APPLY);
}

static void testStatusAfterLocalResetCanBeCorrelated() {
    assert(mo_protocol::mayAnswerStatusQuery(true, 99));
    assert(mo_protocol::queryNonceMatches(99, 99));
    assert(!mo_protocol::queryNonceMatches(99, 100));
}

static void testInvalidScheduleCannotEnterQuarantineRecovery() {
    assert(!mo_protocol::canAcceptRecovery(true, true, false));
    assert(!mo_protocol::recoveryScheduleMayProceed(false, true));
}

static void testRecoveryReadyMayReportDifferentVerifiedOldMO() {
    assert(mo_protocol::readyMatchesExpected(true, 6, 5, true, true));
    assert(!mo_protocol::readyMatchesExpected(true, 6, 5, true, false));
}

static void testMatchingMessageTupleDoesNotMatchStaleTimestamp() {
    assert(mo_protocol::matchesTransaction(9, 6, 0xfffffff0UL, 9, 6, 0xfffffff0UL));
    assert(!mo_protocol::matchesTransaction(9, 6, 0xfffffff0UL, 9, 6, 0x00000010UL));
}

#define RUN_TEST(test) do { test(); printf("HOST_TEST_PASS %s\n", #test); } while(0)

int main() {
    RUN_TEST(testSustainedOverloadGrowsOneStep);
    RUN_TEST(testTemporaryOverloadDoesNotGrow);
    RUN_TEST(testSustainedLowDemandReducesMO);
    RUN_TEST(testStartupInactivityDoesNotTriggerPrematureReduction);
    RUN_TEST(testHighPriorityDemandHasGreaterWeight);
    RUN_TEST(testCooldownPreventsRepeatedChanges);
    RUN_TEST(testMOminAndMaxLimits);
    RUN_TEST(testPendingReconfigurationBlocksMOChange);
    RUN_TEST(testRejectedRequestCountsAsOverloadNotAllocation);
    RUN_TEST(testOverflowRejectionWithoutIndicationCountsAsOverload);
    RUN_TEST(testInvalidConfigurationIsRejected);
    RUN_TEST(testProposalAndReadyRequireExpectedSourceAndExactTransaction);
    RUN_TEST(testCommitRequiresMatchingProposal);
    RUN_TEST(testShrinkAndAllocationSafetyPredicates);
    RUN_TEST(testMissingCommitCannotActivate);
    RUN_TEST(testActivationTimeWraparound);
    RUN_TEST(testMOApplicationFailureRollsBackLocalState);
    RUN_TEST(testNetworkCompletionRequiresEveryPeerApplied);
    RUN_TEST(testOverloadThresholdEqualityTriggersAfterPersistence);
    RUN_TEST(testLowDemandThresholdEqualityTriggersAfterPersistence);
    RUN_TEST(testDemandOscillationResetsPersistenceStreaks);
    RUN_TEST(testDemandCountersSaturateInsteadOfWrapping);
    RUN_TEST(testConfigurationRejectsUnsupportedMaximumAndZeroPersistence);
    RUN_TEST(testMatchingMessageTupleDoesNotMatchStaleTimestamp);
    RUN_TEST(testCommittedDecisionCannotBeAbortedOnAckLoss);
    RUN_TEST(testPreparedClientWithoutCommitMustQuiesce);
    RUN_TEST(testCommittedClientAppliesAtBoundary);
    RUN_TEST(testRecoveryDeadlineUsesRolloverSafeClock);
    RUN_TEST(testSessionAndSequenceBothQualifyTransaction);
    RUN_TEST(testBoundaryActionForCommittedAndUncommittedClients);
    RUN_TEST(testReadyOldMOAndScheduleValidation);
    RUN_TEST(testInvalidSameMOStateCannotBeReleased);
    RUN_TEST(testRecoveryRequiresExplicitFlagAndValidatedState);
    RUN_TEST(testStatusQueryFromUnpreparedPeerRequiresCoordinatorAndNonce);
    RUN_TEST(testStatusResponseMustMatchOutstandingNonce);
    RUN_TEST(testBoundedRecoveryBackoff);
    RUN_TEST(testParticipantSetOverflowRejected);
    RUN_TEST(testScheduleDimensionsAndRollbackMustValidate);
    RUN_TEST(testACTRollbackFailureIsDetectedByDimensions);
    RUN_TEST(testSABRollbackFailureIsDetectedByDimensions);
    RUN_TEST(testRejectedPrecommitCanAbortButCommittedCannot);
    RUN_TEST(testLostAbortLeavesPreparedParticipantQuiescent);
    RUN_TEST(testStatusAfterLocalResetCanBeCorrelated);
    RUN_TEST(testInvalidScheduleCannotEnterQuarantineRecovery);
    RUN_TEST(testRecoveryReadyMayReportDifferentVerifiedOldMO);
    puts("Adaptive MO and reconfiguration protocol tests passed (45 cases).");
    return 0;
}
