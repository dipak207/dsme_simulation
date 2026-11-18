/* Vendor-specific, epoch-qualified adaptive MO transaction command. */
#ifndef MOCMD_H_
#define MOCMD_H_

#include "../../helper/Integers.h"
#include "../../mac_services/dataStructures/DSMEMessageElement.h"

namespace dsme {

enum MOCommandPhase {
    MO_CMD_PREPARE = 1,
    MO_CMD_READY = 2,
    MO_CMD_COMMIT = 3,
    MO_CMD_COMMIT_ACK = 4,
    MO_CMD_APPLIED = 5,
    MO_CMD_RELEASE = 6,
    MO_CMD_RELEASE_ACK = 7,
    MO_CMD_ABORT = 8,
    MO_CMD_REJECT = 9,
    MO_CMD_STATUS_QUERY = 10,
    MO_CMD_STATUS_RESPONSE = 11,
    MO_CMD_PROPOSE = MO_CMD_PREPARE,
    MO_CMD_COMPLETE = MO_CMD_RELEASE,
    MO_CMD_COMPLETE_ACK = MO_CMD_RELEASE_ACK
};

class MOCmd : public DSMEMessageElement {
public:
    MOCmd() : phase(0), targetMo(0), oldMo(0), epochSession(0), epochSequence(0),
              activationSymbols(0), deadlineSymbols(0), participantHash(0),
              actualMo(0), participantState(0), scheduleValid(0), recovery(0), queryNonce(0), scheduleDigest(0) {}

    uint8_t getPhase() const { return phase; }
    void setPhase(uint8_t v) { phase = v; }
    uint8_t getTargetMo() const { return targetMo; }
    void setTargetMo(uint8_t v) { targetMo = v; }
    uint8_t getOldMo() const { return oldMo; }
    void setOldMo(uint8_t v) { oldMo = v; }
    uint32_t getEpochSession() const { return epochSession; }
    void setEpochSession(uint32_t v) { epochSession = v; }
    uint32_t getEpochSequence() const { return epochSequence; }
    void setEpochSequence(uint32_t v) { epochSequence = v; }
    uint32_t getActivationSymbols() const { return activationSymbols; }
    void setActivationSymbols(uint32_t v) { activationSymbols = v; }
    uint32_t getDeadlineSymbols() const { return deadlineSymbols; }
    void setDeadlineSymbols(uint32_t v) { deadlineSymbols = v; }
    uint32_t getParticipantHash() const { return participantHash; }
    void setParticipantHash(uint32_t v) { participantHash = v; }
    uint8_t getActualMo() const { return actualMo; }
    void setActualMo(uint8_t v) { actualMo = v; }
    uint8_t getParticipantState() const { return participantState; }
    void setParticipantState(uint8_t v) { participantState = v; }
    bool getScheduleValid() const { return scheduleValid != 0; }
    bool getRecovery() const { return recovery != 0; }
    void setRecovery(bool v) { recovery = v ? 1 : 0; }
    void setScheduleValid(bool v) { scheduleValid = v ? 1 : 0; }
    uint32_t getQueryNonce() const { return queryNonce; }
    void setQueryNonce(uint32_t v) { queryNonce = v; }
    uint32_t getScheduleDigest() const { return scheduleDigest; }
    void setScheduleDigest(uint32_t v) { scheduleDigest = v; }

    virtual uint8_t getSerializationLength() { return 35; }
    virtual void serialize(Serializer& s) {
        s << phase << targetMo << oldMo << epochSession << epochSequence
          << activationSymbols << deadlineSymbols << participantHash
          << actualMo << participantState << scheduleValid << recovery << queryNonce << scheduleDigest;
    }

private:
    uint8_t phase, targetMo, oldMo;
    uint32_t epochSession, epochSequence;
    uint32_t activationSymbols, deadlineSymbols, participantHash;
    uint8_t actualMo, participantState;
    uint8_t scheduleValid, recovery;
    uint32_t queryNonce, scheduleDigest;
};

} // namespace dsme
#endif
