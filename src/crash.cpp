// Crash reporting (see crash.h).
//
// The unhandled-exception filter does almost nothing on the crashing thread: it hands the
// exception to a fresh thread that writes the report, so it also works for stack overflows.
// Fail-fast crashes (0xC0000409, e.g. /GS or heap corruption detected by the OS) skip the
// filter entirely; for those only the Windows event log / WER has the details.
#include "crash.h"

#include <windows.h>
#include <psapi.h>
#include <dbghelp.h>

#include <cstdio>
#include <cstring>

#include "config.h"

namespace crash {
namespace {

volatile LONG g_inCrash = 0;
volatile unsigned long long g_lastFrame = 0;
EXCEPTION_POINTERS* g_ep = nullptr;
DWORD g_crashTid = 0;
LONG g_firstChance = 0;

const char* CodeName(DWORD c) {
    switch (c) {
    case EXCEPTION_ACCESS_VIOLATION: return "access violation";
    case EXCEPTION_ILLEGAL_INSTRUCTION: return "illegal instruction";
    case EXCEPTION_INT_DIVIDE_BY_ZERO: return "integer divide by zero";
    case EXCEPTION_STACK_OVERFLOW: return "stack overflow";
    case EXCEPTION_PRIV_INSTRUCTION: return "privileged instruction";
    case EXCEPTION_IN_PAGE_ERROR: return "in-page error";
    case EXCEPTION_DATATYPE_MISALIGNMENT: return "misaligned data";
    case EXCEPTION_FLT_DIVIDE_BY_ZERO: return "float divide by zero";
    case EXCEPTION_FLT_INVALID_OPERATION: return "float invalid operation";
    case EXCEPTION_BREAKPOINT: return "breakpoint";
    case 0xC0000409: return "fail-fast / stack buffer overrun";
    case 0xC0000374: return "heap corruption";
    case 0xE06D7363: return "C++ exception";
    default: return "unknown";
    }
}

// "module.dll+0x1234" for an address, or "?" when it is not inside a loaded module.
void Where(DWORD addr, char* out, size_t n) {
    HMODULE m = nullptr;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)(uintptr_t)addr, &m) && m) {
        char path[MAX_PATH] = "?";
        GetModuleFileNameA(m, path, sizeof path);
        const char* name = strrchr(path, '\\');
        snprintf(out, n, "%s+0x%lx", name ? name + 1 : path, (unsigned long)(addr - (DWORD)(uintptr_t)m));
    } else {
        snprintf(out, n, "?");
    }
}

bool Readable(const void* p, size_t len) {
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery(p, &mbi, sizeof mbi) || mbi.State != MEM_COMMIT) return false;
    if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return false;
    return (const char*)p + len <= (const char*)mbi.BaseAddress + mbi.RegionSize;
}

bool Executable(DWORD addr) {
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery((void*)(uintptr_t)addr, &mbi, sizeof mbi) || mbi.State != MEM_COMMIT) return false;
    return mbi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY);
}

// A stack value is probably a return address if it points into code right after a call.
bool LooksLikeReturn(DWORD v) {
    if (v < 0x10000 || !Executable(v) || !Readable((void*)(uintptr_t)(v - 7), 7)) return false;
    const unsigned char* p = (const unsigned char*)(uintptr_t)v;
    if (p[-5] == 0xE8) return true;                                // call rel32
    if (p[-2] == 0xFF && (p[-1] & 0x38) == 0x10) return true;      // call reg / [reg]
    if (p[-3] == 0xFF && (p[-2] & 0x38) == 0x10) return true;      // call [reg+d8]
    if (p[-6] == 0xFF && (p[-5] & 0x38) == 0x10) return true;      // call [abs] / [reg+d32]
    if (p[-7] == 0xFF && (p[-6] & 0x38) == 0x10) return true;      // call [reg*s+d32]
    return false;
}

// One or more lines ("Windows 10.0 build 19044\n...") describing the OS.
void WindowsVersion(char* out, size_t n) {
    out[0] = 0;
    size_t len = 0;
    typedef LONG(WINAPI * RtlGetVersionFn)(OSVERSIONINFOW*);
    auto fn = (RtlGetVersionFn)(void*)GetProcAddress(GetModuleHandleA("ntdll.dll"), "RtlGetVersion");
    OSVERSIONINFOW v = {};
    v.dwOSVersionInfoSize = sizeof v;
    if (fn && fn(&v) == 0)
        len += snprintf(out + len, n - len, "Windows %lu.%lu build %lu\n", v.dwMajorVersion, v.dwMinorVersion,
                        v.dwBuildNumber);
    const char* key = "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion";
    char product[128] = "", edition[64] = "", display[64] = "";
    DWORD sz = sizeof product;
    RegGetValueA(HKEY_LOCAL_MACHINE, key, "ProductName", RRF_RT_REG_SZ, nullptr, product, &sz);
    sz = sizeof edition;
    RegGetValueA(HKEY_LOCAL_MACHINE, key, "EditionID", RRF_RT_REG_SZ, nullptr, edition, &sz);
    sz = sizeof display;
    RegGetValueA(HKEY_LOCAL_MACHINE, key, "DisplayVersion", RRF_RT_REG_SZ, nullptr, display, &sz);
    if (product[0] && len < n) len += snprintf(out + len, n - len, "%s (%s) %s\n", product, edition, display);
    if (GetProcAddress(GetModuleHandleA("ntdll.dll"), "wine_get_version") && len < n)
        snprintf(out + len, n - len, "running under Wine/Proton\n");
}

void WriteReport() {
    char path[MAX_PATH];
    snprintf(path, sizeof path, "%s\\szModern-crash.txt", g_gameDir);
    FILE* f = fopen(path, "w");
    EXCEPTION_RECORD* er = g_ep->ExceptionRecord;
    CONTEXT* c = g_ep->ContextRecord;
    char where[MAX_PATH + 32];
    if (f) {
        SYSTEMTIME t;
        GetLocalTime(&t);
        fprintf(f, "Slave Zero crash report  %04d-%02d-%02d %02d:%02d:%02d\n", t.wYear, t.wMonth, t.wDay, t.wHour,
                t.wMinute, t.wSecond);
        fprintf(f, "renderer: %s\n", g_dllPath);
        char os[512];
        WindowsVersion(os, sizeof os);
        fputs(os, f);
        fprintf(f, "frames presented: %llu\n\n", g_lastFrame);

        Where((DWORD)(uintptr_t)er->ExceptionAddress, where, sizeof where);
        fprintf(f, "exception 0x%08lx (%s) at 0x%08lx  %s\n", er->ExceptionCode, CodeName(er->ExceptionCode),
                (unsigned long)(uintptr_t)er->ExceptionAddress, where);
        if (er->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && er->NumberParameters >= 2) {
            const char* op = er->ExceptionInformation[0] == 0 ? "reading" : er->ExceptionInformation[0] == 8 ? "executing" : "writing";
            fprintf(f, "  %s address 0x%08lx\n", op, (unsigned long)er->ExceptionInformation[1]);
        }
        fprintf(f, "thread %lu\n\n", g_crashTid);
        fprintf(f, "EAX=%08lx EBX=%08lx ECX=%08lx EDX=%08lx\nESI=%08lx EDI=%08lx EBP=%08lx ESP=%08lx\nEIP=%08lx EFL=%08lx\n\n",
                c->Eax, c->Ebx, c->Ecx, c->Edx, c->Esi, c->Edi, c->Ebp, c->Esp, c->Eip, c->EFlags);

        // Code bytes around EIP help tell a bad jump from a bad pointer.
        if (Readable((void*)(uintptr_t)(c->Eip - 16), 32)) {
            fprintf(f, "code at EIP-16:");
            for (int i = -16; i < 16; ++i) fprintf(f, "%s%02x", i == 0 ? " [" : " ", ((unsigned char*)(uintptr_t)c->Eip)[i]);
            fprintf(f, "\n\n");
        }

        // Likely call stack: return addresses found on the stack (heuristic, may include stale entries).
        fprintf(f, "probable call stack (stack scan):\n");
        DWORD sp = c->Esp;
        int found = 0;
        for (DWORD a = sp; found < 40 && a < sp + 64 * 1024; a += 4) {
            if (!Readable((void*)(uintptr_t)a, 4)) break;
            DWORD v = *(DWORD*)(uintptr_t)a;
            if (!LooksLikeReturn(v)) continue;
            Where(v, where, sizeof where);
            fprintf(f, "  [esp+%04lx] %08lx  %s\n", (unsigned long)(a - sp), (unsigned long)v, where);
            ++found;
        }

        fprintf(f, "\nloaded modules:\n");
        HMODULE mods[512];
        DWORD need = 0;
        if (EnumProcessModules(GetCurrentProcess(), mods, sizeof mods, &need)) {
            for (DWORD i = 0; i < need / sizeof(HMODULE) && i < 512; ++i) {
                MODULEINFO mi = {};
                char mp[MAX_PATH] = "?";
                GetModuleInformation(GetCurrentProcess(), mods[i], &mi, sizeof mi);
                GetModuleFileNameA(mods[i], mp, sizeof mp);
                fprintf(f, "  %08lx-%08lx  %s\n", (unsigned long)(uintptr_t)mi.lpBaseOfDll,
                        (unsigned long)((uintptr_t)mi.lpBaseOfDll + mi.SizeOfImage), mp);
            }
        }
        fclose(f);
    }

    // Minidump (open in Visual Studio / WinDbg). dbghelp is loaded only now.
    if (HMODULE dbg = LoadLibraryA("dbghelp.dll")) {
        typedef BOOL(WINAPI * WriteDumpFn)(HANDLE, DWORD, HANDLE, MINIDUMP_TYPE, PMINIDUMP_EXCEPTION_INFORMATION,
                                            PMINIDUMP_USER_STREAM_INFORMATION, PMINIDUMP_CALLBACK_INFORMATION);
        auto write = (WriteDumpFn)(void*)GetProcAddress(dbg, "MiniDumpWriteDump");
        snprintf(path, sizeof path, "%s\\szModern-crash.dmp", g_gameDir);
        HANDLE h = CreateFileA(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (write && h != INVALID_HANDLE_VALUE) {
            MINIDUMP_EXCEPTION_INFORMATION mei = {g_crashTid, g_ep, FALSE};
            write(GetCurrentProcess(), GetCurrentProcessId(), h,
                  (MINIDUMP_TYPE)(MiniDumpWithIndirectlyReferencedMemory | MiniDumpWithThreadInfo |
                                  MiniDumpWithUnloadedModules),
                  &mei, nullptr, nullptr);
        }
        if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
    }

    Where((DWORD)(uintptr_t)er->ExceptionAddress, where, sizeof where);
    Log("CRASH: exception 0x%08lx (%s) at %s, frame %llu - see szModern-crash.txt", er->ExceptionCode,
        CodeName(er->ExceptionCode), where, g_lastFrame);
}

DWORD WINAPI ReportThread(void*) {
    WriteReport();
    return 0;
}

LONG WINAPI Filter(EXCEPTION_POINTERS* ep) {
    if (InterlockedExchange(&g_inCrash, 1)) return EXCEPTION_CONTINUE_SEARCH;
    g_ep = ep;
    g_crashTid = GetCurrentThreadId();
    // Fresh thread with its own stack: the crashing one may have none left.
    if (HANDLE t = CreateThread(nullptr, 256 * 1024, ReportThread, nullptr, 0, nullptr)) {
        WaitForSingleObject(t, 20000);
        CloseHandle(t);
    } else {
        WriteReport();
    }
    return EXCEPTION_CONTINUE_SEARCH;  // let Windows / WER finish the crash as usual
}

// First-chance log of serious exceptions: some are handled by the game or a driver, but when
// the process later dies without a report these lines are the only trace.
LONG WINAPI FirstChance(EXCEPTION_POINTERS* ep) {
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    if (code != EXCEPTION_ACCESS_VIOLATION && code != EXCEPTION_ILLEGAL_INSTRUCTION &&
        code != EXCEPTION_STACK_OVERFLOW && code != EXCEPTION_INT_DIVIDE_BY_ZERO && code != 0xC0000374)
        return EXCEPTION_CONTINUE_SEARCH;
    if (InterlockedIncrement(&g_firstChance) > 10) return EXCEPTION_CONTINUE_SEARCH;
    if (code == EXCEPTION_STACK_OVERFLOW) {  // no stack for formatting
        Log("first-chance stack overflow");
        return EXCEPTION_CONTINUE_SEARCH;
    }
    char where[MAX_PATH + 32];
    Where((DWORD)(uintptr_t)ep->ExceptionRecord->ExceptionAddress, where, sizeof where);
    Log("first-chance exception 0x%08lx (%s) at %s, frame %llu", code, CodeName(code), where, g_lastFrame);
    return EXCEPTION_CONTINUE_SEARCH;
}

}  // namespace

void Install() {
    static bool vectored = false;
    if (!vectored) {
        AddVectoredExceptionHandler(0, FirstChance);
        vectored = true;
    }
    LPTOP_LEVEL_EXCEPTION_FILTER prev = SetUnhandledExceptionFilter(Filter);
    if (prev && prev != Filter) Log("crash handler: replaced a filter set by the game (%p)", (void*)prev);
}

void LogSystemInfo() {
    char os[512];
    WindowsVersion(os, sizeof os);
    for (char* line = strtok(os, "\n"); line; line = strtok(nullptr, "\n")) Log("%s", line);
}

void NoteFrame(unsigned long long frame) { g_lastFrame = frame; }

}  // namespace crash
