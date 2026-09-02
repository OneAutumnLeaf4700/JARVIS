#include "consent_gate.h"

#include <sstream>

namespace {

// T3/T4 confirmation is a per-call payload convention, never persisted (INV-9: "explicit
// confirmation regardless of prior permissions"). The word "confirm" must appear as the LAST
// whitespace-delimited token in the payload — not anywhere, and not as a substring (that would
// let something like "reconfirmation" slip through by accident).
bool payloadConfirms(const std::string& payload) {
    std::istringstream stream(payload);
    std::string token;
    std::string lastToken;
    while (stream >> token) {
        lastToken = token;
    }
    return lastToken == "confirm";
}

}  // namespace

ConsentGate::ConsentGate(const PluginConfig& config) : config_(config) {}

ConsentResult ConsentGate::check(const Capability& capability, const std::string& payload) const {
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
            if (payloadConfirms(payload)) {
                return {true, ""};
            }
            return {false, "'" + capability.name +
                                "' is power tier T3/T4 (destructive/external) and requires "
                                "explicit confirmation on every call — grants never cover it. "
                                "Re-run with 'confirm' added to the command, e.g. '" +
                                capability.name + " confirm'."};
    }
    return {false, "Unknown power tier."};
}
