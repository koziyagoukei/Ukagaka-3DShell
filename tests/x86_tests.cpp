// x86 tests: run in a 32-bit process like SSP.
//   uka3d_x86_tests saori <ukagaka3d_saori.dll> <runtime.exe> <test-assets>
//   uka3d_x86_tests yaya  <yaya.dll> <ukagaka3d_saori.dll> <runtime.exe> <test-assets> <ghost-src>
//
// "saori": loads the x86 SAORI DLL and drives the x64 runtime through it.
// "yaya" : assembles the sample ghost in a temp folder, loads the real
//          yaya.dll and sends SHIORI/3.0 requests as SSP would
//          (OnBoot -> menu choices -> OnClose), checking that the 3D runtime
//          starts, reacts and stops.
#include <windows.h>

#include <tlhelp32.h>

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

int g_failures = 0;
int g_checks = 0;

void report(bool ok, const char *expr, int line) {
    ++g_checks;
    if (!ok) {
        ++g_failures;
        std::printf("  FAILED: %s (line %d)\n", expr, line);
    }
}
#define CHECK(expr) report(static_cast<bool>(expr), #expr, __LINE__)

std::string utf8(const std::wstring &w) {
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<std::size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

// Child processes named ukagaka3d.exe whose parent is this process.
std::vector<DWORD> runtimeChildren() {
    std::vector<DWORD> result;
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    for (BOOL ok = Process32FirstW(snapshot, &entry); ok; ok = Process32NextW(snapshot, &entry)) {
        if (entry.th32ParentProcessID == GetCurrentProcessId() && _wcsicmp(entry.szExeFile, L"ukagaka3d.exe") == 0)
            result.push_back(entry.th32ProcessID);
    }
    CloseHandle(snapshot);
    return result;
}

bool waitNoChildren(DWORD timeoutMs) {
    const ULONGLONG deadline = GetTickCount64() + timeoutMs;
    while (GetTickCount64() < deadline) {
        if (runtimeChildren().empty())
            return true;
        Sleep(50);
    }
    return runtimeChildren().empty();
}

struct Module {
    using LoadFn = BOOL(__cdecl *)(HGLOBAL, long);
    using UnloadFn = BOOL(__cdecl *)();
    using RequestFn = HGLOBAL(__cdecl *)(HGLOBAL, long *);
    HMODULE handle{};
    LoadFn loadu{};
    UnloadFn unload{};
    RequestFn request{};

    bool open(const fs::path &path) {
        handle = LoadLibraryW(path.c_str());
        if (!handle)
            return false;
        loadu = reinterpret_cast<LoadFn>(GetProcAddress(handle, "loadu"));
        unload = reinterpret_cast<UnloadFn>(GetProcAddress(handle, "unload"));
        request = reinterpret_cast<RequestFn>(GetProcAddress(handle, "request"));
        return loadu && unload && request;
    }
    BOOL load(const std::string &dirUtf8) const {
        HGLOBAL h = GlobalAlloc(GMEM_FIXED, dirUtf8.size());
        std::memcpy(h, dirUtf8.data(), dirUtf8.size());
        return loadu(h, static_cast<long>(dirUtf8.size()));
    }
    std::string call(const std::string &text) const {
        HGLOBAL h = GlobalAlloc(GMEM_FIXED, text.size());
        std::memcpy(h, text.data(), text.size());
        long len = static_cast<long>(text.size());
        HGLOBAL out = request(h, &len);
        if (!out)
            return {};
        std::string result(static_cast<const char *>(out), static_cast<std::size_t>(len));
        GlobalFree(out);
        return result;
    }
};

std::string saori(const Module &m, const std::vector<std::string> &args) {
    std::string text = "EXECUTE SAORI/1.0\r\nCharset: UTF-8\r\nSender: test\r\nSecurityLevel: Local\r\n";
    for (std::size_t i = 0; i < args.size(); ++i)
        text += "Argument" + std::to_string(i) + ": " + args[i] + "\r\n";
    return m.call(text + "\r\n");
}

std::string shiori(const Module &m, const char *method, const std::string &id,
                   const std::vector<std::string> &refs = {}) {
    std::string text = std::string(method) + " SHIORI/3.0\r\nCharset: UTF-8\r\nSender: SSP\r\nSecurityLevel: local\r\n";
    text += "ID: " + id + "\r\n";
    for (std::size_t i = 0; i < refs.size(); ++i)
        text += "Reference" + std::to_string(i) + ": " + refs[i] + "\r\n";
    return m.call(text + "\r\n");
}

void copyTree(const fs::path &from, const fs::path &to) {
    fs::create_directories(to);
    fs::copy(from, to, fs::copy_options::recursive | fs::copy_options::overwrite_existing);
}

// Lays out a ghost folder the same way scripts/package-ghost.ps1 does.
fs::path assembleGhost(const fs::path &work, const fs::path &ghostSrc, const fs::path &yaya, const fs::path &saoriDll,
                       const fs::path &runtime, const fs::path &assets) {
    const fs::path root = work / L"ukagaka3d_sample";
    const fs::path master = root / L"ghost" / L"master";
    fs::create_directories(master);
    if (!ghostSrc.empty()) {
        copyTree(ghostSrc, root);
        // The distribution uses Uka. Keep the automated fixture offline and synthetic.
        const auto configPath = master / L"ukagaka3d.json";
        std::string config;
        {
            std::ifstream input(configPath, std::ios::binary);
            config.assign(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
        }
        const std::string ukaPath = "3d/model/uka/MS_Uka.pmx";
        const auto offset = config.find(ukaPath);
        CHECK(offset != std::string::npos);
        if (offset != std::string::npos)
            config.replace(offset, ukaPath.size(), "3d/model/物理テストキャラ.pmx");
        std::ofstream(configPath, std::ios::binary) << config;
    }
    if (!yaya.empty())
        fs::copy_file(yaya, master / L"yaya.dll", fs::copy_options::overwrite_existing);
    fs::copy_file(saoriDll, master / L"ukagaka3d_saori.dll", fs::copy_options::overwrite_existing);
    fs::create_directories(master / L"3d" / L"motion");
    fs::copy_file(runtime, master / L"3d" / L"ukagaka3d.exe", fs::copy_options::overwrite_existing);
    copyTree(assets / L"テスト用モデル", master / L"3d" / L"model");
    copyTree(assets / L"accessories", master / L"3d" / L"accessories");
    fs::copy_file(assets / L"モーション" / L"挨拶.vmd", master / L"3d" / L"motion" / L"greet.vmd",
                  fs::copy_options::overwrite_existing);
    fs::copy_file(assets / L"モーション" / L"うなずき.vmd", master / L"3d" / L"motion" / L"nod.vmd",
                  fs::copy_options::overwrite_existing);
    fs::copy_file(assets / L"モーション" / L"待機テスト.vmd", master / L"3d" / L"motion" / L"idle.vmd",
                  fs::copy_options::overwrite_existing);
    fs::copy_file(assets / L"モーション" / L"歩行.vmd", master / L"3d" / L"motion" / L"walk.vmd",
                  fs::copy_options::overwrite_existing);
    fs::copy_file(assets / L"モーション" / L"着席.vmd", master / L"3d" / L"motion" / L"sit.vmd",
                  fs::copy_options::overwrite_existing);
    fs::copy_file(assets / L"モーション" / L"つまみ.vmd", master / L"3d" / L"motion" / L"grab.vmd",
                  fs::copy_options::overwrite_existing);
    return master;
}

void testSaori(const fs::path &dll, const fs::path &runtime, const fs::path &assets, const fs::path &work) {
    const fs::path master = assembleGhost(work, fs::path(), fs::path(), dll, runtime, assets);
    std::ofstream(master / L"ukagaka3d.json", std::ios::binary)
        << std::string(R"({"model": "3d/model/物理テストキャラ.pmx", "window": {"width": 160, "height": 240}, "fps": 30})");
    Module m;
    CHECK(m.open(master / L"ukagaka3d_saori.dll"));
    if (!m.handle)
        return;
    CHECK(m.load(utf8(master.native()) + "\\"));
    CHECK(m.call("GET Version SAORI/1.0\r\nCharset: UTF-8\r\n\r\n").starts_with("SAORI/1.0 200 OK"));
    std::string r = saori(m, {"launch"});
    std::printf("    launch: %s\n", r.substr(0, r.find("\r\n\r\n")).c_str());
    CHECK(r.find("Result: 1") != std::string::npos);
    CHECK(runtimeChildren().size() == 1);

    CHECK(saori(m, {"play_motion", "greet", "0", "idle"}).find("Result: 1") != std::string::npos);
    CHECK(saori(m, {"set_morph", "笑い", "1"}).find("Result: 1") != std::string::npos);
    r = saori(m, {"status"});
    CHECK(r.find("Value0: running") != std::string::npos && r.find("greet.vmd") != std::string::npos);
    // Shift_JIS caller
    r = m.call("EXECUTE SAORI/1.0\r\nCharset: Shift_JIS\r\nArgument0: set_morph\r\nArgument1: \x8F\xCE\x82\xA2\r\n\r\n");
    CHECK(r.find("Result: 1") != std::string::npos && r.find("Charset: Shift_JIS") != std::string::npos);
    CHECK(saori(m, {"load_model", "..\\..\\..\\x.pmx"}).find("forbidden_path") != std::string::npos);
    CHECK(saori(m, {"get_behavior_status"}).find("Value0: Idle") != std::string::npos);
    CHECK(saori(m, {"move_to", "", "", "120", "walk", "idle", "0"}).find("Result: 1") != std::string::npos);
    CHECK(saori(m, {"get_behavior_status"}).find("Value0: Walking") != std::string::npos);
    CHECK(saori(m, {"stop_move"}).find("Result: 1") != std::string::npos);
    CHECK(saori(m, {"move_to", "", "", "", "..\\..\\..\\x.vmd"}).find("forbidden_path") != std::string::npos);
    // unload() stops the runtime it started.
    CHECK(m.unload());
    CHECK(waitNoChildren(4000));
    FreeLibrary(m.handle);
}

void testYaya(const fs::path &yaya, const fs::path &dll, const fs::path &runtime, const fs::path &assets,
              const fs::path &ghostSrc, const fs::path &work) {
    const fs::path master = assembleGhost(work, ghostSrc, yaya, dll, runtime, assets);
    Module y;
    CHECK(y.open(master / L"yaya.dll"));
    if (!y.handle)
        return;
    CHECK(y.load(utf8(master.native()) + "\\"));

    std::string r = shiori(y, "GET", "OnBoot", {"master"});
    std::printf("    OnBoot -> %s\n", r.substr(0, 160).c_str());
    CHECK(r.starts_with("SHIORI/3.0 200 OK"));
    CHECK(r.find("こんにちは") != std::string::npos);
    CHECK(r.find("\\q[笑顔にする,OnUk3dSmile]") != std::string::npos);
    CHECK(runtimeChildren().size() == 1);
    CHECK(shiori(y, "GET", "OnUk3dGlassesOn").find("眼鏡をかけました") != std::string::npos);
    CHECK(shiori(y, "GET", "OnUk3dGlassesOff").find("眼鏡を外しました") != std::string::npos);

    r = shiori(y, "GET", "OnUk3dSmile");
    CHECK(r.find("にこっ") != std::string::npos);
    r = shiori(y, "GET", "OnUk3dNod");
    CHECK(r.find("うんうん") != std::string::npos);
    r = shiori(y, "GET", "OnUk3dStatus");
    std::printf("    OnUk3dStatus -> %s\n", r.substr(r.find("Value: "), 80).c_str());
    CHECK(r.find("running") != std::string::npos);
    r = shiori(y, "GET", "OnUk3dResetFace");
    CHECK(r.find("いつもの顔") != std::string::npos);
    CHECK(shiori(y, "GET", "OnUk3dDesktop").find("手前の窓に座る") != std::string::npos);
    CHECK(shiori(y, "GET", "OnUk3dWalk").find("歩きます。") != std::string::npos);
    CHECK(shiori(y, "GET", "OnUk3dStopMove").find("止まりました。") != std::string::npos);
    CHECK(shiori(y, "GET", "OnUk3dAutoOn").find("自律行動を ON") != std::string::npos);
    CHECK(shiori(y, "GET", "OnUk3dAutoOff").find("自律行動を OFF") != std::string::npos);
    CHECK(shiori(y, "GET", "OnUk3dCrossOn").find("画面またぎを ON") != std::string::npos);
    CHECK(shiori(y, "GET", "OnUk3dCrossOff").find("画面またぎを OFF") != std::string::npos);
    CHECK(shiori(y, "GET", "OnUk3dTopmostOn").find("常に最前面を ON") != std::string::npos);
    CHECK(shiori(y, "GET", "OnUk3dTopmostOff").find("常に最前面を OFF") != std::string::npos);
    CHECK(shiori(y, "GET", "OnMouseClick", {"190", "64", "0", "0", "Head", "0", "mouse"})
              .find("頭をこつん") != std::string::npos);
    CHECK(shiori(y, "GET", "OnMouseClick", {"190", "110", "0", "0", "Face", "0", "mouse"})
              .find("ほっぺ") != std::string::npos);
    CHECK(shiori(y, "GET", "OnUk3dPet", {"190", "64", "0", "0", "Head", "0", "mouse"})
              .find("なでてくれると") != std::string::npos);
    CHECK(shiori(y, "GET", "OnMouseDoubleClick", {"190", "366", "0", "0", "Body", "0", "mouse"})
              .find("なにをしましょう") != std::string::npos);
    CHECK(shiori(y, "GET", "OnMouseClick", {"190", "64", "0", "0", "Head", "1", "mouse"})
              .starts_with("SHIORI/3.0 204"));
    CHECK(shiori(y, "GET", "OnSomethingUnknown").starts_with("SHIORI/3.0 204"));
    CHECK(shiori(y, "NOTIFY", "OnNotifyOSInfo", {"Windows"}).starts_with("SHIORI/3.0 204"));

    // OnClose returns the farewell ending in \- and keeps the 3D character
    // alive for it; SSP then unloads the SHIORI, which stops the runtime.
    r = shiori(y, "GET", "OnClose", {"user"});
    CHECK(r.find("\\-") != std::string::npos);
    CHECK(runtimeChildren().size() == 1);
    CHECK(y.unload());
    CHECK(waitNoChildren(4000));
    FreeLibrary(y.handle);
}

} // namespace

int wmain(int argc, wchar_t **argv) {
    SetConsoleOutputCP(CP_UTF8);
    if (argc < 2) {
        std::printf("usage: see source\n");
        return 2;
    }
    const std::wstring mode = argv[1];
    const fs::path work = fs::temp_directory_path() / (L"uka3d_x86_tests_" + std::to_wstring(GetCurrentProcessId()));
    fs::create_directories(work);
    try {
        if (mode == L"saori" && argc >= 5) {
            std::printf("[saori_x86]\n");
            testSaori(fs::absolute(argv[2]), fs::absolute(argv[3]), fs::absolute(argv[4]), work);
        } else if (mode == L"shiori" && argc >= 4) {
            // Development aid: uka3d_x86_tests shiori <ghost/master> <ID> [<ID>...]
            Module y;
            const fs::path master = fs::absolute(argv[2]);
            if (!y.open(master / L"yaya.dll") || !y.load(utf8(master.native()) + "\\")) {
                std::printf("cannot load yaya.dll\n");
                return 2;
            }
            for (int i = 3; i < argc; ++i)
                std::printf("== %s\n%s\n", utf8(argv[i]).c_str(), shiori(y, "GET", utf8(argv[i])).c_str());
            y.unload();
            return 0;
        } else if (mode == L"yaya" && argc >= 7) {
            std::printf("[yaya_ghost]\n");
            testYaya(fs::absolute(argv[2]), fs::absolute(argv[3]), fs::absolute(argv[4]), fs::absolute(argv[5]),
                     fs::absolute(argv[6]), work);
        } else {
            std::printf("bad arguments\n");
            return 2;
        }
    } catch (const std::exception &e) {
        ++g_failures;
        std::printf("  FAILED: exception: %s\n", e.what());
    }
    std::error_code ec;
    fs::remove_all(work, ec);
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
