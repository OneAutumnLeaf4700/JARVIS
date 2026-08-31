#include "consent_gate.h"

ConsentGate::ConsentGate(const PluginConfig& config) : config_(config) {}

ConsentResult ConsentGate::check(const Capability& capability) const {
    switch (capability.powerTier) {
        case PowerTier::T0_READ_ONLY:
        case PowerTier::T1_STATEFUL_LOCAL:
            return {true, ""};

        case PowerTier::T2_SYSTEM_AFFECTING:
            if (config_.isGranted(capability.name)) {
                return {true, ""};
            }
            return {false, "Capability '" + capability.name +
                                "' requires consent. Grant it with: jarvis --grant " + capability.name};

        case PowerTier::T3_DESTRUCTIVE:
        case PowerTier::T4_EXTERNAL:
            return {false, "Power tier T3/T4 enforcement is not yet implemented — capability '" +
                                capability.name + "' cannot run yet."};
    }
    return {false, "Unknown power tier."};
}
