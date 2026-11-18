#ifndef ADAPTIVE_MO_CONTROLLER_H
#define ADAPTIVE_MO_CONTROLLER_H

#include <stdint.h>

namespace dsme {

enum AdaptiveMOPriority : uint8_t {
    ADAPTIVE_MO_LOW_PRIORITY = 0,
    ADAPTIVE_MO_HIGH_PRIORITY = 1
};

enum AdaptiveMODecision : uint8_t {
    ADAPTIVE_MO_NO_CHANGE = 0,
    ADAPTIVE_MO_INCREASE,
    ADAPTIVE_MO_DECREASE
};

struct AdaptiveMOConfig {
    uint8_t minMo;
    uint8_t maxMo;
    uint16_t overloadDemandUnits;
    uint16_t lowDemandUnits;
    uint8_t overloadWindows;
    uint8_t lowDemandWindows;
    uint8_t cooldownWindows;
    uint8_t startupGraceWindows;

    AdaptiveMOConfig()
        : minMo(4),
          maxMo(9),
          overloadDemandUnits(4),
          lowDemandUnits(1),
          overloadWindows(3),
          lowDemandWindows(5),
          cooldownWindows(2),
          startupGraceWindows(10) {
    }
};

struct AdaptiveMOWindow {
    uint16_t weightedDemandUnits;
    uint16_t highPriorityRequests;
    uint16_t rejectedRequests;
};

class AdaptiveMOController {
public:
    AdaptiveMOController()
        : config(),
          window{0, 0, 0},
          lastEvaluatedWindow{0, 0, 0},
          policyMo(0),
          overloadStreak(0),
          lowDemandStreak(0),
          cooldownRemaining(0),
          startupWindowsElapsed(0),
          valid(false) {
    }

    bool configure(uint8_t initialMo, const AdaptiveMOConfig& newConfig) {
        if(newConfig.minMo > newConfig.maxMo || newConfig.maxMo > 14 ||
           newConfig.lowDemandUnits >= newConfig.overloadDemandUnits ||
           newConfig.overloadWindows == 0 || newConfig.lowDemandWindows == 0 ||
           initialMo < newConfig.minMo || initialMo > newConfig.maxMo) {
            valid = false;
            return false;
        }

        config = newConfig;
        policyMo = initialMo;
        valid = true;
        resetState();
        clearWindow();
        lastEvaluatedWindow = window;
        return true;
    }

    void recordDemand(AdaptiveMOPriority priority, uint8_t slots) {
        if(!valid || slots == 0) {
            return;
        }
        const uint16_t weight = priority == ADAPTIVE_MO_HIGH_PRIORITY ? 2 : 1;
        addSaturated(window.weightedDemandUnits, static_cast<uint16_t>(slots * weight));
        if(priority == ADAPTIVE_MO_HIGH_PRIORITY) {
            addSaturated(window.highPriorityRequests, 1);
        }
    }

    void recordRejection() {
        if(valid) {
            addSaturated(window.rejectedRequests, 1);
        }
    }

    void recordRejectedRequest(AdaptiveMOPriority priority, uint8_t slots) {
        if(!valid || slots == 0) {
            return;
        }
        recordDemand(priority, slots);
        recordRejection();
    }

    AdaptiveMODecision evaluate(bool reconfigurationPending) {
        if(!valid) {
            return ADAPTIVE_MO_NO_CHANGE;
        }

        const AdaptiveMOWindow completedWindow = window;
        lastEvaluatedWindow = completedWindow;
        clearWindow();

        const bool startupGraceActive = startupWindowsElapsed < config.startupGraceWindows;
        if(startupWindowsElapsed < 0xff) {
            ++startupWindowsElapsed;
        }

        if(reconfigurationPending) {
            overloadStreak = 0;
            lowDemandStreak = 0;
            return ADAPTIVE_MO_NO_CHANGE;
        }

        if(cooldownRemaining > 0) {
            --cooldownRemaining;
            overloadStreak = 0;
            lowDemandStreak = 0;
            return ADAPTIVE_MO_NO_CHANGE;
        }

        const bool overloaded = completedWindow.rejectedRequests > 0 ||
            completedWindow.weightedDemandUnits >= config.overloadDemandUnits;
        const bool lowDemand = completedWindow.rejectedRequests == 0 &&
            completedWindow.weightedDemandUnits <= config.lowDemandUnits;

        if(overloaded) {
            overloadStreak = incrementSaturated(overloadStreak);
            lowDemandStreak = 0;
            if(overloadStreak >= config.overloadWindows && policyMo < config.maxMo) {
                ++policyMo;
                overloadStreak = 0;
                cooldownRemaining = config.cooldownWindows;
                return ADAPTIVE_MO_INCREASE;
            }
        } else if(lowDemand && !startupGraceActive) {
            lowDemandStreak = incrementSaturated(lowDemandStreak);
            overloadStreak = 0;
            if(lowDemandStreak >= config.lowDemandWindows && policyMo > config.minMo) {
                --policyMo;
                lowDemandStreak = 0;
                cooldownRemaining = config.cooldownWindows;
                return ADAPTIVE_MO_DECREASE;
            }
        } else {
            overloadStreak = 0;
            lowDemandStreak = 0;
        }

        return ADAPTIVE_MO_NO_CHANGE;
    }

    uint8_t getPolicyMO() const {
        return policyMo;
    }

    /**
     * Re-align the policy state with the actually active MO, for example after
     * an aborted reconfiguration. Resets the persistence streaks and starts a
     * fresh cooldown so the policy does not immediately re-propose.
     */
    void syncPolicyMO(uint8_t actualMo) {
        if(!valid) {
            return;
        }
        policyMo = actualMo;
        overloadStreak = 0;
        lowDemandStreak = 0;
        cooldownRemaining = config.cooldownWindows;
        clearWindow();
    }

    AdaptiveMOWindow getLastEvaluatedWindow() const {
        return lastEvaluatedWindow;
    }

private:
    static void addSaturated(uint16_t& value, uint16_t amount) {
        const uint32_t sum = static_cast<uint32_t>(value) + amount;
        value = sum > 0xffffU ? 0xffffU : static_cast<uint16_t>(sum);
    }

    static uint8_t incrementSaturated(uint8_t value) {
        return value == 0xff ? value : static_cast<uint8_t>(value + 1);
    }

    void clearWindow() {
        window.weightedDemandUnits = 0;
        window.highPriorityRequests = 0;
        window.rejectedRequests = 0;
    }

    void resetState() {
        overloadStreak = 0;
        lowDemandStreak = 0;
        cooldownRemaining = 0;
        startupWindowsElapsed = 0;
    }

    AdaptiveMOConfig config;
    AdaptiveMOWindow window;
    AdaptiveMOWindow lastEvaluatedWindow;
    uint8_t policyMo;
    uint8_t overloadStreak;
    uint8_t lowDemandStreak;
    uint8_t cooldownRemaining;
    uint8_t startupWindowsElapsed;
    bool valid;
};

} // namespace dsme

#endif
