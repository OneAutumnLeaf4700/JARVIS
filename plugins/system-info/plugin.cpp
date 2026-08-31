#include <cstdlib>
#include <cstring>
#include <sstream>
#include <string>
#include <thread>

#include "jarvis_plugin_abi.h"

namespace {

std::string operatingSystemName() {
#if defined(_WIN32)
    return "Windows";
#elif defined(__APPLE__)
    return "macOS";
#elif defined(__linux__)
    return "Linux";
#else
    return "Unknown";
#endif
}

std::string architectureName() {
#if defined(__x86_64__) || defined(_M_X64)
    return "x86_64";
#elif defined(__aarch64__) || defined(_M_ARM64)
    return "arm64";
#elif defined(__i386__) || defined(_M_IX86)
    return "x86";
#else
    return "Unknown";
#endif
}

std::string compilerName() {
#if defined(__clang__)
    return "Clang " + std::to_string(__clang_major__) + "." + std::to_string(__clang_minor__);
#elif defined(__GNUC__)
    return "GCC " + std::to_string(__GNUC__) + "." + std::to_string(__GNUC_MINOR__);
#elif defined(_MSC_VER)
    return "MSVC " + std::to_string(_MSC_VER);
#else
    return "Unknown";
#endif
}

char* systemInfoExecute(const char* /*payload*/) {
    std::ostringstream out;
    out << "System information:\n";
    out << "OS: " << operatingSystemName() << "\n";
    out << "Architecture: " << architectureName() << "\n";
    out << "Compiler: " << compilerName() << "\n";
    out << "C++ standard: " << __cplusplus << "\n";
    out << "Hardware threads: ";
    const unsigned int threadCount = std::thread::hardware_concurrency();
    if (threadCount == 0) {
        out << "unavailable";
    } else {
        out << threadCount;
    }

    const std::string text = out.str();
    char* result = static_cast<char*>(std::malloc(text.size() + 1));
    std::memcpy(result, text.c_str(), text.size() + 1);
    return result;
}

}  // namespace

extern "C" int jarvis_plugin_abi_version() {
    return JARVIS_PLUGIN_ABI_VERSION;
}

extern "C" int jarvis_plugin_register(void* host_context, const JarvisPluginHost* host) {
    return host->registerCapability(
        host_context, "system-info",
        "Shows local OS, architecture, compiler, and hardware-thread information. Usage: system-info",
        JARVIS_POWER_TIER_T0_READ_ONLY, &systemInfoExecute);
}
