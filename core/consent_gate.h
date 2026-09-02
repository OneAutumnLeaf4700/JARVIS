#pragma once

#include <string>

#include "capability.h"
#include "plugin_config.h"

// Outcome of a consent check: allowed or not, with a human-readable reason when denied (empty
// when allowed).
struct ConsentResult {
    bool allowed;
    std::string reason;
};

// The single place a capability's declared PowerTier is turned into an allow/deny decision.
// Capabilities never check their own consent (INV-9) — this is that check, called from
// CapabilityRegistry::dispatch() only. `payload` is the capability's raw invocation payload:
// T3/T4 confirmation reads it for a standalone "confirm" token (INV-9 — explicit, per-call,
// never inherited from a persisted grant); T0-T2 ignore it.
class ConsentGate {
 public:
    explicit ConsentGate(const PluginConfig& config);
    ConsentResult check(const Capability& capability, const std::string& payload) const;

 private:
    const PluginConfig& config_;
};
