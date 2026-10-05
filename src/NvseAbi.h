#pragma once
#include <cstdint>
// Minimal ABI declarations, matching xNVSE PluginAPI.h. No SDK link library.
// Interface prefix only: later xNVSE fields are intentionally not accessed.
struct NVSEInterface {
    std::uint32_t nvseVersion, runtimeVersion, editorVersion, isEditor;
    bool (*RegisterCommand)(void*);
    void (*SetOpcodeBase)(std::uint32_t);
    void* (*QueryInterface)(std::uint32_t);
    std::uint32_t (*GetPluginHandle)();
    bool (*RegisterTypedCommand)(void*, std::uint32_t);
    const char* (*GetRuntimeDirectory)();
    std::uint32_t isNogore;
};
struct PluginInfo { std::uint32_t infoVersion; const char* name; std::uint32_t version; };
struct Message { const char* sender; std::uint32_t type, dataLen; void* data; };
struct Messaging {
    std::uint32_t version;
    bool (*RegisterListener)(std::uint32_t, const char*, void (*)(Message*));
    bool (*Dispatch)(std::uint32_t, std::uint32_t, void*, std::uint32_t, const char*);
};
enum : std::uint32_t {
    ExitToMainMenu = 2, LoadGame = 3, PreLoadGame = 6, PostLoadGame = 8,
    NewGame = 14, DeferredInit = 18
};
