// Lone Echo II Mod Engine loader.
//
// Built as winmm.dll and placed next to loneecho2.exe (bin\win10). loneecho2.exe imports WINMM.dll statically and
// winmm is not a KnownDLL, so Windows loads this copy first, before any game code runs. Every winmm export jumps
// straight to the real System32 winmm.dll (winmm_stubs.S), so nothing else in the process notices.
//
// Inside loneecho2.exe it:
//   * applies the exe patches for every switch that's on in le2mods.ini (patches/*.json, compiled in);
//   * hooks the game's GetProcAddress import. The engine loads each compiled script DLL and asks it for
//     "setup_bindings"; at that moment the script is patched, before any of its code has run;
//   * watches le2mods.ini and applies/reverts the "live" switches (god mode) while the game runs;
//   * turns off Windows power throttling (EcoQoS) for the game process.
// Every patch checks the bytes it replaces first. A different game build simply gets "skipped" lines in the log.

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00  // Windows 10: SetProcessInformation / ProcessPowerThrottling
#endif
#include <windows.h>
#include <share.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdarg>
#include <string>

struct PatchBytes
{
    const char* module;   // nullptr = loneecho2.exe, else a script DLL file name
    uint32_t rva;
    const uint8_t* expect;
    const uint8_t* replace;
    uint16_t len;
};
// A call hook redirects one `call <callee>` (E8 rel32) in loneecho2.exe to a wrapper that calls the original and
// then writes float values into the structure passed as argument `arg` (0-3). Used where the game loads data from
// its packages through a known function, e.g. a tool's component-data serializer.
struct FloatWrite
{
    uint32_t offset;
    float value;
};
struct CallHook
{
    uint32_t site;    // rva of the E8 call instruction
    uint32_t callee;  // rva it must call (checked)
    uint8_t arg;
    const FloatWrite* writes;
    uint8_t count;
};
struct FeatureDef
{
    const char* id;
    bool live;
    const PatchBytes* patches;
    int count;
    const CallHook* hooks;
    int hookCount;
};
#include "patches_gen.inc"

static const int kFeatureCount = (int)(sizeof(k_features) / sizeof(k_features[0]));

// ---------------------------------------------------------------- winmm pass-through
struct WinmmName { const char* name; WORD ordinal; };
static const WinmmName k_winmm[] = {
#include "winmm_names.inc"
};
extern "C" void* le2_winmm_real[sizeof(k_winmm) / sizeof(k_winmm[0])];
void* le2_winmm_real[sizeof(k_winmm) / sizeof(k_winmm[0])];

static void ResolveRealWinmm()
{
    wchar_t path[MAX_PATH];
    GetSystemDirectoryW(path, MAX_PATH);
    wcscat_s(path, L"\\winmm.dll");
    HMODULE real = LoadLibraryW(path);
    for (size_t i = 0; i < sizeof(k_winmm) / sizeof(k_winmm[0]); ++i)
    {
        FARPROC p = nullptr;
        if (real != nullptr)
            p = k_winmm[i].name ? GetProcAddress(real, k_winmm[i].name)
                                : GetProcAddress(real, MAKEINTRESOURCEA(k_winmm[i].ordinal));
        le2_winmm_real[i] = (void*)p;
    }
}

// ---------------------------------------------------------------- log / paths
static wchar_t g_dir[MAX_PATH];  // bin\win10\ (with trailing backslash)
static wchar_t g_ini[MAX_PATH];
static FILE* g_log = nullptr;
static CRITICAL_SECTION g_lock;

static void Log(const char* format, ...)
{
    if (g_log == nullptr)
    {
        wchar_t path[MAX_PATH];
        wcscpy_s(path, g_dir);
        wcscat_s(path, L"le2mods.log");
        g_log = _wfsopen(path, L"w", _SH_DENYNO);
        if (g_log == nullptr)
            return;
    }
    SYSTEMTIME t;
    GetLocalTime(&t);
    fprintf(g_log, "[%02d:%02d:%02d.%03d] ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    va_list args;
    va_start(args, format);
    vfprintf(g_log, format, args);
    va_end(args);
    fputc('\n', g_log);
    fflush(g_log);
}

// ---------------------------------------------------------------- config
static bool g_logScripts = false;  // [debug] log_scripts=1: one log line per script the engine loads
static bool g_want[64];     // per feature: switched on in le2mods.ini
static bool g_applied[64];  // per live feature: its exe patches are currently in
static FILETIME g_iniTime;

// A tiny ini reader. GetPrivateProfileInt goes through the registry's ini mapping and isn't safe to call from DllMain
// (we run under the loader lock), so the file is read and parsed here: "[section]" lines and "key = value" lines,
// ';' or '#' comments. The whole file is cached per read.
static std::string g_iniText;

static void LoadIniText()
{
    g_iniText.clear();
    HANDLE h = CreateFileW(g_ini, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return;
    char buf[8192];
    DWORD got = 0;
    if (ReadFile(h, buf, sizeof(buf) - 1, &got, nullptr))
        g_iniText.assign(buf, got);
    CloseHandle(h);
    if (g_iniText.compare(0, 3, "\xEF\xBB\xBF") == 0)  // UTF-8 BOM (Notepad)
        g_iniText.erase(0, 3);
}

static bool IniFlag(const char* section, const char* key, int fallback)
{
    std::string current;
    size_t pos = 0;
    while (pos < g_iniText.size())
    {
        size_t end = g_iniText.find('\n', pos);
        if (end == std::string::npos)
            end = g_iniText.size();
        std::string line = g_iniText.substr(pos, end - pos);
        pos = end + 1;
        size_t a = line.find_first_not_of(" \t\r");
        if (a == std::string::npos || line[a] == ';' || line[a] == '#')
            continue;
        size_t b = line.find_last_not_of(" \t\r");
        line = line.substr(a, b - a + 1);
        if (line[0] == '[')
        {
            current = line.substr(1, line.find(']') == std::string::npos ? std::string::npos : line.find(']') - 1);
            continue;
        }
        size_t eq = line.find('=');
        if (eq == std::string::npos || _stricmp(current.c_str(), section) != 0)
            continue;
        std::string k = line.substr(0, line.find_last_not_of(" \t", eq - 1) + 1);
        if (_stricmp(k.c_str(), key) != 0)
            continue;
        std::string v = line.substr(eq + 1);
        v.erase(0, v.find_first_not_of(" \t"));
        return !(v.empty() || v[0] == '0' || _strnicmp(v.c_str(), "false", 5) == 0 || _strnicmp(v.c_str(), "off", 3) == 0);
    }
    return fallback != 0;
}

// "key = 1.5" from the cached ini text, or the fallback.
static float IniFloat(const char* section, const char* key, float fallback)
{
    std::string marker = std::string("[") + section + "]";
    size_t at = g_iniText.find(marker);
    if (at == std::string::npos)
        return fallback;
    size_t end = g_iniText.find("\n[", at);
    std::string body = g_iniText.substr(at, end == std::string::npos ? std::string::npos : end - at);
    size_t k = body.find(std::string("\n") + key);
    if (k == std::string::npos)
        return fallback;
    size_t eq = body.find('=', k);
    size_t eol = body.find('\n', k + 1);
    if (eq == std::string::npos || (eol != std::string::npos && eq > eol))
        return fallback;
    return (float)atof(body.c_str() + eq + 1);
}

static bool IniChanged()
{
    WIN32_FILE_ATTRIBUTE_DATA a;
    if (!GetFileAttributesExW(g_ini, GetFileExInfoStandard, &a))
        return false;
    if (CompareFileTime(&a.ftLastWriteTime, &g_iniTime) == 0)
        return false;
    g_iniTime = a.ftLastWriteTime;
    return true;
}

static void ReadConfig()
{
    LoadIniText();
    for (int i = 0; i < kFeatureCount; ++i)
        g_want[i] = IniFlag("mods", k_features[i].id, 0);
    g_logScripts = IniFlag("debug", "log_scripts", 0);
}

// ---------------------------------------------------------------- patching
enum class Write { Done, Already, Skipped };
static int g_nonAtomicWrites = 0;  // patches that straddle an 8-byte boundary (logged for live switches)

static Write Put(uint8_t* at, const uint8_t* from, const uint8_t* to, uint16_t len)
{
    if (memcmp(at, to, len) == 0)
        return Write::Already;
    if (memcmp(at, from, len) != 0)
        return Write::Skipped;
    // Only the bytes that actually change are written (god mode's 6-byte patch changes 2), which keeps most writes
    // inside one 8-byte word.
    uint16_t first = 0, last = len - 1;
    while (first < len && from[first] == to[first])
        ++first;
    while (last > first && from[last] == to[last])
        --last;
    if (first == len)
        return Write::Already;
    at += first;
    to += first;
    len = last - first + 1;
    DWORD old;
    if (!VirtualProtect(at, len, PAGE_EXECUTE_READWRITE, &old))
        return Write::Skipped;
    // Live switches patch code other threads may be running. A patch that fits inside one aligned 8-byte word is
    // written with a single atomic exchange, so no thread can ever see half of it.
    uintptr_t a = (uintptr_t)at;
    if ((a & 7) + len <= 8)
    {
        volatile LONG64* word = (volatile LONG64*)(a & ~(uintptr_t)7);
        LONG64 oldWord, newWord;
        do
        {
            oldWord = *word;
            newWord = oldWord;
            memcpy((uint8_t*)&newWord + (a & 7), to, len);
        } while (InterlockedCompareExchange64(word, newWord, oldWord) != oldWord);
    }
    else
    {
        memcpy(at, to, len);
        g_nonAtomicWrites++;
    }
    VirtualProtect(at, len, old, &old);
    FlushInstructionCache(GetCurrentProcess(), at, len);
    return Write::Done;
}

static uint8_t* ModuleBase(const PatchBytes& p)
{
    return (uint8_t*)(p.module == nullptr ? GetModuleHandleW(nullptr) : GetModuleHandleA(p.module));
}

// Apply (on=true) or revert one feature's patches that live in `module` (nullptr = exe, "*" = every loaded module).
static int SetFeature(int f, bool on, const char* module)
{
    const FeatureDef& def = k_features[f];
    const bool every = module != nullptr && strcmp(module, "*") == 0;
    int done = 0;
    for (int i = 0; i < def.count; ++i)
    {
        const PatchBytes& p = def.patches[i];
        if (!every)
        {
            if ((module == nullptr) != (p.module == nullptr))
                continue;
            if (module != nullptr && _stricmp(module, p.module) != 0)
                continue;
        }
        uint8_t* base = ModuleBase(p);
        if (base == nullptr)
            continue;  // script not loaded yet: the GetProcAddress hook catches it later
        Write w = on ? Put(base + p.rva, p.expect, p.replace, p.len) : Put(base + p.rva, p.replace, p.expect, p.len);
        if (w == Write::Skipped)
            Log("  %s: %s %s+0x%x skipped (bytes differ: other game build?)", def.id, on ? "apply" : "revert",
                p.module ? p.module : "exe", p.rva);
        else
            ++done;
    }
    return done;
}

// ---------------------------------------------------------------- call hooks
// Each installed hook gets a wrapper Wrap<I>. The E8 at the call site is rewritten to jump to a 14-byte absolute
// jump placed in a page allocated within +-2 GB of the exe (rel32 can't reach this DLL directly).
typedef uint64_t (*Fn6)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
static const int kMaxCallHooks = 32;
static const CallHook* g_hook[kMaxCallHooks];
static Fn6 g_hookOriginal[kMaxCallHooks];
static int g_hookCount = 0;

template <int I>
static uint64_t Wrap(uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t e, uint64_t f)
{
    // Arguments 5 and 6 are passed through as-is (read from the caller's stack slots) in case the callee uses them.
    uint64_t result = g_hookOriginal[I](a, b, c, d, e, f);
    const CallHook* h = g_hook[I];
    uint64_t args[4] = { a, b, c, d };
    uint8_t* object = (uint8_t*)args[h->arg & 3];
    if (object != nullptr)
        for (int i = 0; i < h->count; ++i)
            *(float*)(object + h->writes[i].offset) = h->writes[i].value;
    return result;
}

static const Fn6* WrapFns()
{
    static const Fn6 fns[kMaxCallHooks] = {
        &Wrap<0>, &Wrap<1>, &Wrap<2>, &Wrap<3>, &Wrap<4>, &Wrap<5>, &Wrap<6>, &Wrap<7>,
        &Wrap<8>, &Wrap<9>, &Wrap<10>, &Wrap<11>, &Wrap<12>, &Wrap<13>, &Wrap<14>, &Wrap<15>,
        &Wrap<16>, &Wrap<17>, &Wrap<18>, &Wrap<19>, &Wrap<20>, &Wrap<21>, &Wrap<22>, &Wrap<23>,
        &Wrap<24>, &Wrap<25>, &Wrap<26>, &Wrap<27>, &Wrap<28>, &Wrap<29>, &Wrap<30>, &Wrap<31>,
    };
    return fns;
}

static uint8_t* NearPage(uint8_t* exe)
{
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    uintptr_t gran = si.dwAllocationGranularity;
    uintptr_t base = (uintptr_t)exe & ~(gran - 1);
    for (uintptr_t step = gran; step < 0x70000000; step += gran)
    {
        if (base > step)
            if (void* p = VirtualAlloc((void*)(base - step), 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE))
                return (uint8_t*)p;
    }
    return nullptr;
}

// One executable page near the exe, shared by the call hooks and the crash guards (16 bytes per jump).
static uint8_t* page = nullptr;
static int used = 0;

static int InstallCallHooks(int f)
{
    const FeatureDef& def = k_features[f];
    if (def.hookCount == 0)
        return 0;
    uint8_t* exe = (uint8_t*)GetModuleHandleW(nullptr);
    if (page == nullptr && (page = NearPage(exe)) == nullptr)
    {
        Log("  %s: no memory near the exe for call hooks", def.id);
        return 0;
    }
    int done = 0;
    for (int i = 0; i < def.hookCount; ++i)
    {
        const CallHook& h = def.hooks[i];
        uint8_t* site = exe + h.site;
        if (site[0] != 0xE8 || site + 5 + *(int32_t*)(site + 1) != exe + h.callee)
        {
            Log("  %s: call hook at +0x%x skipped (not a call to +0x%x: other game build?)", def.id, h.site, h.callee);
            continue;
        }
        if (g_hookCount >= kMaxCallHooks || used + 16 > 4096)
        {
            Log("  %s: too many call hooks", def.id);
            break;
        }
        int slot = g_hookCount++;
        g_hook[slot] = &h;
        g_hookOriginal[slot] = (Fn6)(exe + h.callee);
        uint8_t* jump = page + used;
        used += 16;
        jump[0] = 0xFF; jump[1] = 0x25; *(uint32_t*)(jump + 2) = 0;  // jmp qword ptr [rip+0]
        *(uint64_t*)(jump + 6) = (uint64_t)WrapFns()[slot];
        uint8_t call[5] = { 0xE8 };
        *(int32_t*)(call + 1) = (int32_t)(jump - (site + 5));
        if (Put(site, site, call, 5) != Write::Skipped)  // site's current bytes are by definition "expected"
            ++done;
    }
    return done;
}

// ---------------------------------------------------------------- crash guards
// Always on (no switch), skipped with -nomods or [fixes] crash_guards=0.
//
// Faraday field target: the field lookup 0x140acc350 (used by CR16FaradayProjectorFieldExpression 0x140cf55b0, and by
// 0x140cf4a10) checks an active field's target component ref with 0x140148e60 -> 0x14013c260. That resolver only
// treats a null world pointer as empty, so a ref left at the -1 template (field slot active, target never set) is
// dereferenced: AV reading 0x217f at 0x14013c283. It happened with the faraday projector on biomass (3 dumps,
// 2026-11-10, the last one with the faraday max-upgrade patches already removed). The guard answers "nonzero" for
// such a ref, which sends 0x140acc350 down its own "no target" path (out = -1, the same as an inactive slot).
namespace Guards
{
    typedef uint32_t (*RefCheckFn)(uint64_t*);
    static const uint32_t kFieldRefCallSite = 0xacc3ad;  // call 0x140148e60 inside 0x140acc350
    static const uint32_t kRefCheck = 0x148e60;
    static RefCheckFn g_refCheck = nullptr;
    static volatile LONG g_fieldRefHits = 0;

    static uint32_t FieldRefCheck(uint64_t* ref)
    {
        if (ref != nullptr && *ref == ~0ull)
        {
            LONG n = InterlockedIncrement(&g_fieldRefHits);
            if (n == 1 || n == 10 || n == 100 || n % 1000 == 0)
                Log("crash guard: faraday field with an invalid target skipped (%ld so far)", n);
            return 1;
        }
        return g_refCheck(ref);
    }

    static bool Redirect(uint8_t* exe, uint32_t siteRva, uint32_t calleeRva, void* target)
    {
        uint8_t* site = exe + siteRva;
        if (site[0] != 0xE8 || site + 5 + *(int32_t*)(site + 1) != exe + calleeRva)
            return false;
        if (page == nullptr && (page = NearPage(exe)) == nullptr)
            return false;
        if (used + 16 > 4096)
            return false;
        uint8_t* jump = page + used;
        used += 16;
        jump[0] = 0xFF; jump[1] = 0x25; *(uint32_t*)(jump + 2) = 0;  // jmp qword ptr [rip+0]
        *(uint64_t*)(jump + 6) = (uint64_t)target;
        uint8_t call[5] = { 0xE8 };
        *(int32_t*)(call + 1) = (int32_t)(jump - (site + 5));
        return Put(site, site, call, 5) != Write::Skipped;
    }

    static void Install(uint8_t* exe)
    {
        g_refCheck = (RefCheckFn)(exe + kRefCheck);
        if (Redirect(exe, kFieldRefCallSite, kRefCheck, (void*)&FieldRefCheck))
            Log("crash guard: faraday field target check on");
        else
            Log("crash guard: faraday field call site differs (other game build?): off");
    }
}

// ---------------------------------------------------------------- script hook
typedef FARPROC(WINAPI* GetProcAddressFn)(HMODULE, LPCSTR);
static GetProcAddressFn g_realGetProcAddress = nullptr;

// ---------------------------------------------------------------- Oculus runtime wrappers
// The game resolves every Oculus function through the GetProcAddress import we already hook, so it can be handed a
// wrapper instead. Only used on the Oculus runtime (Air Link / Link), never in OpenXR mode.
namespace Ovr
{
    struct HapticsBuffer
    {
        const void* samples;
        int count;
        int submitMode;
    };
    typedef int (*SubmitVibrationFn)(void*, int, const HapticsBuffer*);
    typedef int (*SetVibrationFn)(void*, int, float, float);
    typedef int (*GetSessionStatusFn)(void*, uint8_t*);

    static bool g_questHaptics = false;
    static bool g_keepRunningInDash = false;
    static float g_hapticStrength = 1.5f;
    static SetVibrationFn g_setVibration = nullptr;
    static GetSessionStatusFn g_getSessionStatus = nullptr;

    // Quest controllers ignore buffered haptics over Link, and the game only ever uses the buffered call. So each
    // frame's buffer is averaged into one amplitude and sent with ovr_SetControllerVibration. That call is only made
    // when the level changes, plus a keep-alive before the runtime's own time-out.
    static int SubmitVibration(void* session, int type, const HapticsBuffer* buffer)
    {
        static float s_last[2] = { 0.0f, 0.0f };
        static ULONGLONG s_sent[2] = { 0, 0 };
        static bool s_logged[2] = { false, false };
        if (g_setVibration == nullptr || (type != 1 && type != 2))
            return 0;
        float amplitude = 0.0f;
        if (buffer != nullptr && buffer->samples != nullptr && buffer->count > 0)
        {
            const uint8_t* samples = (const uint8_t*)buffer->samples;
            unsigned total = 0;
            for (int i = 0; i < buffer->count; ++i)
                total += samples[i];
            amplitude = (float)total / (float)buffer->count / 255.0f * g_hapticStrength;
            amplitude = amplitude > 1.0f ? 1.0f : amplitude < 0.01f ? 0.0f : amplitude;
        }
        int hand = type - 1;
        ULONGLONG now = GetTickCount64();
        float change = amplitude - s_last[hand];
        bool keepAlive = amplitude > 0.0f && now - s_sent[hand] > 500;
        if (change > -0.02f && change < 0.02f && !keepAlive && !(amplitude == 0.0f && s_last[hand] != 0.0f))
            return 0;
        int result = g_setVibration(session, type, amplitude > 0.0f ? 1.0f : 0.0f, amplitude);
        s_last[hand] = amplitude;
        s_sent[hand] = now;
        if (amplitude > 0.0f && !s_logged[hand])
        {
            s_logged[hand] = true;
            Log("haptics: first %s hand vibration, amplitude %.2f, result %d", hand ? "right" : "left", amplitude, result);
        }
        return 0;
    }

    // ovrSessionStatus bytes: 0 IsVisible, 1 HmdPresent, 2 HmdMounted, 3 DisplayLost, 4 ShouldQuit, 5 ShouldRecenter,
    // 6 HasInputFocus, 7 OverlayPresent, 8 DepthRequested.
    static int GetSessionStatus(void* session, uint8_t* status)
    {
        static uint8_t s_previous[9] = { 0xff };
        int result = g_getSessionStatus(session, status);
        if (result < 0 || status == nullptr)
            return result;
        if (memcmp(s_previous, status, sizeof(s_previous)) != 0)
        {
            memcpy(s_previous, status, sizeof(s_previous));
            Log("oculus status: visible %d mounted %d focus %d overlay %d quit %d", status[0], status[2], status[6],
                status[7], status[4]);
        }
        status[6] = 1;  // HasInputFocus: the Dash doesn't take the game's focus away
        status[7] = 0;  // OverlayPresent: no overlay as far as the game can tell
        return result;
    }

    static FARPROC Wrap(HMODULE module, LPCSTR name, FARPROC real)
    {
        if (g_questHaptics && strcmp(name, "ovr_SubmitControllerVibration") == 0)
        {
            g_setVibration = (SetVibrationFn)g_realGetProcAddress(module, "ovr_SetControllerVibration");
            if (g_setVibration != nullptr)
            {
                Log("haptics: buffered vibration redirected to ovr_SetControllerVibration (strength %.2f)",
                    g_hapticStrength);
                return (FARPROC)&SubmitVibration;
            }
        }
        if (g_keepRunningInDash && strcmp(name, "ovr_GetSessionStatus") == 0)
        {
            g_getSessionStatus = (GetSessionStatusFn)real;
            Log("dash: game keeps running behind the Oculus Dash");
            return (FARPROC)&GetSessionStatus;
        }
        return real;
    }
}

static void OnScriptLoaded(HMODULE module)
{
    char path[MAX_PATH];
    if (GetModuleFileNameA(module, path, MAX_PATH) == 0)
        return;
    const char* name = strrchr(path, '\\');
    name = name ? name + 1 : path;
    static LONG s_loaded = 0;
    LONG n = InterlockedIncrement(&s_loaded);
    if (g_logScripts)
        Log("script #%ld: %s", n, name);
    EnterCriticalSection(&g_lock);
    for (int f = 0; f < kFeatureCount; ++f)
    {
        if (!g_want[f])
            continue;
        for (int i = 0; i < k_features[f].count; ++i)
        {
            const PatchBytes& p = k_features[f].patches[i];
            if (p.module != nullptr && _stricmp(p.module, name) == 0)
            {
                int n = SetFeature(f, true, name);
                Log("script %s loaded: %s (%d patches)", name, k_features[f].id, n);
                break;
            }
        }
    }
    LeaveCriticalSection(&g_lock);
}

static FARPROC WINAPI HookGetProcAddress(HMODULE module, LPCSTR name)
{
    FARPROC result = g_realGetProcAddress(module, name);
    if (result == nullptr || ((ULONG_PTR)name >> 16) == 0)
        return result;
    if (strcmp(name, "setup_bindings") == 0)
        OnScriptLoaded(module);
    else if (name[0] == 'o' && name[1] == 'v' && name[2] == 'r' && name[3] == '_')
        return Ovr::Wrap(module, name, result);
    return result;
}

// Point loneecho2.exe's KERNEL32!GetProcAddress import at HookGetProcAddress.
static bool HookExeImport()
{
    uint8_t* exe = (uint8_t*)GetModuleHandleW(nullptr);
    auto* dos = (IMAGE_DOS_HEADER*)exe;
    auto* nt = (IMAGE_NT_HEADERS64*)(exe + dos->e_lfanew);
    const IMAGE_DATA_DIRECTORY& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    for (auto* d = (IMAGE_IMPORT_DESCRIPTOR*)(exe + dir.VirtualAddress); d->Name != 0; ++d)
    {
        if (_stricmp((const char*)(exe + d->Name), "KERNEL32.dll") != 0)
            continue;
        auto* names = (IMAGE_THUNK_DATA64*)(exe + d->OriginalFirstThunk);
        auto* iat = (IMAGE_THUNK_DATA64*)(exe + d->FirstThunk);
        for (; names->u1.AddressOfData != 0; ++names, ++iat)
        {
            if (IMAGE_SNAP_BY_ORDINAL64(names->u1.Ordinal))
                continue;
            auto* byName = (IMAGE_IMPORT_BY_NAME*)(exe + names->u1.AddressOfData);
            if (strcmp((const char*)byName->Name, "GetProcAddress") != 0)
                continue;
            g_realGetProcAddress = (GetProcAddressFn)iat->u1.Function;
            DWORD old;
            VirtualProtect(&iat->u1.Function, sizeof(ULONGLONG), PAGE_READWRITE, &old);
            iat->u1.Function = (ULONGLONG)&HookGetProcAddress;
            VirtualProtect(&iat->u1.Function, sizeof(ULONGLONG), old, &old);
            return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------- all gadgets (runtime)
// The story unlocks MageHand, Faraday Projector and Cure Gun with one EnableComponentNode each (r16_player_upgrade's
// upg_magehand_initial / upg_faraday_initial / upg_curegun_unlock): actor "player", enable = 1. We make the same
// engine call from the game thread, by wrapping CR16Game::Update (vtable +0x1a0). The engine saves the result.
namespace Gadgets
{
    const uint32_t kVtableSlot = 0x14e3020;  // CR16Game vtable +0x1a0 (rva)
    const uint32_t kUpdate = 0x148640;       // its original target (rva), checked before swapping
    const uint32_t kFindActor = 0x4ae730;    // bool (space, actorHash, CGameSpace** spaceOut, uint64* ref)
    const uint32_t kGetSystem = 0x128760;    // CS* (space, typeHash)
    const uint32_t kEnable = 0x7064e0;       // (cs, uint16 componentIndex, int enable) - queued, safe mid-frame
    const uint64_t kPlayer = 0x41d2d7311b3a020cULL;
    const struct { uint64_t type; const char* name; } kTools[] = {
        { 0x9a0ec0a12a0d5418ULL, "MageHand" },
        { 0x530551356665db1eULL, "FaradayProjector" },
        { 0xe86b0250046a3130ULL, "CureGunTool" },
    };

    typedef uint64_t (*UpdateFn)(uint64_t, uint64_t, uint64_t, uint64_t);
    typedef int (*FindActorFn)(void*, uint64_t, void**, uint64_t*);
    typedef void* (*GetSystemFn)(void*, uint64_t);
    typedef uint32_t* (*FindComponentFn)(void*, uint32_t*, uint64_t, uint64_t);
    typedef void (*EnableFn)(void*, uint16_t, int);

    static uint8_t* g_exe;
    static UpdateFn g_original;
    static unsigned g_calls = 0;
    static uint32_t g_phasesSeen = 0;   // bit per phase value 0..31 seen in the first calls
    static void* g_doneFor = nullptr;   // Jack's space when the tools were last granted
    static uint64_t g_doneCount = ~0ULL;

    static void Grant(uint8_t* game)
    {
        uint64_t count = *(uint64_t*)(game + 0x1fc8);
        void** pairs = *(void***)(game + 0x1f98);
        if (pairs == nullptr || count == 0 || count > 256)
            return;
        auto findActor = (FindActorFn)(g_exe + kFindActor);
        for (uint64_t i = 0; i < count; ++i)
        {
            void* space = pairs[i * 2 + 1];
            if (space == nullptr)
                continue;
            void* jackSpace = space;
            uint64_t ref = 0;
            if (!findActor(space, kPlayer, &jackSpace, &ref) || (uint16_t)ref == 0xffff || jackSpace == nullptr)
                continue;
            if (jackSpace == g_doneFor && count == g_doneCount)
                return;  // already granted in this set of loaded levels
            int enabled = 0;
            for (const auto& tool : kTools)
            {
                void* cs = ((GetSystemFn)(g_exe + kGetSystem))(jackSpace, tool.type);
                if (cs == nullptr)
                    continue;
                uint32_t out = 0xffffffff;
                auto find = (FindComponentFn)((*(void***)cs)[0x1a8 / 8]);
                uint32_t* found = find(cs, &out, ref, tool.type);
                uint16_t idx = (uint16_t)(found ? *found : 0xffff);
                if (idx == 0xffff)
                {
                    Log("all_gadgets: Jack has no %s component here", tool.name);
                    continue;
                }
                ((EnableFn)(g_exe + kEnable))(cs, idx, 1);
                ++enabled;
                Log("all_gadgets: %s enabled (component %u)", tool.name, idx);
            }
            g_doneFor = jackSpace;
            g_doneCount = count;
            if (enabled == 0)
                Log("all_gadgets: Jack found but no tool components: will retry after the next level load");
            return;
        }
    }

    static uint64_t HookedUpdate(uint64_t game, uint64_t phase, uint64_t r8, uint64_t r9)
    {
        uint64_t result = g_original(game, phase, r8, r9);
        ++g_calls;
        if (g_calls <= 300 && phase < 32 && !(g_phasesSeen & (1u << phase)))
        {
            g_phasesSeen |= 1u << phase;
            Log("all_gadgets: CR16Game::Update phase %llu seen", (unsigned long long)phase);
        }
        if (phase == 1 && game != 0 && g_calls % 120 == 0)
            Grant((uint8_t*)game);
        return result;
    }

    static void Install(uint8_t* exe)
    {
        g_exe = exe;
        auto* slot = (volatile LONG64*)(exe + kVtableSlot);
        LONG64 expected = (LONG64)(exe + kUpdate);
        if (*slot != expected)
        {
            Log("all_gadgets: CR16Game::Update slot differs (other game build?): off");
            return;
        }
        g_original = (UpdateFn)(exe + kUpdate);
        DWORD old;
        VirtualProtect((void*)slot, 8, PAGE_READWRITE, &old);
        InterlockedExchange64(slot, (LONG64)&HookedUpdate);
        VirtualProtect((void*)slot, 8, old, &old);
        Log("all_gadgets: on (tools are granted once Jack is in a level; the game saves them)");
    }
}

// ---------------------------------------------------------------- live switches
static DWORD WINAPI WatchThread(LPVOID)
{
    for (;;)
    {
        Sleep(250);
        if (!IniChanged())
            continue;
        EnterCriticalSection(&g_lock);
        LoadIniText();
        for (int f = 0; f < kFeatureCount; ++f)
        {
            if (!k_features[f].live)
                continue;
            bool want = IniFlag("mods", k_features[f].id, 0);
            if (want == g_applied[f])
                continue;
            g_want[f] = want;
            int before = g_nonAtomicWrites;
            int n = SetFeature(f, want, "*");
            g_applied[f] = want;
            Log("%s %s live (%d patches%s)", k_features[f].id, want ? "ON" : "OFF", n,
                g_nonAtomicWrites != before ? ", some not atomic" : "");
        }
        LeaveCriticalSection(&g_lock);
    }
}

// ---------------------------------------------------------------- start-up
static void DisablePowerThrottling()
{
    PROCESS_POWER_THROTTLING_STATE s = {};
    s.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
    s.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED;
    s.StateMask = 0;  // controlled, and off
    if (SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &s, sizeof(s)))
        Log("power throttling (EcoQoS) off for loneecho2.exe");
}

// OpenXR mode: the game's LibOVR shim loads LibOVRRT64_1.dll from LIBOVR_DLL_DIR first. lone-echo-openxr's runtime in
// bin\win10\openxr answers those calls through OpenXR, so the game runs on VDXR (Virtual Desktop) or SteamVR with no
// Oculus runtime in the path. We run before the game's CRT starts, so it sees these variables.
static void SetUpOpenXr()
{
    int feature = -1;
    for (int f = 0; f < kFeatureCount; ++f)
        if (strcmp(k_features[f].id, "openxr_runtime") == 0)
            feature = f;
    if (feature < 0 || !g_want[feature])
        return;
    wchar_t dir[MAX_PATH], dll[MAX_PATH];
    wcscpy_s(dir, g_dir);
    wcscat_s(dir, L"openxr\\");
    wcscpy_s(dll, dir);
    wcscat_s(dll, L"LibOVRRT64_1.dll");
    if (GetFileAttributesW(dll) == INVALID_FILE_ATTRIBUTES)
    {
        Log("openxr: %ls is missing: staying on the Oculus runtime", dll);
        g_want[feature] = false;
        return;
    }
    SetEnvironmentVariableW(L"LIBOVR_DLL_DIR", dir);
    SetDllDirectoryW(dir);  // the runtime finds openxr_loader.dll by plain DLL search
    if (IniFlag("vr", "vdxr", 0))
    {
        wchar_t json[MAX_PATH];
        DWORD n = GetEnvironmentVariableW(L"ProgramW6432", json, MAX_PATH);
        if (n > 0 && n < MAX_PATH - 80)
        {
            wcscat_s(json, L"\\Virtual Desktop Streamer\\OpenXR\\virtualdesktop-openxr.json");
            if (GetFileAttributesW(json) != INVALID_FILE_ATTRIBUTES)
            {
                SetEnvironmentVariableW(L"XR_RUNTIME_JSON", json);
                Log("openxr: using Virtual Desktop's VDXR runtime");
            }
            else
                Log("openxr: VDXR not found: using the system OpenXR runtime");
        }
    }
    else
        Log("openxr: using the system OpenXR runtime");
    if (IniFlag("debug", "openxr_trace", 0))
        SetEnvironmentVariableW(L"LIBOVR_OPENXR_LOG", L"buffered");  // per-frame trace, written at a clean quit
    else if (IniFlag("debug", "openxr_log", 0))
        SetEnvironmentVariableW(L"LIBOVR_OPENXR_LOG", L"stencil");  // startup/shutdown markers, no per-call cost
}

static void Start()
{
    InitializeCriticalSection(&g_lock);
    uint8_t* exe = (uint8_t*)GetModuleHandleW(nullptr);
    auto* nt = (IMAGE_NT_HEADERS64*)(exe + ((IMAGE_DOS_HEADER*)exe)->e_lfanew);
    Log("Lone Echo II Mod Engine loader, loneecho2.exe timestamp %08lX%s", nt->FileHeader.TimeDateStamp,
        nt->FileHeader.TimeDateStamp == 0x61578052 ? " (supported build)" : " (UNKNOWN build: patches are byte-checked)");
    if (wcsstr(GetCommandLineW(), L"-nomods") != nullptr)
    {
        Log("-nomods on the command line: nothing applied");
        return;
    }
    IniChanged();  // remember the current timestamp
    ReadConfig();
    Log("config %ls: %zu bytes", g_ini, g_iniText.size());
    SetUpOpenXr();
    bool openXr = false;
    for (int f = 0; f < kFeatureCount; ++f)
        openXr = openXr || (strcmp(k_features[f].id, "openxr_runtime") == 0 && g_want[f]);
    Ovr::g_questHaptics = !openXr && IniFlag("vr", "quest_haptics", 1);
    Ovr::g_keepRunningInDash = !openXr && IniFlag("vr", "keep_running_in_dash", 1);
    float strength = IniFloat("vr", "haptic_strength", 1.5f);
    Ovr::g_hapticStrength = strength < 0.1f ? 0.1f : strength > 5.0f ? 5.0f : strength;
    for (int f = 0; f < kFeatureCount; ++f)
    {
        if (!g_want[f])
            continue;
        int n = SetFeature(f, true, nullptr);
        int hooks = InstallCallHooks(f);
        if (k_features[f].live)
            g_applied[f] = true;
        if (hooks)
            Log("%s on (%d exe patches, %d call hooks)", k_features[f].id, n, hooks);
        else
            Log("%s on (%d exe patches now, script patches as scripts load)", k_features[f].id, n);
    }
    if (IniFlag("fixes", "crash_guards", 1))
        Guards::Install(exe);
    Log(HookExeImport() ? "script hook installed" : "script hook FAILED: script patches are off");
    if (IniFlag("mods", "all_gadgets", 0))
        Gadgets::Install(exe);
    if (IniFlag("perf", "no_power_throttling", 1))
        DisablePowerThrottling();
    HANDLE t = CreateThread(nullptr, 0, WatchThread, nullptr, 0, nullptr);
    if (t != nullptr)
        CloseHandle(t);
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID)
{
    if (reason != DLL_PROCESS_ATTACH)
        return TRUE;
    DisableThreadLibraryCalls(module);
    ResolveRealWinmm();
    wchar_t exePath[MAX_PATH];
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    wchar_t* file = wcsrchr(exePath, L'\\');
    if (file == nullptr || _wcsicmp(file + 1, L"loneecho2.exe") != 0)
        return TRUE;  // another program (the crash reporter) loaded it: pass-through only
    wcsncpy_s(g_dir, exePath, file - exePath + 1);
    wcscpy_s(g_ini, g_dir);
    wcscat_s(g_ini, L"le2mods.ini");
    Start();
    return TRUE;
}
