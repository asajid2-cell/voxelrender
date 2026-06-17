#include "Simulation/HeightAtAttribution.h"

// dbghelp + windows.h are confined to this TU so the header stays free of the
// legacy `near`/`far` macros (SparseClipmap.cpp uses them as identifiers).
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <dbghelp.h>

#include <cstdio>
#include <mutex>

#pragma comment(lib, "dbghelp.lib")

namespace VENPOD::Simulation {

std::string HeightAtSymbolize(const void* addr) {
    static std::once_flag initFlag;
    std::call_once(initFlag, []() {
        SymSetOptions(SymGetOptions() | SYMOPT_LOAD_LINES | SYMOPT_DEFERRED_LOADS |
                      SYMOPT_UNDNAME);
        SymInitialize(GetCurrentProcess(), nullptr, TRUE);
    });

    const DWORD64 a = reinterpret_cast<DWORD64>(addr);
    HANDLE proc = GetCurrentProcess();

    char symBuffer[sizeof(SYMBOL_INFO) + MAX_SYM_NAME * sizeof(char)] = {};
    SYMBOL_INFO* sym = reinterpret_cast<SYMBOL_INFO*>(symBuffer);
    sym->SizeOfStruct = sizeof(SYMBOL_INFO);
    sym->MaxNameLen = MAX_SYM_NAME;

    std::string out;
    DWORD64 disp = 0;
    if (SymFromAddr(proc, a, &disp, sym)) {
        out = sym->Name;
        char off[24];
        std::snprintf(off, sizeof(off), "+0x%llx", static_cast<unsigned long long>(disp));
        out += off;
    } else {
        char hex[32];
        std::snprintf(hex, sizeof(hex), "0x%llx", static_cast<unsigned long long>(a));
        out = hex;
    }

    IMAGEHLP_LINE64 line = {};
    line.SizeOfStruct = sizeof(IMAGEHLP_LINE64);
    DWORD lineDisp = 0;
    if (SymGetLineFromAddr64(proc, a, &lineDisp, &line) && line.FileName) {
        const char* file = line.FileName;
        const char* slash = file;
        for (const char* p = file; *p; ++p) {
            if (*p == '\\' || *p == '/') {
                slash = p + 1;
            }
        }
        char loc[64];
        std::snprintf(loc, sizeof(loc), "(%s:%lu)", slash, line.LineNumber);
        out += loc;
    }
    return out;
}

}  // namespace VENPOD::Simulation
