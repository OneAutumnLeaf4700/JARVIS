#ifndef JARVIS_PLUGIN_ABI_H
#define JARVIS_PLUGIN_ABI_H

/* The JARVIS plugin ABI — pure C, no C++ types cross this boundary. This is the ONLY header a
 * plugin author includes. See docs/superpowers/specs/2026-08-31-plugin-sdk-loader-design.md
 * §3 for the full rationale (avoiding STL-ABI/exception/RTTI mismatches across a dlopen
 * boundary between independently-built binaries). */

#ifdef __cplusplus
extern "C" {
#endif

#define JARVIS_PLUGIN_ABI_VERSION 1

typedef enum {
    JARVIS_POWER_TIER_T0_READ_ONLY = 0,
    JARVIS_POWER_TIER_T1_STATEFUL_LOCAL = 1,
    JARVIS_POWER_TIER_T2_SYSTEM_AFFECTING = 2,
    JARVIS_POWER_TIER_T3_DESTRUCTIVE = 3,
    JARVIS_POWER_TIER_T4_EXTERNAL = 4
} JarvisPowerTier;

/* payload is a NUL-terminated UTF-8 string owned by the host, read-only, valid only for the
 * duration of the call. Returns a NUL-terminated UTF-8 string the plugin allocated with
 * malloc(); the host copies it and then free()s the original — malloc/free is the one
 * allocator both sides can agree on across a dlopen boundary without a shared allocator
 * library, unlike `new`/`delete` or std::string. A null return is treated as an empty
 * string by the host. */
typedef char* (*JarvisCapabilityFn)(const char* payload);

typedef struct {
    /* Called by the plugin's jarvis_plugin_register(), once per capability, during load.
     * Currently always returns 1 — this call itself never rejects. Manifest/registration
     * mismatches (wrong count, wrong intent name, wrong power tier, etc.) are instead caught
     * afterward by the loader's cross-check once jarvis_plugin_register() returns, and reject
     * the whole plugin load as a unit at that point, not this individual registration call.
     * host_context is the opaque pointer the host passed into jarvis_plugin_register() — pass
     * it back unchanged. */
    int (*registerCapability)(
        void* host_context,
        const char* intent_name,
        const char* description,
        JarvisPowerTier power_tier,
        JarvisCapabilityFn execute);
} JarvisPluginHost;

/* Every plugin exports both of these symbols with these exact names.
 *
 * int jarvis_plugin_abi_version(void);
 *     Returns JARVIS_PLUGIN_ABI_VERSION the plugin was BUILT against (i.e. return the literal
 *     macro value, so this always tracks whatever header the plugin compiled with). The
 *     loader refuses to call jarvis_plugin_register() at all on a mismatch.
 *
 * int jarvis_plugin_register(void* host_context, const JarvisPluginHost* host);
 *     Called once at load time. Call host->registerCapability(...) once per capability this
 *     plugin provides. Returns 1 if registration succeeded, 0 on failure (the loader then
 *     refuses to activate this plugin).
 */

#ifdef __cplusplus
}
#endif

#endif /* JARVIS_PLUGIN_ABI_H */
