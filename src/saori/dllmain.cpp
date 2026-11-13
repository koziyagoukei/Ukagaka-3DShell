// ukagaka3d_saori.dll - SAORI-universal bridge between a SHIORI (e.g. YAYA)
// running inside SSP and the ukagaka3d runtime process.
//
// Exports (cdecl, see ukagaka3d_saori.def):
//   BOOL    loadu(HGLOBAL h, long len)   path in UTF-8 (preferred by YAYA / SSP 2.6.92+)
//   BOOL    load(HGLOBAL h, long len)    path in the ANSI code page
//   BOOL    unload()
//   HGLOBAL request(HGLOBAL h, long *len)
// Memory: the caller allocates `h` with GlobalAlloc(GMEM_FIXED) and we free it;
// we allocate the result with GlobalAlloc(GMEM_FIXED) and the caller frees it.
#include "saori/SaoriBridge.h"

#include <windows.h>

#ifndef UKA3D_VERSION
#define UKA3D_VERSION "0.1.0-beta"
#endif

#include <cstring>
#include <memory>
#include <mutex>

namespace {

std::mutex g_mutex;
std::unique_ptr<uka3d::saori::SaoriBridge> g_bridge;

std::filesystem::path moduleDirectory() {
    HMODULE module = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&moduleDirectory), &module);
    std::wstring buffer(MAX_PATH, L'\0');
    for (;;) {
        const DWORD length = GetModuleFileNameW(module, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (length < buffer.size()) {
            buffer.resize(length);
            break;
        }
        buffer.resize(buffer.size() * 2);
    }
    return std::filesystem::path(buffer).parent_path();
}

BOOL doLoad(HGLOBAL h) {
    // The path argument is not needed: the DLL's own folder is the ghost folder.
    if (h != nullptr)
        GlobalFree(h);
    std::lock_guard lock(g_mutex);
    try {
        if (!g_bridge)
            g_bridge = std::make_unique<uka3d::saori::SaoriBridge>(moduleDirectory());
        return TRUE;
    } catch (...) {
        return FALSE;
    }
}

HGLOBAL toGlobal(const std::string &text, long *len) {
    HGLOBAL memory = GlobalAlloc(GMEM_FIXED, text.size() + 1);
    if (memory == nullptr) {
        *len = 0;
        return nullptr;
    }
    std::memcpy(memory, text.data(), text.size());
    static_cast<char *>(memory)[text.size()] = '\0';
    *len = static_cast<long>(text.size());
    return memory;
}

} // namespace

extern "C" BOOL __cdecl loadu(HGLOBAL h, long /*len*/) {
    return doLoad(h);
}

extern "C" BOOL __cdecl load(HGLOBAL h, long /*len*/) {
    return doLoad(h);
}

extern "C" BOOL __cdecl unload() {
    std::lock_guard lock(g_mutex);
    try {
        if (g_bridge)
            g_bridge->shutdownRuntime();
    } catch (...) {
    }
    g_bridge.reset();
    return TRUE;
}

extern "C" HGLOBAL __cdecl request(HGLOBAL h, long *len) {
    using namespace uka3d::saori;
    std::string raw;
    if (h != nullptr && len != nullptr && *len > 0)
        raw.assign(static_cast<const char *>(h), static_cast<std::size_t>(*len));
    if (h != nullptr)
        GlobalFree(h);
    if (len == nullptr)
        return nullptr;

    std::string error;
    const auto parsed = parseRequest(raw, error);
    if (!parsed) {
        SaoriResponse bad;
        bad.status = 400;
        bad.result = error;
        return toGlobal(formatResponse(bad, Charset::utf8, "UTF-8"), len);
    }
    SaoriResponse response;
    if (parsed->kind == SaoriRequest::Kind::getVersion) {
        response.status = 200;
        response.result = "ukagaka3d_saori/" UKA3D_VERSION;
    } else if (!parsed->securityLevel.empty() && _stricmp(parsed->securityLevel.c_str(), "local") != 0) {
        response.status = 400; // only requests from the local ghost are accepted
        response.result = "SecurityLevel が Local ではないため拒否しました";
    } else {
        // Keep the bridge alive until execute returns, even if unload runs concurrently.
        std::lock_guard lock(g_mutex);
        try {
            if (!g_bridge)
                g_bridge = std::make_unique<SaoriBridge>(moduleDirectory()); // load() not called
            response = g_bridge->execute(parsed->arguments);
        } catch (const std::exception &e) {
            response = fail(e.what(), "internal");
        }
    }
    return toGlobal(formatResponse(response, parsed->charset, parsed->charsetName), len);
}
