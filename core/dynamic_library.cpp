#include "dynamic_library.h"

#if defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace dynlib {

Handle open(const std::string& path) {
#if defined(_WIN32)
    return reinterpret_cast<Handle>(LoadLibraryA(path.c_str()));
#else
    return dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
#endif
}

void* symbol(Handle handle, const char* name) {
#if defined(_WIN32)
    return reinterpret_cast<void*>(GetProcAddress(reinterpret_cast<HMODULE>(handle), name));
#else
    return dlsym(handle, name);
#endif
}

void close(Handle handle) {
#if defined(_WIN32)
    FreeLibrary(reinterpret_cast<HMODULE>(handle));
#else
    dlclose(handle);
#endif
}

std::string lastError() {
#if defined(_WIN32)
    return "LoadLibrary/GetProcAddress failed, GetLastError()=" + std::to_string(GetLastError());
#else
    const char* err = dlerror();
    return err ? err : "unknown dynamic-library error";
#endif
}

std::string platformLibraryFilename(const std::string& declaredName) {
#if defined(_WIN32)
    std::string name = declaredName;
    const std::string soSuffix = ".so";
    if (name.size() >= soSuffix.size() &&
        name.compare(name.size() - soSuffix.size(), soSuffix.size(), soSuffix) == 0) {
        name.resize(name.size() - soSuffix.size());
    }
    if (name.rfind("lib", 0) == 0) {
        name = name.substr(3);
    }
    return name + ".dll";
#else
    return declaredName;
#endif
}

}  // namespace dynlib
