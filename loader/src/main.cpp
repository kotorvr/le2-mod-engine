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
struct FeatureDef
{
    const char* id;
    bool live;
    const PatchBytes* patches;
    int count;
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

static Write Put(uint8_t* at, const uint8_t* from, const uint8_t* to, uint16_t len)
{
    if (memcmp(at, to, len) == 0)
        return Write::Already;
    if (memcmp(at, from, len) != 0)
        return Write::Skipped;
    DWORD old;
    if (!VirtualProtect(at, len, PAGE_EXECUTE_READWRITE, &old))
        return Write::Skipped;
    memcpy(at, to, len);
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

// ---------------------------------------------------------------- script hook
typedef FARPROC(WINAPI* GetProcAddressFn)(HMODULE, LPCSTR);
static GetProcAddressFn g_realGetProcAddress = nullptr;

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
    if (result != nullptr && ((ULONG_PTR)name >> 16) != 0 && strcmp(name, "setup_bindings") == 0)
        OnScriptLoaded(module);
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
            int n = SetFeature(f, want, "*");
            g_applied[f] = want;
            Log("%s %s live (%d patches)", k_features[f].id, want ? "ON" : "OFF", n);
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
    for (int f = 0; f < kFeatureCount; ++f)
    {
        if (!g_want[f])
            continue;
        int n = SetFeature(f, true, nullptr);
        if (k_features[f].live)
            g_applied[f] = true;
        Log("%s on (%d exe patches now, script patches as scripts load)", k_features[f].id, n);
    }
    Log(HookExeImport() ? "script hook installed" : "script hook FAILED: script patches are off");
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
