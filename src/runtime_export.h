#pragma once

#include <cstdint>

#include <rex/ppc/context.h>

#if defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace nhl3on3 {

using GuestFunc = void (*)(PPCContext&, uint8_t*);

// Finds the SDK runtime's implementation of a guest export that this
// executable overrides, so the override can forward to it.
inline GuestFunc ResolveRuntimeExport(const char* name) {
#if defined(_WIN32)
  for (const wchar_t* module : {L"rexruntime.dll", L"rexruntimerd.dll", L"rexruntimed.dll"}) {
    if (HMODULE handle = GetModuleHandleW(module)) {
      if (auto* fn = GetProcAddress(handle, name)) {
        return reinterpret_cast<GuestFunc>(fn);
      }
    }
  }
  return nullptr;
#else
  return reinterpret_cast<GuestFunc>(dlsym(RTLD_NEXT, name));
#endif
}

}  // namespace nhl3on3
