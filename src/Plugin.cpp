#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <array>
#include <vector>
#include <string>
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <cstdlib>
#include <algorithm>
#include "NvseAbi.h"
#include "ShotQueue.h"

static_assert(sizeof(void*) == 4, "Build for Win32/x86, not x64.");
namespace {
using U32 = std::uint32_t;
template<class T> T Read(const void* p, std::size_t offset = 0) {
    T v; std::memcpy(&v, static_cast<const unsigned char*>(p) + offset, sizeof v); return v;
}
struct Form { unsigned char opaque[24]; };
U32 ID(const Form* p) { return p ? Read<U32>(p, 12) : 0; }
unsigned char Type(const Form* p) { return p ? Read<unsigned char>(p, 4) : 0; }
bool IsActor(const Form* p) { return Type(p) == 0x3B || Type(p) == 0x3C; }

// TESModel and BSString are borrowed POD views, never constructed/destructed by
// the engine here. The engine only reads these in the two verified call sites.
struct Model {
    void* vtable;
    const char* path;
    std::uint16_t length, capacity;
    U32 unk0C; void* unk10; U32 flags;
};
static_assert(sizeof(Model) == 0x18);
Model* OriginalModel(Form* weapon) { return reinterpret_cast<Model*>(reinterpret_cast<unsigned char*>(weapon) + 0x1D0); }
struct MapEntry { MapEntry* next; U32 key; Form* value; };
struct FormMap { void* vtable; U32 buckets; MapEntry** table; U32 count; };
Form* Lookup(U32 id) {
    auto* map = *reinterpret_cast<FormMap**>(0x11C54C0);
    if (!map || !map->buckets || !map->table) return nullptr;
    for (auto* e = map->table[id % map->buckets]; e; e = e->next) if (e->key == id) return e->value;
    return nullptr;
}
int ModIndex(const std::string& name) {
    const auto* dh = *reinterpret_cast<unsigned char**>(0x11C3F2C);
    if (!dh) return -1;
    const U32 count = (std::min)(Read<U32>(dh, 0x218), 255u);
    for (U32 i = 0; i < count; ++i) {
        auto* mod = Read<unsigned char*>(dh, 0x21C + 4 * i);
        if (mod && _stricmp(reinterpret_cast<char*>(mod + 0x20), name.c_str()) == 0)
            return Read<unsigned char>(mod, 0x40C);
    }
    return -1;
}
struct Node { Form* value; Node* next; };
bool Contains(Form* list, Form* ammo, unsigned depth = 0) {
    if (Type(list) != 0x55 || depth >= 8) return false;
    auto* node = reinterpret_cast<Node*>(reinterpret_cast<unsigned char*>(list) + 0x18);
    for (unsigned n = 0; node && n < 4096; ++n, node = node->next) {
        if (node->value == ammo) return true;
        if (Type(node->value) == 0x55 && Contains(node->value, ammo, depth + 1)) return true;
    }
    return false;
}
struct Rule { bool exact{}, active{}; std::string plugin, path; U32 local{}; Form* form{}; };
std::vector<Rule> rules;
std::string root, ini, logPath;
bool enabled = true, requireLoose = true, installed = false, ready = false, verbose = true;
SRWLOCK stateLock = SRWLOCK_INIT, logLock = SRWLOCK_INIT;
dc::ShotQueue shots;
U32 epoch = 1;
struct Lock { SRWLOCK* p; explicit Lock(SRWLOCK& l):p(&l){AcquireSRWLockExclusive(p);} ~Lock(){ReleaseSRWLockExclusive(p);} };
void Log(const char* fmt, ...) {
    Lock l(logLock);
    FILE* f = std::fopen(logPath.c_str(), "a"); if (!f) return;
    std::fprintf(f, "[%llu] ", static_cast<unsigned long long>(GetTickCount64()));
    va_list args; va_start(args, fmt); std::vfprintf(f, fmt, args); va_end(args);
    std::fputc('\n', f); std::fclose(f);
}
std::string Get(const char* section, const char* key, const char* fallback) {
    char value[1024]{};
    GetPrivateProfileStringA(section, key, fallback, value, sizeof value, ini.c_str());
    return value;
}
void LoadConfig() {
    enabled = GetPrivateProfileIntA("General", "Enabled", 1, ini.c_str()) != 0;
    verbose = GetPrivateProfileIntA("General", "DebugLog", 1, ini.c_str()) != 0;
    requireLoose = GetPrivateProfileIntA("General", "RequireLooseMeshes", 1, ini.c_str()) != 0;
    shots.lifetime = (std::clamp)(GetPrivateProfileIntA("General", "PendingTimeoutMs", 30000, ini.c_str()), 1000u, 120000u);
    for (unsigned i = 1; i <= 64; ++i) {
        const std::string section = "Rule" + std::to_string(i);
        std::string kind = Get(section.c_str(), "Type", "");
        if (kind.empty()) continue;
        if (_stricmp(kind.c_str(), "Ammo") && _stricmp(kind.c_str(), "List")) { Log("Ignoring %s: invalid Type", section.c_str()); continue; }
        Rule r; r.exact = !_stricmp(kind.c_str(), "Ammo");
        r.plugin = Get(section.c_str(), "Plugin", "");
        const auto hex = Get(section.c_str(), "Form", ""); char* end = nullptr;
        const auto local = std::strtoul(hex.c_str(), &end, 16);
        r.path = Get(section.c_str(), "Model", "");
        std::replace(r.path.begin(), r.path.end(), '/', '\\');
        if (!_strnicmp(r.path.c_str(), "Meshes\\", 7)) r.path.erase(0, 7);
        if (!local || local > 0xFFFFFF || end == hex.c_str() || *end || r.plugin.empty() ||
            r.path.empty() || r.path.size() > 240 || r.path.front() == '\\' ||
            r.path.find(':') != std::string::npos || r.path.find("..") != std::string::npos ||
            r.path.size() < 4 || _stricmp(r.path.c_str() + r.path.size() - 4, ".nif")) {
            Log("Ignoring %s: invalid plugin, local FormID or mesh path", section.c_str()); continue;
        }
        r.local = static_cast<U32>(local); rules.push_back(std::move(r));
    }
}
void ResolveRules() {
    Lock l(stateLock); ready = false; shots.clear(); ++epoch;
    for (auto& r : rules) {
        r.active = false; r.form = nullptr;
        const int idx = ModIndex(r.plugin);
        if (idx < 0 || idx == 255) { Log("Missing plugin: %s", r.plugin.c_str()); continue; }
        r.form = Lookup((static_cast<U32>(idx) << 24) | r.local);
        if (Type(r.form) != (r.exact ? 0x29 : 0x55)) {
            Log("Record missing/wrong type: %s|%06X", r.plugin.c_str(), r.local); continue;
        }
        const auto full = root + "Data\\Meshes\\" + r.path;
        const DWORD attrs = GetFileAttributesA(full.c_str());
        if (attrs == INVALID_FILE_ATTRIBUTES || (attrs & FILE_ATTRIBUTE_DIRECTORY)) {
            Log("Loose mesh not found: %s%s", r.path.c_str(), requireLoose ? " (rule disabled)" : " (BSA lookup left to engine)");
            if (requireLoose) continue;
        }
        r.active = true; Log("Active %s %s|%06X -> %s", r.exact ? "AMMO" : "FLST", r.plugin.c_str(), r.local, r.path.c_str());
    }
    ready = true;
}
int SelectRule(Form* ammo) {
    if (Type(ammo) != 0x29) return -1;
    // All exact matches precede all FormLists, independent of INI ordering.
    for (std::size_t i = 0; i < rules.size(); ++i)
        if (rules[i].active && rules[i].exact && rules[i].form == ammo) return static_cast<int>(i);
    for (std::size_t i = 0; i < rules.size(); ++i)
        if (rules[i].active && !rules[i].exact && Contains(rules[i].form, ammo)) return static_cast<int>(i);
    return -1;
}
dc::Key Key(Form* actor, Form* weapon) { return {reinterpret_cast<std::uintptr_t>(actor), ID(actor), ID(weapon)}; }

using GetAmmoFn = Form* (__thiscall*)(Form*, Form*);
Form* __fastcall CaptureAmmo(Form* weapon, void*, Form* actor) {
    // This is the engine's own ammo selection, including its direct-AMMO path.
    Form* ammo = reinterpret_cast<GetAmmoFn>(0x525980)(weapon, actor);
    if (!IsActor(actor) || Type(weapon) != 0x28) return ammo;
    Lock l(stateLock);
    if (!ready) return ammo;
    const int rule = SelectRule(ammo);
    // Unmatched shots are queued too: never reuse the preceding mapped shot.
    const bool ok = shots.push(Key(actor, weapon), {ID(ammo), rule, GetTickCount64()});
    if (verbose) Log("SHOT actor=%08X weapon=%08X ammo=%08X rule=%d queued=%d", ID(actor), ID(weapon), ID(ammo), rule + 1, ok);
    return ammo;
}
struct Ejection {
    void* frame{}; Form* actor{}; Form* weapon{}; U32 generation{};
    bool replace{}; Model model{};
};
// Each entry belongs to one native EjectShellCasing stack frame. Separate copies
// prevent a nested ejection from changing another call's temporary TESModel.
thread_local std::array<Ejection, 16> contexts{};
thread_local unsigned contextIndex = 0;
Model* __cdecl BeginEjection(Form* weapon, Form* actor, void* frame) {
    Model* original = OriginalModel(weapon);
    Lock l(stateLock);
    auto& ctx = contexts[contextIndex++ % contexts.size()]; ctx = {};
    ctx.frame = frame; ctx.actor = actor; ctx.weapon = weapon; ctx.generation = epoch;
    if (!ready || !IsActor(actor)) return original;
    dc::Shot shot;
    if (!shots.pop(Key(actor, weapon), GetTickCount64(), shot)) {
        if (verbose) Log("EJECT actor=%08X weapon=%08X: no snapshot; original casing", ID(actor), ID(weapon));
        return original;
    }
    if (verbose) Log("EJECT actor=%08X weapon=%08X firedAmmo=%08X rule=%d", ID(actor), ID(weapon), shot.ammoID, shot.rule + 1);
    // Keep native suppression for weapons with no shell casing model.
    if (!original->path || !original->length || shot.rule < 0 || static_cast<std::size_t>(shot.rule) >= rules.size()) return original;
    const auto& r = rules[shot.rule]; if (!r.active) return original;
    ctx.model = *original; ctx.model.path = r.path.c_str();
    ctx.model.length = static_cast<std::uint16_t>(r.path.size());
    ctx.model.capacity = static_cast<std::uint16_t>(r.path.size() + 1);
    ctx.replace = true; return &ctx.model;
}
Model* __cdecl FinishEjectionModel(Form* weapon, Form* actor, void* frame) {
    Lock l(stateLock);
    for (unsigned n = 0; n < contexts.size(); ++n) {
        auto& c = contexts[(contextIndex - 1u - n) % contexts.size()];
        if (c.frame == frame && c.actor == actor && c.weapon == weapon && c.generation == epoch)
            return ready && c.replace ? &c.model : OriginalModel(weapon);
    }
    return OriginalModel(weapon);
}
__declspec(naked) void BeginAdapter() {
    __asm {
        push ebp
        push dword ptr [ebp+8]
        push ecx
        call BeginEjection
        add esp, 12
        ret
    }
}
__declspec(naked) void FinishAdapter() {
    __asm {
        push ebp
        push dword ptr [ebp+8]
        push ecx
        call FinishEjectionModel
        add esp, 12
        ret
    }
}
struct Patch { std::uintptr_t site; const void* target; std::array<unsigned char, 5> expected; };
bool Install() {
    const std::array<Patch, 3> patches{{
        {0x523ABE, reinterpret_cast<void*>(&CaptureAmmo), {0xE8,0xBD,0x1E,0,0}},
        {0x524E0B, reinterpret_cast<void*>(&BeginAdapter), {0xE8,0xC0,0xB2,0xF3,0xFF}},
        {0x524F31, reinterpret_cast<void*>(&FinishAdapter), {0xE8,0x9A,0xB1,0xF3,0xFF}}
    }};
    for (const auto& p : patches) if (std::memcmp(reinterpret_cast<void*>(p.site), p.expected.data(), 5)) {
        Log("NOT INSTALLED: code mismatch at %08X (runtime variant or another hook)", static_cast<U32>(p.site)); return false;
    }
    // Pre-acquire write access for all sites, so protection failure cannot leave
    // a partially installed set. Called once during xNVSE DeferredInit.
    std::array<DWORD, 3> protections{};
    for (std::size_t i = 0; i < patches.size(); ++i) {
        if (!VirtualProtect(reinterpret_cast<void*>(patches[i].site), 5, PAGE_EXECUTE_READWRITE, &protections[i])) {
            for (std::size_t j = i; j-- > 0;) { DWORD unused; VirtualProtect(reinterpret_cast<void*>(patches[j].site), 5, protections[j], &unused); }
            Log("NOT INSTALLED: VirtualProtect failed"); return false;
        }
    }
    for (const auto& p : patches) {
        std::array<unsigned char, 5> bytes{}; bytes[0] = 0xE8;
        const U32 displacement = static_cast<U32>(reinterpret_cast<std::uintptr_t>(p.target) - (p.site + 5));
        std::memcpy(bytes.data() + 1, &displacement, 4);
        std::memcpy(reinterpret_cast<void*>(p.site), bytes.data(), bytes.size());
        FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(p.site), bytes.size());
    }
    for (std::size_t i = patches.size(); i-- > 0;) { DWORD unused;
        if (!VirtualProtect(reinterpret_cast<void*>(patches[i].site), 5, protections[i], &unused)) Log("Warning: page protection restore failed at %08X", static_cast<U32>(patches[i].site));
    }
    Log("Installed three verified call-site hooks. Build 1; in-game validation required."); return true;
}
void OnMessage(Message* m) {
    if (!m) return;
    if (m->type == DeferredInit) {
        if (!enabled || installed) return;
        ResolveRules(); installed = Install();
        if (!installed) { Lock l(stateLock); ready = false; }
    } else if (m->type == PreLoadGame || m->type == ExitToMainMenu) {
        Lock l(stateLock); shots.clear(); ++epoch; ready = false;
    } else if ((m->type == LoadGame || m->type == PostLoadGame || m->type == NewGame) && installed) {
        ResolveRules();
    }
}
}

extern "C" __declspec(dllexport) bool NVSEPlugin_Query(const NVSEInterface* nvse, PluginInfo* info) {
    info->infoVersion = 1; info->name = "DiverseCasings"; info->version = 1;
    return nvse && !nvse->isEditor && !nvse->isNogore && nvse->nvseVersion >= 6 && nvse->runtimeVersion == 0x040020D0;
}
extern "C" __declspec(dllexport) bool NVSEPlugin_Load(const NVSEInterface* nvse) {
    root = nvse->GetRuntimeDirectory();
    if (!root.empty() && root.back() != '\\' && root.back() != '/') root += '\\';
    ini = root + "Data\\NVSE\\Plugins\\DiverseCasings.ini";
    logPath = root + "DiverseCasings.log";
    Log("DiverseCasings Build 1 loading; expected runtime 1.4.0.525, x86.");
    LoadConfig();
    auto* messaging = static_cast<Messaging*>(nvse->QueryInterface(2));
    if (!messaging || messaging->version < 4) { Log("xNVSE messaging v4 required"); return false; }
    return messaging->RegisterListener(nvse->GetPluginHandle(), "NVSE", OnMessage);
}
