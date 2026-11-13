// IPC / SAORI bridge tests.
//   uka3d_ipc_tests <runtime.exe> <ukagaka3d_saori.dll> <test-assets-dir>
//
// Starts real runtime processes (small transparent windows) and talks to them
// over the named pipe, both directly and through the SAORI bridge.
#include "ipc/PathPolicy.h"
#include "ipc/PipeClient.h"
#include "platform/Utf.h"
#include "saori/SaoriBridge.h"
#include "saori/SaoriProtocol.h"

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;
using namespace uka3d;
using ipc::Json;

namespace {

int g_failures = 0;
int g_checks = 0;
fs::path g_runtime;
fs::path g_saoriDll;
fs::path g_assets;
fs::path g_work;

void report(bool ok, const char *expr, int line) {
    ++g_checks;
    if (!ok) {
        ++g_failures;
        std::printf("  FAILED: %s (line %d)\n", expr, line);
    }
}
#define CHECK(expr) report(static_cast<bool>(expr), #expr, __LINE__)

ULONGLONG nowMs() {
    return GetTickCount64();
}

std::string uniqueId(const char *prefix) {
    return std::string(prefix) + "-" + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(nowMs() % 100000);
}

// Starts the runtime directly with the given extra arguments.
PROCESS_INFORMATION startRuntime(const std::vector<std::wstring> &args) {
    std::wstring commandLine = L"\"" + g_runtime.native() + L"\"";
    for (const auto &arg : args)
        commandLine += L" \"" + arg + L"\"";
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION info{};
    CreateProcessW(g_runtime.c_str(), commandLine.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
                   g_runtime.parent_path().c_str(), &startup, &info);
    return info;
}

void stopProcess(PROCESS_INFORMATION &info) {
    if (info.hProcess == nullptr)
        return;
    if (WaitForSingleObject(info.hProcess, 3000) != WAIT_OBJECT_0)
        TerminateProcess(info.hProcess, 1);
    CloseHandle(info.hProcess);
    CloseHandle(info.hThread);
    info = {};
}

DWORD exitCodeAfter(PROCESS_INFORMATION &info, DWORD waitMs) {
    if (WaitForSingleObject(info.hProcess, waitMs) != WAIT_OBJECT_0)
        return STILL_ACTIVE;
    DWORD code = 0;
    GetExitCodeProcess(info.hProcess, &code);
    return code;
}

std::wstring modelPath() {
    return (g_assets / L"テスト用モデル" / L"物理テストキャラ.pmx").native();
}

std::vector<std::wstring> runtimeArgs(const std::string &id, DWORD parentPid) {
    return {L"--ipc",    utf8ToWide(id), L"--parent-pid", std::to_wstring(parentPid), L"--allowed-root",
            g_assets.native(), L"--model", modelPath(), L"--width", L"160", L"--height", L"240", L"--fps", L"30",
            L"--log", (g_work / (utf8ToWide(id) + L".log")).native()};
}

// Builds a self-contained "ghost" folder for the SAORI bridge.
fs::path makeGhost(const wchar_t *name) {
    const fs::path ghost = g_work / name;
    fs::create_directories(ghost / L"3d" / L"model");
    fs::create_directories(ghost / L"3d" / L"motion");
    fs::copy(g_assets / L"テスト用モデル", ghost / L"3d" / L"model",
             fs::copy_options::recursive | fs::copy_options::overwrite_existing);
    for (const auto *motion : {L"挨拶.vmd", L"うなずき.vmd", L"待機テスト.vmd"})
        fs::copy_file(g_assets / L"モーション" / motion, ghost / L"3d" / L"motion" / motion,
                      fs::copy_options::overwrite_existing);
    const Json config{{"runtime", pathToUtf8(g_runtime)},
                      {"model", "3d/model/物理テストキャラ.pmx"},
                      {"motionDir", "3d/motion"},
                      {"window", {{"width", 160}, {"height", 240}, {"transparent", true}}},
                      {"fps", 30},
                      {"timeouts", {{"launchMs", 8000}, {"requestMs", 3000}, {"loadMs", 8000}}}};
    std::ofstream(ghost / L"ukagaka3d.json", std::ios::binary) << config.dump(2);
    return ghost;
}

Json statusOf(saori::SaoriBridge &bridge) {
    const auto response = bridge.execute({"status"});
    if (response.values.size() < 2 || response.values[0] != "running")
        return nullptr;
    return Json::parse(response.values[1], nullptr, false);
}

// ---------------------------------------------------------------------------
void testSaoriProtocol() {
    std::string error;
    const std::string request = "EXECUTE SAORI/1.0\r\nCharset: UTF-8\r\nSender: テスト\r\nSecurityLevel: Local\r\n"
                                "Argument0: set_morph\r\nArgument1: 笑い\r\nArgument2: 0.5\r\n\r\n";
    auto parsed = saori::parseRequest(request, error);
    CHECK(parsed.has_value());
    CHECK(parsed && parsed->kind == saori::SaoriRequest::Kind::execute);
    CHECK(parsed && parsed->arguments.size() == 3 && parsed->arguments[1] == "笑い" && parsed->arguments[2] == "0.5");
    CHECK(parsed && parsed->sender == "テスト" && parsed->securityLevel == "Local");

    // Shift_JIS request: arguments are decoded to UTF-8.
    const std::string sjis = "EXECUTE SAORI/1.0\r\nCharset: Shift_JIS\r\nArgument0: set_morph\r\nArgument1: "
                             "\x8F\xCE\x82\xA2\r\n\r\n"; // 笑い in CP932
    parsed = saori::parseRequest(sjis, error);
    CHECK(parsed && parsed->charset == saori::Charset::shiftJis && parsed->arguments[1] == "笑い");

    parsed = saori::parseRequest("GET Version SAORI/1.0\r\nCharset: UTF-8\r\n\r\n", error);
    CHECK(parsed && parsed->kind == saori::SaoriRequest::Kind::getVersion);
    CHECK(!saori::parseRequest("GET SHIORI/3.0\r\n\r\n", error));
    CHECK(!saori::parseRequest("", error));
    CHECK(!saori::parseRequest("EXECUTE SAORI/1.0\r\nCharset: EBCDIC\r\n\r\n", error));
    // Sparse / out-of-order arguments.
    parsed = saori::parseRequest("EXECUTE SAORI/1.0\r\nArgument2: c\r\nArgument0: a\r\n\r\n", error);
    CHECK(parsed && parsed->arguments.size() == 3 && parsed->arguments[0] == "a" && parsed->arguments[1].empty());

    saori::SaoriResponse response;
    response.result = "1";
    response.values = {"line1\r\nline2", "笑い"};
    const std::string text = saori::formatResponse(response, saori::Charset::utf8, "UTF-8");
    CHECK(text.starts_with("SAORI/1.0 200 OK\r\n"));
    CHECK(text.find("Value0: line1  line2\r\n") != std::string::npos); // CR/LF neutralised
    CHECK(text.find("Value1: 笑い\r\n") != std::string::npos);
    CHECK(text.ends_with("\r\n\r\n"));
    const std::string sjisText = saori::formatResponse(response, saori::Charset::shiftJis, "Shift_JIS");
    CHECK(sjisText.find("\x8F\xCE\x82\xA2") != std::string::npos);
}

void testPathPolicy() {
    const fs::path root = g_work / L"root";
    fs::create_directories(root / L"sub");
    std::ofstream(root / L"sub" / L"a.vmd") << "x";
    const std::vector<fs::path> roots{root};
    CHECK(ipc::resolveWithinRoots(L"sub\\a.vmd", root, roots).has_value());
    CHECK(ipc::resolveWithinRoots(root / L"sub" / L"a.vmd", root, roots).has_value());
    CHECK(!ipc::resolveWithinRoots(L"..\\outside.vmd", root, roots));
    CHECK(!ipc::resolveWithinRoots(L"sub\\..\\..\\outside.vmd", root, roots));
    CHECK(!ipc::resolveWithinRoots(L"C:\\Windows\\win.ini", root, roots));
    CHECK(!ipc::resolveWithinRoots(L"\\\\server\\share\\a.vmd", root, roots));
    CHECK(!ipc::resolveWithinRoots(L"sub\\a.vmd:stream", root, roots));
    // "root2" must not count as inside "root".
    fs::create_directories(g_work / L"root2");
    CHECK(!ipc::resolveWithinRoots(g_work / L"root2" / L"a.vmd", root, roots));
}

// 1) DLL load/unload and raw request() round trip.
void testDllExports() {
    HMODULE dll = LoadLibraryW(g_saoriDll.c_str());
    CHECK(dll != nullptr);
    if (!dll)
        return;
    using LoadFn = BOOL(__cdecl *)(HGLOBAL, long);
    using UnloadFn = BOOL(__cdecl *)();
    using RequestFn = HGLOBAL(__cdecl *)(HGLOBAL, long *);
    auto loadu = reinterpret_cast<LoadFn>(GetProcAddress(dll, "loadu"));
    auto load = reinterpret_cast<LoadFn>(GetProcAddress(dll, "load"));
    auto unload = reinterpret_cast<UnloadFn>(GetProcAddress(dll, "unload"));
    auto request = reinterpret_cast<RequestFn>(GetProcAddress(dll, "request"));
    CHECK(loadu && load && unload && request);
    if (!(loadu && unload && request)) {
        FreeLibrary(dll);
        return;
    }
    const auto call = [&](const std::string &text) {
        HGLOBAL in = GlobalAlloc(GMEM_FIXED, text.size());
        std::memcpy(in, text.data(), text.size());
        long len = static_cast<long>(text.size());
        HGLOBAL out = request(in, &len);
        std::string result(static_cast<const char *>(out), static_cast<std::size_t>(len));
        GlobalFree(out);
        return result;
    };
    const std::string dir = "C:\\";
    HGLOBAL path = GlobalAlloc(GMEM_FIXED, dir.size());
    std::memcpy(path, dir.data(), dir.size());
    CHECK(loadu(path, static_cast<long>(dir.size())));
    CHECK(call("GET Version SAORI/1.0\r\nCharset: UTF-8\r\n\r\n").starts_with("SAORI/1.0 200 OK"));
    const std::string version = call("EXECUTE SAORI/1.0\r\nCharset: UTF-8\r\nArgument0: version\r\n\r\n");
    CHECK(version.find("Result: 1\r\n") != std::string::npos && version.find("ukagaka3d_saori/") != std::string::npos);
    CHECK(call("EXECUTE SAORI/1.0\r\nCharset: UTF-8\r\nSecurityLevel: External\r\nArgument0: launch\r\n\r\n")
              .starts_with("SAORI/1.0 400"));
    CHECK(call("garbage").starts_with("SAORI/1.0 400"));
    const std::string unknown = call("EXECUTE SAORI/1.0\r\nCharset: UTF-8\r\nArgument0: dance\r\n\r\n");
    CHECK(unknown.find("Result: 0\r\n") != std::string::npos && unknown.find("unknown_command") != std::string::npos);
    const std::string notRunning = call("EXECUTE SAORI/1.0\r\nCharset: UTF-8\r\nArgument0: set_morph\r\nArgument1: x\r\n\r\n");
    CHECK(notRunning.find("Result: 0\r\n") != std::string::npos && notRunning.find("not_running") != std::string::npos);
    CHECK(unload());
    CHECK(FreeLibrary(dll));
}

// 4, 5, 6) Direct IPC: success, connect timeout, invalid JSON, unknown command.
void testDirectIpc() {
    // 5) Connection timeout to a pipe nobody serves.
    {
        ipc::PipeClient client;
        ipc::Error err;
        const ULONGLONG start = nowMs();
        CHECK(!client.connect(uniqueId("nobody"), 300, 0, err));
        const ULONGLONG elapsed = nowMs() - start;
        std::printf("    connect timeout: %llu ms (%s)\n", elapsed, err.code.c_str());
        CHECK(err.code == ipc::error::timeout && elapsed >= 250 && elapsed < 1500);
        CHECK(!client.connect("bad id!", 100, 0, err));
    }

    const std::string id = uniqueId("direct");
    auto process = startRuntime(runtimeArgs(id, GetCurrentProcessId()));
    CHECK(process.hProcess != nullptr);
    ipc::PipeClient client;
    ipc::Error err;
    CHECK(client.connect(id, 8000, process.dwProcessId, err));

    // 4) Success path.
    auto response = client.call("ping", Json::object(), 3000, err);
    CHECK(response && response->value("success", false) && (*response)["result"]["pong"] == true);
    response = client.call("status", Json::object(), 3000, err);
    CHECK(response && (*response)["result"]["model"]["name"] == "物理テストキャラ");
    CHECK(response && (*response)["result"]["physics"]["enabled"] == true);

    // 6) Invalid JSON / envelope errors keep the connection usable.
    response = client.callRaw("{this is not json", 3000, err);
    CHECK(response && (*response)["success"] == false && (*response)["error"]["code"] == ipc::error::invalidJson);
    response = client.callRaw(R"({"version":99,"requestId":"x","command":"ping"})", 3000, err);
    CHECK(response && (*response)["error"]["code"] == ipc::error::unsupportedVersion);
    response = client.callRaw(R"({"version":1,"command":"ping"})", 3000, err);
    CHECK(response && (*response)["error"]["code"] == ipc::error::invalidRequest);
    response = client.callRaw(R"({"version":1,"requestId":"7","command":"ping","params":[1]})", 3000, err);
    CHECK(response && (*response)["error"]["code"] == ipc::error::invalidRequest && (*response)["requestId"] == "7");
    response = client.call("dance", Json::object(), 3000, err);
    CHECK(response && (*response)["error"]["code"] == ipc::error::unknownCommand);
    response = client.call("ping", Json::object(), 3000, err);
    CHECK(response && response->value("success", false));

    // Path policy on the runtime side.
    response = client.call("play_motion", {{"path", "C:\\Windows\\win.ini"}}, 3000, err);
    CHECK(response && (*response)["error"]["code"] == ipc::error::forbiddenPath);
    response = client.call("play_motion", {{"path", pathToUtf8(g_assets / L"モーション" / L"無い.vmd")}}, 3000, err);
    CHECK(response && (*response)["error"]["code"] == ipc::error::notFound);
    response = client.call("play_motion", {{"path", pathToUtf8(g_assets / L"broken" / L"壊れたモーション.vmd")}}, 3000, err);
    CHECK(response && (*response)["error"]["code"] == ipc::error::loadFailed);
    response = client.call("set_morph", {{"name", "存在しない"}}, 3000, err);
    CHECK(response && (*response)["error"]["code"] == ipc::error::notFound);
    response = client.call("set_morph", {{"weight", 1}}, 3000, err);
    CHECK(response && (*response)["error"]["code"] == ipc::error::invalidParams);

    // Graceful shutdown: response arrives, then the process exits with 0.
    response = client.call("shutdown", Json::object(), 3000, err);
    CHECK(response && response->value("success", false));
    CHECK(exitCodeAfter(process, 5000) == 0);
    stopProcess(process);
}

// 11) Instance ids: the same id cannot be served twice; different ids coexist.
//     Access control: only the parent process may connect; parent death ends the runtime.
void testInstancesAndAccess() {
    const std::string idA = uniqueId("multiA");
    const std::string idB = uniqueId("multiB");
    auto a = startRuntime(runtimeArgs(idA, GetCurrentProcessId()));
    auto b = startRuntime(runtimeArgs(idB, GetCurrentProcessId()));
    ipc::PipeClient clientA, clientB;
    ipc::Error err;
    CHECK(clientA.connect(idA, 8000, a.dwProcessId, err));
    CHECK(clientB.connect(idB, 8000, b.dwProcessId, err));
    auto statusA = clientA.call("status", Json::object(), 3000, err);
    auto statusB = clientB.call("status", Json::object(), 3000, err);
    CHECK(statusA && statusB && (*statusA)["result"]["pid"] == a.dwProcessId && (*statusB)["result"]["pid"] == b.dwProcessId);
    CHECK(statusA && (*statusA)["result"]["instanceId"] == idA);

    // Same id again -> the second runtime cannot create the pipe and exits with 5.
    auto duplicate = startRuntime(runtimeArgs(idA, GetCurrentProcessId()));
    const DWORD duplicateExit = exitCodeAfter(duplicate, 10000);
    std::printf("    duplicate instance exit code: %lu\n", duplicateExit);
    CHECK(duplicateExit == 5);
    stopProcess(duplicate);
    // The original instance is unaffected.
    CHECK(clientA.call("ping", Json::object(), 3000, err).has_value());

    clientA.call("shutdown", Json::object(), 3000, err);
    clientB.call("shutdown", Json::object(), 3000, err);
    CHECK(exitCodeAfter(a, 5000) == 0 && exitCodeAfter(b, 5000) == 0);
    stopProcess(a);
    stopProcess(b);

    // A runtime owned by another parent refuses us, and exits when that parent dies.
    wchar_t fakeParentCmd[] = L"cmd.exe /c ping -n 60 127.0.0.1 >nul";
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION parent{};
    CHECK(CreateProcessW(nullptr, fakeParentCmd, nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si,
                         &parent));
    const std::string idC = uniqueId("foreign");
    auto c = startRuntime(runtimeArgs(idC, parent.dwProcessId));
    ipc::PipeClient intruder;
    bool refused = false;
    if (intruder.connect(idC, 8000, c.dwProcessId, err)) {
        auto response = intruder.call("ping", Json::object(), 2000, err);
        refused = !response || (*response)["success"] == false;
    }
    CHECK(refused);
    TerminateProcess(parent.hProcess, 0);
    CloseHandle(parent.hProcess);
    CloseHandle(parent.hThread);
    const ULONGLONG start = nowMs();
    const DWORD orphanExit = exitCodeAfter(c, 5000);
    std::printf("    runtime exit after parent death: code %lu in %llu ms\n", orphanExit, nowMs() - start);
    CHECK(orphanExit == 0);
    stopProcess(c);
}

// 3, 7, 8, 9, 10) Through the SAORI bridge: launch, model, motions, morphs, crash, shutdown.
void testBridge() {
    const fs::path ghost = makeGhost(L"ghostA");
    saori::SaoriBridge bridge(ghost);
    bridge.setInstanceId(uniqueId("bridge"));

    CHECK(bridge.execute({"status"}).values[0] == "stopped");
    auto r = bridge.execute({"play_motion", "挨拶"});
    CHECK(r.result == "0" && r.values[1] == ipc::error::notRunning);

    // 3) launch
    const ULONGLONG start = nowMs();
    r = bridge.execute({"launch"});
    std::printf("    launch: %s %s (%llu ms)\n", r.result.c_str(), r.values[0].c_str(), nowMs() - start);
    CHECK(r.result == "1" && r.values[0] == "started");
    CHECK(bridge.execute({"launch"}).values[0] == "already_running");
    Json status = statusOf(bridge);
    CHECK(status.is_object() && status["model"]["name"] == "物理テストキャラ");

    // 8) motion by name (3d/motion/<name>.vmd), non-looping with a follow-up.
    r = bridge.execute({"play_motion", "挨拶", "0", "うなずき"});
    CHECK(r.result == "1");
    status = statusOf(bridge);
    CHECK(status.is_object() && status["motion"]["loop"] == false &&
          status["motion"]["path"].get<std::string>().find("挨拶.vmd") != std::string::npos);
    std::this_thread::sleep_for(std::chrono::milliseconds(3600)); // greet is 3 s
    status = statusOf(bridge);
    CHECK(status.is_object() && status["motion"].is_object() &&
          status["motion"]["path"].get<std::string>().find("うなずき.vmd") != std::string::npos &&
          status["motion"]["loop"] == true);
    // Path restrictions.
    CHECK(bridge.execute({"play_motion", "..\\..\\x.vmd"}).values[1] == ipc::error::forbiddenPath);
    CHECK(bridge.execute({"play_motion", "C:\\Windows\\win.ini"}).values[1] == ipc::error::forbiddenPath);
    CHECK(bridge.execute({"play_motion", "存在しない"}).result == "0");
    CHECK(bridge.execute({"play_motion", "うなずき", "maybe"}).values[1] == ipc::error::invalidParams);
    CHECK(bridge.execute({"stop_motion"}).result == "1");
    status = statusOf(bridge);
    CHECK(status.is_object() && status["motion"].is_null());

    // 9) morphs
    CHECK(bridge.execute({"set_morph", "笑い", "1.0"}).result == "1");
    status = statusOf(bridge);
    CHECK(status.is_object() && status["morphOverrides"]["笑い"] == 1.0);
    CHECK(bridge.execute({"set_morph", "笑い", "abc"}).values[1] == ipc::error::invalidParams);
    CHECK(bridge.execute({"set_morph", "怒り"}).values[1] == ipc::error::notFound);
    CHECK(bridge.execute({"set_morphs", "1", "笑い", "0.5", "存在しない", "1"}).result == "1");
    status = statusOf(bridge);
    CHECK(status.is_object() && status["morphOverrides"]["笑い"] == 0.5 && status["morphOverrides"].size() == 1);
    CHECK(bridge.execute({"set_morphs", "1", "笑い"}).values[1] == ipc::error::invalidParams);
    CHECK(bridge.execute({"set_morphs", "x", "笑い", "1"}).values[1] == ipc::error::invalidParams);
    CHECK(bridge.execute({"reset_morph"}).result == "1");
    status = statusOf(bridge);
    CHECK(status.is_object() && status["morphOverrides"].empty());

    // 7) load another model (inside the ghost folder) and one outside it.
    CHECK(bridge.execute({"load_model", "3d/model/テストキャラ.pmx"}).result == "1");
    status = statusOf(bridge);
    CHECK(status.is_object() && status["model"]["name"] == "テストキャラ" && status["physics"]["enabled"] == false);
    CHECK(bridge.execute({"load_model", pathToUtf8(modelPath())}).values[1] == ipc::error::forbiddenPath);
    fs::copy_file(g_assets / L"broken" / L"偽物.pmx", ghost / L"3d" / L"model" / L"偽物.pmx",
                  fs::copy_options::overwrite_existing);
    r = bridge.execute({"load_model", "3d/model/偽物.pmx"});
    CHECK(r.result == "0" && r.values[1] == ipc::error::loadFailed);
    status = statusOf(bridge);
    CHECK(status.is_object() && status["model"]["name"] == "テストキャラ"); // previous model kept

    // 10) abnormal termination of the runtime is detected without hanging.
    const DWORD pid = status["pid"].get<DWORD>();
    HANDLE runtime = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, pid);
    TerminateProcess(runtime, 9);
    WaitForSingleObject(runtime, 3000);
    CloseHandle(runtime);
    const ULONGLONG t0 = nowMs();
    r = bridge.execute({"set_morph", "笑い"});
    CHECK(r.result == "0" && r.values[1] == ipc::error::notRunning);
    CHECK(bridge.execute({"status"}).values[0] == "stopped");
    CHECK(nowMs() - t0 < 1500);

    // Relaunch works after a crash, and shutdown stops the process.
    CHECK(bridge.execute({"launch"}).values[0] == "started");
    const Json relaunched = statusOf(bridge);
    CHECK(relaunched.is_object());
    const DWORD pid2 = relaunched.is_object() ? relaunched["pid"].get<DWORD>() : 0;
    HANDLE second = OpenProcess(SYNCHRONIZE, FALSE, pid2);
    CHECK(bridge.execute({"shutdown"}).values[0] == "stopped");
    CHECK(second != nullptr && WaitForSingleObject(second, 2000) == WAIT_OBJECT_0);
    if (second)
        CloseHandle(second);
    CHECK(bridge.execute({"status"}).values[0] == "stopped");

    // Two ghosts (different folders) get different instance ids and coexist.
    const fs::path ghostB = makeGhost(L"ghostB");
    saori::SaoriBridge first(ghost);
    saori::SaoriBridge secondGhost(ghostB);
    CHECK(first.instanceId() != secondGhost.instanceId());
    CHECK(first.execute({"launch"}).values[0] == "started");
    CHECK(secondGhost.execute({"launch"}).values[0] == "started");
    const Json s1 = statusOf(first);
    const Json s2 = statusOf(secondGhost);
    CHECK(s1.is_object() && s2.is_object() && s1["pid"] != s2["pid"]);
    first.shutdownRuntime();
    secondGhost.shutdownRuntime();
    CHECK(first.execute({"status"}).values[0] == "stopped");
}

void testDesktopBehavior() {
    const std::string id = uniqueId("desktop");
    auto args = runtimeArgs(id, GetCurrentProcessId());
    // This process provides ordinary target windows, so connect as a direct
    // client instead of identifying it as the excluded SSP parent.
    args.erase(args.begin() + 2, args.begin() + 4);
    args.insert(args.end(), {L"--exit-after", L"20"});
    auto process = startRuntime(args);
    ipc::PipeClient client;
    ipc::Error err;
    if (!client.connect(id, 8000, process.dwProcessId, err)) {
        CHECK(false);
        stopProcess(process);
        return;
    }
    const auto call = [&](const char *command, const Json &params = Json::object()) {
        return client.call(command, params, 3000, err).value_or(Json::object());
    };
    const auto result = [&](const char *command, const Json &params = Json::object()) {
        const auto response = call(command, params);
        CHECK(response.value("success", false));
        return response.value("result", Json::object());
    };
    const auto waitFor = [&](const std::function<bool(const Json &)> &predicate, DWORD timeout = 3000) {
        const ULONGLONG until = nowMs() + timeout;
        do {
            if (predicate(result("get_behavior_status")))
                return true;
            Sleep(40);
        } while (nowMs() < until);
        return false;
    };
    const auto isIdle = [](const Json &status) { return status.value("state", "") == "Idle"; };
    const auto initial = result("get_behavior_status");
    CHECK(isIdle(initial) && !initial.value("autoEnabled", true) && !initial.value("crossMonitors", true));
    CHECK(!call("move_to", {{"x", 1}}).value("success", true));
    CHECK(!call("move_to", {{"nextMonitor", true}}).value("success", true));
    const Json moving = result("move_to", {{"x", -100000}, {"y", 100000}, {"speed", 2500},
                                           {"walkMotion", "モーション/未作成.vmd"}, {"idleMotion", "モーション/待機テスト.vmd"}});
    CHECK(moving.value("state", "") == "Walking");
    CHECK(waitFor(isIdle));
    const Json stopped = result("get_behavior_status");
    const auto work = initial.value("monitorWork", std::vector<long>{});
    CHECK(work.size() == 4 && stopped.value("x", 100000) == work[0]);
    const auto afterWalk = result("status");
    CHECK(afterWalk.value("motion", Json{}).is_object() &&
          afterWalk["motion"].value("path", "").find("待機テスト.vmd") != std::string::npos);

    const DPI_AWARENESS_CONTEXT oldDpi = SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    const HINSTANCE instance = GetModuleHandleW(nullptr);
    WNDCLASSW windowClass{};
    windowClass.lpfnWndProc = DefWindowProcW;
    windowClass.hInstance = instance;
    windowClass.lpszClassName = L"Uka3dBehaviorTestTarget";
    CHECK(RegisterClassW(&windowClass) != 0);
    const auto create = [&](DWORD exStyle, DWORD style, int width, int height) {
        return CreateWindowExW(exStyle, windowClass.lpszClassName, L"uka3d behavior test", style,
                               240, 360, width, height, nullptr, nullptr, instance, nullptr);
    };
    HWND target = create(0, WS_OVERLAPPEDWINDOW, 450, 250);
    const HWND hidden = create(0, WS_OVERLAPPEDWINDOW, 450, 250);
    const HWND tool = create(WS_EX_TOOLWINDOW, WS_OVERLAPPEDWINDOW, 450, 250);
    const HWND tiny = create(0, WS_POPUP, 40, 30);
    CHECK(target && hidden && tool && tiny);
    for (HWND hwnd : {target, tool, tiny})
        ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    const auto targetId = [](HWND hwnd) { return std::to_string(reinterpret_cast<std::uintptr_t>(hwnd)); };
    const auto listed = result("list_windows").value("windows", Json::array());
    const auto containsWindow = [&](HWND hwnd) {
        return std::ranges::any_of(listed, [&](const Json &entry) { return entry.value("target", "") == targetId(hwnd); });
    };
    CHECK(containsWindow(target) && !containsWindow(hidden) && !containsWindow(tool) && !containsWindow(tiny));
    CHECK(!call("sit_on_window", {{"target", targetId(hidden)}}).value("success", true));
    CHECK(!call("sit_on_window", {{"target", targetId(target)}, {"position", "invalid"}}).value("success", true));
    const auto sit = [&] {
        const auto response = result("sit_on_window", {{"target", targetId(target)}, {"position", "top-center"},
                                                        {"motion", "モーション/着席.vmd"}, {"anchorBone", "センター"}});
        CHECK(response.value("state", "") == "Transition");
        return waitFor([](const Json &status) { return status.value("state", "") == "Sitting"; });
    };
    CHECK(sit());
    const auto seated = result("get_behavior_status").value("seat", Json::object());
    SetWindowPos(target, nullptr, 290, 400, 530, 250, SWP_NOACTIVATE | SWP_NOZORDER);
    CHECK(waitFor([&](const Json &status) {
        const auto seat = status.value("seat", Json::object());
        return seat.value("edgeX", 0) == seated.value("edgeX", 0) + 90 &&
               seat.value("edgeY", 0) == seated.value("edgeY", 0) + 40;
    }));
    // configure_seat: per-model anchor settings, applied to the current seat at once.
    {
        CHECK(result("stand_up").value("state", "") == "Transition");
        CHECK(waitFor(isIdle));
        CHECK(result("sit_on_window", {{"target", targetId(target)}, {"motion", "モーション/着席.vmd"}})
                  .value("state", "") == "Transition");
        CHECK(waitFor([](const Json &status) { return status.value("state", "") == "Sitting"; }));
        const Json before = result("get_behavior_status");
        // The test model has thigh bones: the underside of the thighs rests on the edge.
        CHECK(before.value("seat", Json::object()).value("anchor", "") == "contact");
        const Json joint = result("configure_seat", {{"useContact", false}, {"anchorBone", "頭"},
                                                      {"transitionMs", 120}});
        CHECK(joint.value("mode", "") == "bone" && joint.value("contactPoints", 1) == 0 &&
              joint.value("anchor", "") == "頭");
        // The head joint is far above the thigh surface: the character sinks (window moves down).
        CHECK(waitFor([&](const Json &status) { return status.value("y", 0) > before.value("y", 0) + 20; }));
        const Json lowered = result("get_behavior_status");
        CHECK(result("configure_seat", {{"offsetY", 2.0}}).value("offsetY", 0.0) == 2.0);
        CHECK(waitFor([&](const Json &status) { return status.value("y", 0) < lowered.value("y", 0) - 5; }));
        CHECK(lowered.value("seat", Json::object()).value("edgeY", 0) ==
              result("get_behavior_status").value("seat", Json::object()).value("edgeY", 1)); // target did not move
        const auto code = [&](const Json &params) {
            const auto response = call("configure_seat", params);
            return response.value("success", true) ? std::string() : response["error"].value("code", "");
        };
        CHECK(code({{"anchorBone", "無いボーン"}}) == ipc::error::notFound);
        CHECK(code({{"contactBones", Json::array({"無いボーン"})}}) == ipc::error::notFound);
        CHECK(code({{"contactBones", "センター"}}) == ipc::error::invalidParams);
        CHECK(code({{"yaw", 999}}) == ipc::error::invalidParams);
        CHECK(code({{"transitionMs", -1}}) == ipc::error::invalidParams);
        CHECK(code({{"reset", "yes"}}) == ipc::error::invalidParams);
        const Json reset = result("configure_seat", {{"reset", true}});
        CHECK(reset.value("mode", "") == "contact" && reset.value("offsetY", 1.0) == 0.0 &&
              reset.value("transitionMs", 0) == 350);
        CHECK(result("stand_up").value("state", "") == "Transition"); // hops back to where it came from
        CHECK(waitFor(isIdle));
        CHECK(sit());
    }
    ShowWindow(target, SW_SHOWMINNOACTIVE);
    CHECK(waitFor(isIdle));
    CHECK(!call("sit_on_window", {{"target", targetId(target)}}).value("success", true));
    ShowWindow(target, SW_SHOWNOACTIVATE);
    CHECK(sit());
    CHECK(result("move_to", {{"speed", 1000}}).value("state", "") == "Walking");
    CHECK(result("stop_move").value("state", "") == "Transition");
    CHECK(waitFor(isIdle));
    CHECK(sit());
    DestroyWindow(target);
    CHECK(waitFor(isIdle));
    for (HWND hwnd : {hidden, tool, tiny})
        DestroyWindow(hwnd);
    if (oldDpi)
        SetThreadDpiAwarenessContext(oldDpi);

    CHECK(result("set_cross_monitor", {{"enabled", true}}).value("crossMonitors", false));
    CHECK(!result("set_cross_monitor", {{"enabled", false}}).value("crossMonitors", true));
    CHECK(result("set_auto_behavior", {{"enabled", true}, {"waitSeconds", 2}, {"speed", 120},
                                       {"sitChance", 0}}).value("autoEnabled", false));
    CHECK(waitFor([](const Json &status) { return status.value("state", "") == "Walking"; }, 4000));
    CHECK(!result("set_auto_behavior", {{"enabled", false}}).value("autoEnabled", true));
    CHECK(waitFor(isIdle));
    CHECK(result("set_topmost", {{"enabled", true}}).value("topmost", false));
    CHECK(!result("set_topmost", {{"enabled", false}}).value("topmost", true));
    result("configure_grab", {{"motion", "モーション/つまみ.vmd"}, {"idleMotion", "モーション/待機テスト.vmd"},
                              {"holdMs", 180}});
    struct RuntimeWindow {
        DWORD pid;
        HWND hwnd{};
    } runtimeWindow{process.dwProcessId};
    EnumWindows([](HWND hwnd, LPARAM data) -> BOOL {
        auto &window = *reinterpret_cast<RuntimeWindow *>(data);
        DWORD pid = 0;
        GetWindowThreadProcessId(hwnd, &pid);
        wchar_t name[64]{};
        GetClassNameW(hwnd, name, static_cast<int>(std::size(name)));
        if (pid != window.pid || std::wstring_view(name) != L"Ukagaka3dShellWindow")
            return TRUE;
        window.hwnd = hwnd;
        return FALSE;
    }, reinterpret_cast<LPARAM>(&runtimeWindow));
    CHECK(runtimeWindow.hwnd);
    if (runtimeWindow.hwnd) {
        const auto mouse = [&](UINT message) {
            DWORD_PTR reply{};
            CHECK(SendMessageTimeoutW(runtimeWindow.hwnd, message, 0, 0, SMTO_ABORTIFHUNG, 1000, &reply));
        };
        mouse(WM_LBUTTONDOWN);
        mouse(WM_LBUTTONUP);
        CHECK(!result("get_behavior_status").value("grabbed", true));
        mouse(WM_LBUTTONDOWN);
        CHECK(waitFor([](const Json &status) { return status.value("grabbed", false); }));
        const auto held = result("status").value("motion", Json::object());
        CHECK(held.is_object() && held.value("path", "").find("つまみ.vmd") != std::string::npos);
        mouse(WM_LBUTTONUP);
        CHECK(waitFor(isIdle));
        const auto released = result("status").value("motion", Json::object());
        CHECK(released.is_object() && released.value("path", "").find("待機テスト.vmd") != std::string::npos);
    }
    result("shutdown");
    CHECK(exitCodeAfter(process, 5000) == 0);
    stopProcess(process);
}

void run(const char *name, const std::function<void()> &test) {
    std::printf("[%s]\n", name);
    const int before = g_failures;
    try {
        test();
    } catch (const std::exception &e) {
        ++g_failures;
        std::printf("  FAILED: exception: %s\n", e.what());
    }
    std::printf("  %s\n", g_failures == before ? "ok" : "NG");
}

} // namespace

int wmain(int argc, wchar_t **argv) {
    SetConsoleOutputCP(CP_UTF8);
    if (argc < 4) {
        std::printf("usage: uka3d_ipc_tests <runtime.exe> <ukagaka3d_saori.dll> <test-assets-dir>\n");
        return 2;
    }
    g_runtime = fs::absolute(argv[1]);
    g_saoriDll = fs::absolute(argv[2]);
    g_assets = fs::absolute(argv[3]);
    g_work = fs::temp_directory_path() / (L"uka3d_ipc_tests_" + std::to_wstring(GetCurrentProcessId()));
    fs::create_directories(g_work);

    run("saori_protocol", testSaoriProtocol);
    run("path_policy", testPathPolicy);
    run("dll_exports", testDllExports);
    run("direct_ipc", testDirectIpc);
    run("instances_and_access", testInstancesAndAccess);
    run("saori_bridge", testBridge);
    run("desktop_behavior", testDesktopBehavior);

    std::error_code ec;
    fs::remove_all(g_work, ec);
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
