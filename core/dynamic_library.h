#pragma once

#include <string>

// Thin OS shim over the platform's dynamic-library primitive: dlopen/dlsym/dlclose on POSIX,
// LoadLibrary/GetProcAddress/FreeLibrary on Windows. This is the ONLY place PluginLoader touches
// an OS-specific loading call — everything above this stays platform-agnostic (INV-10: this is a
// primitive wrapper, not a library hiding the concept).
namespace dynlib {

using Handle = void*;

// Loads the shared library at path. Returns nullptr on failure — check lastError() for why.
Handle open(const std::string& path);

// Resolves a symbol by name in an already-open library. Returns nullptr if not found.
void* symbol(Handle handle, const char* name);

// Unloads a library opened with open().
void close(Handle handle);

// Human-readable reason the most recent open()/symbol() call on this thread failed.
std::string lastError();

// Translates a manifest's declared library filename (written using the POSIX "lib<name>.so"
// convention, e.g. "libsystem_info_plugin.so") into the current platform's actual shared-library
// filename. On POSIX this is a no-op passthrough. On Windows it strips the "lib" prefix and
// ".so" suffix and appends ".dll" — matching the filename CMake's default SHARED library naming
// produces on each platform, so a single manifest.json needs no per-platform variants.
std::string platformLibraryFilename(const std::string& declaredName);

}  // namespace dynlib
