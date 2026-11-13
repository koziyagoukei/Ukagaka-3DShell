#include "saori/SaoriBridge.h"

#include "ipc/PathPolicy.h"
#include "platform/Utf.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <format>

namespace uka3d::saori {
namespace {

#ifndef UKA3D_VERSION
#define UKA3D_VERSION "0.2.0"
#endif
constexpr const char *kVersion = "ukagaka3d_saori/" UKA3D_VERSION;

std::string lower(std::string_view text) {
    std::string result(text);
    std::ranges::transform(result, result.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return result;
}

// Windows command-line quoting (CommandLineToArgvW rules).
std::wstring quote(const std::wstring &arg) {
    if (!arg.empty() && arg.find_first_of(L" \t\"") == std::wstring::npos)
        return arg;
    std::wstring result = L"\"";
    std::size_t backslashes = 0;
    for (const wchar_t c : arg) {
        if (c == L'\\') {
            ++backslashes;
        } else if (c == L'"') {
            result.append(backslashes * 2 + 1, L'\\');
            result.push_back(c);
            backslashes = 0;
        } else {
            result.append(backslashes, L'\\');
            result.push_back(c);
            backslashes = 0;
        }
    }
    result.append(backslashes * 2, L'\\');
    result.push_back(L'"');
    return result;
}

std::optional<bool> parseBool(std::string_view text) {
    const std::string value = lower(text);
    if (value == "1" || value == "true" || value == "yes" || value == "on")
        return true;
    if (value == "0" || value == "false" || value == "no" || value == "off")
        return false;
    return std::nullopt;
}

std::uint32_t fnv1a(std::wstring_view text) {
    std::uint32_t hash = 2166136261u;
    for (const wchar_t c : text) {
        hash ^= static_cast<std::uint32_t>(towlower(c));
        hash *= 16777619u;
    }
    return hash;
}

std::string errorMessage(const ipc::Json &response) {
    const auto error = response.find("error");
    if (error != response.end() && error->is_object())
        return error->value("message", std::string("不明なエラー"));
    return "不明なエラー";
}

std::string errorCode(const ipc::Json &response) {
    const auto error = response.find("error");
    if (error != response.end() && error->is_object())
        return error->value("code", std::string(ipc::error::internal));
    return ipc::error::internal;
}

} // namespace

SaoriResponse ok(std::string value, std::string detail) {
    SaoriResponse response;
    response.result = "1";
    response.values = {std::move(value)};
    if (!detail.empty())
        response.values.push_back(std::move(detail));
    return response;
}

SaoriResponse fail(std::string message, std::string code) {
    SaoriResponse response;
    response.result = "0";
    response.values = {std::move(message), std::move(code)};
    return response;
}

SaoriBridge::SaoriBridge(std::filesystem::path baseDir) : baseDir_(std::move(baseDir)) {
    log_.open(baseDir_ / L"ukagaka3d_saori.log", std::ios::binary | std::ios::trunc);
    // One runtime per ghost folder and SSP process.
    instanceId_ = std::format("ghost-{:08x}-{}", fnv1a(baseDir_.native()), GetCurrentProcessId());
    loadConfig();
    log(std::format("{} 読み込み: ghost={} instance={}", kVersion, pathToUtf8(baseDir_), instanceId_));
}

SaoriBridge::~SaoriBridge() {
    closeProcess();
}

void SaoriBridge::log(const std::string &message) {
    if (!log_.is_open())
        return;
    SYSTEMTIME now{};
    GetLocalTime(&now);
    log_ << std::format("{:02}:{:02}:{:02}.{:03} {}\n", now.wHour, now.wMinute, now.wSecond, now.wMilliseconds,
                        message);
    log_.flush();
}

void SaoriBridge::loadConfig() {
    roots_ = {baseDir_};
    const auto path = baseDir_ / L"ukagaka3d.json";
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        log("ukagaka3d.json なし: 既定値を使用");
        return;
    }
    const std::string text((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    const ipc::Json json = ipc::Json::parse(text, nullptr, false, true /*ignore comments*/);
    if (json.is_discarded() || !json.is_object()) {
        log("ukagaka3d.json を解釈できません: 既定値を使用");
        return;
    }
    const auto str = [&](const char *key) -> std::optional<std::filesystem::path> {
        const auto it = json.find(key);
        if (it != json.end() && it->is_string() && !it->get<std::string>().empty())
            return utf8ToPath(it->get<std::string>());
        return std::nullopt;
    };
    if (auto v = str("runtime"))
        config_.runtimeExe = *v;
    if (auto v = str("model"))
        config_.model = *v;
    if (auto v = str("motionDir"))
        config_.motionDir = *v;
    if (auto v = str("interaction"))
        config_.interaction = *v;
    if (const auto it = json.find("seat"); it != json.end() && it->is_object())
        config_.seat = *it;
    if (const auto it = json.find("allowedRoots"); it != json.end() && it->is_array())
        for (const auto &entry : *it)
            if (entry.is_string())
                config_.allowedRoots.push_back(utf8ToPath(entry.get<std::string>()));
    if (const auto it = json.find("window"); it != json.end() && it->is_object()) {
        config_.width = it->value("width", config_.width);
        config_.height = it->value("height", config_.height);
        config_.transparent = it->value("transparent", config_.transparent);
        config_.topmost = it->value("topmost", config_.topmost);
        config_.followWindowClass = it->value("followWindowClass", config_.followWindowClass);
    }
    if (const auto it = json.find("camera"); it != json.end() && it->is_object()) {
        config_.fit = it->value("fit", config_.fit);
        config_.zoom = it->value("zoom", config_.zoom);
    }
    if (const auto it = json.find("fps"); it != json.end() && it->is_number_integer())
        config_.fps = it->get<int>();
    config_.physics = json.value("physics", config_.physics);
    if (const auto it = json.find("face"); it != json.end() && it->is_object()) {
        config_.blink = it->value("blink", config_.blink);
        config_.blinkMorph = it->value("blinkMorph", config_.blinkMorph);
        config_.gaze = it->value("gaze", config_.gaze);
        config_.gazeRadius = std::clamp(it->value("gazeRadius", config_.gazeRadius), 0.0, 10000.0);
    }
    config_.shutdownOnUnload = json.value("shutdownOnUnload", config_.shutdownOnUnload);
    if (const auto it = json.find("timeouts"); it != json.end() && it->is_object()) {
        config_.launchTimeoutMs = std::clamp(it->value("launchMs", 5000u), 500u, 30000u);
        config_.requestTimeoutMs = std::clamp(it->value("requestMs", 3000u), 200u, 30000u);
        config_.loadTimeoutMs = std::clamp(it->value("loadMs", 10000u), 500u, 60000u);
    }
    for (const auto &root : config_.allowedRoots)
        roots_.push_back(root.is_absolute() ? root : (baseDir_ / root).lexically_normal());
    log(std::format("ukagaka3d.json 読み込み: runtime={} model={} 許可フォルダ {} 件", pathToUtf8(config_.runtimeExe),
                    pathToUtf8(config_.model), roots_.size()));
}

std::optional<std::filesystem::path> SaoriBridge::resolvePath(const std::string &text, bool motion,
                                                             std::string &error, bool allowMissing) const {
    if (text.empty()) {
        error = "ファイルが指定されていません";
        return std::nullopt;
    }
    std::filesystem::path path = utf8ToPath(text);
    // A bare name ("greet") means <motionDir>/greet.vmd.
    if (motion && !path.has_extension() && !path.has_parent_path())
        path = config_.motionDir / (path.native() + L".vmd");
    const auto resolved = ipc::resolveWithinRoots(path, baseDir_, roots_);
    if (!resolved) {
        error = "許可されたフォルダ（ゴーストのフォルダ）の外は指定できません: " + text;
        return std::nullopt;
    }
    std::error_code ec;
    if (!allowMissing && !std::filesystem::is_regular_file(*resolved, ec)) {
        error = "ファイルが見つかりません: " + pathToUtf8(*resolved);
        return std::nullopt;
    }
    return resolved;
}

bool SaoriBridge::processRunning() {
    if (process_.hProcess == nullptr)
        return false;
    if (WaitForSingleObject(process_.hProcess, 0) == WAIT_OBJECT_0) {
        DWORD code = 0;
        GetExitCodeProcess(process_.hProcess, &code);
        log(std::format("ランタイムが終了しています (終了コード {})", code));
        closeProcess();
        return false;
    }
    return true;
}

void SaoriBridge::closeProcess() {
    client_.close();
    if (process_.hProcess != nullptr)
        CloseHandle(process_.hProcess);
    if (process_.hThread != nullptr)
        CloseHandle(process_.hThread);
    process_ = {};
}

bool SaoriBridge::ensureConnected(ipc::Error &err) {
    if (!processRunning()) {
        err = {ipc::error::notRunning, "3D ランタイムは起動していません（launch を実行してください）"};
        return false;
    }
    if (client_.connected())
        return true;
    return client_.connect(instanceId_, 1000, process_.dwProcessId, err);
}

SaoriResponse SaoriBridge::launch(const std::vector<std::string> &args) {
    if (processRunning())
        return ok("already_running", std::to_string(process_.dwProcessId));

    std::string error;
    const std::string modelArg = args.size() > 1 && !args[1].empty() ? args[1] : pathToUtf8(config_.model);
    const auto model = resolvePath(modelArg, false, error);
    if (!model)
        return fail(error, ipc::error::notFound);
    const std::filesystem::path exe =
        (config_.runtimeExe.is_absolute() ? config_.runtimeExe : baseDir_ / config_.runtimeExe).lexically_normal();
    std::error_code ec;
    if (!std::filesystem::is_regular_file(exe, ec))
        return fail("3D ランタイムが見つかりません: " + pathToUtf8(exe), ipc::error::notFound);

    std::wstring commandLine = quote(exe.native());
    const auto add = [&](std::wstring_view arg) { commandLine += L" " + quote(std::wstring(arg)); };
    add(L"--ipc");
    add(utf8ToWide(instanceId_));
    add(L"--parent-pid");
    add(std::to_wstring(GetCurrentProcessId()));
    for (const auto &root : roots_) {
        add(L"--allowed-root");
        add(root.native());
    }
    add(L"--model");
    add(model->native());
    if (std::filesystem::is_regular_file(baseDir_ / L"ukagaka3d.json", ec)) {
        add(L"--accessory-config");
        add((baseDir_ / L"ukagaka3d.json").native());
    }
    if (!config_.interaction.empty()) {
        if (const auto profile = resolvePath(pathToUtf8(config_.interaction), false, error)) {
            add(L"--hit-regions");
            add(profile->native());
        } else {
            log("3D 入力設定: " + error);
        }
    }
    add(config_.transparent ? L"--transparent" : L"--windowed");
    add(L"--width");
    add(std::to_wstring(config_.width));
    add(L"--height");
    add(std::to_wstring(config_.height));
    add(L"--fps");
    add(config_.fps <= 0 ? std::wstring(L"unlimited") : std::to_wstring(config_.fps));
    add(config_.physics ? L"--physics" : L"--no-physics");
    if (config_.topmost)
        add(L"--topmost");
    if (!config_.followWindowClass.empty()) {
        add(L"--follow-window-class");
        add(utf8ToWide(config_.followWindowClass));
    }
    add(config_.blink ? L"--blink" : L"--no-blink");
    if (!config_.blinkMorph.empty()) {
        add(L"--blink-morph");
        add(utf8ToWide(config_.blinkMorph));
    }
    add(config_.gaze ? L"--gaze" : L"--no-gaze");
    add(L"--gaze-radius");
    add(std::to_wstring(static_cast<int>(config_.gazeRadius)));
    add(L"--fit");
    add(config_.fit == "height" ? L"height" : L"all");
    add(L"--zoom");
    add(std::to_wstring(config_.zoom));
    add(L"--log");
    add((baseDir_ / L"ukagaka3d_runtime.log").native());

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION info{};
    const std::wstring workingDir = exe.parent_path().native();
    if (!CreateProcessW(exe.c_str(), commandLine.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
                        workingDir.c_str(), &startup, &info)) {
        return fail(std::format("3D ランタイムを起動できません (Win32 {})", GetLastError()), ipc::error::internal);
    }
    process_ = info;
    log(std::format("ランタイム起動: pid {} model={}", info.dwProcessId, pathToUtf8(*model)));

    // Wait until the pipe accepts us, the process dies, or the timeout ends.
    const ULONGLONG deadline = GetTickCount64() + config_.launchTimeoutMs;
    ipc::Error err;
    while (GetTickCount64() < deadline) {
        if (!processRunning())
            return fail("3D ランタイムが起動直後に終了しました（ukagaka3d_runtime.log を確認してください）",
                        ipc::error::notRunning);
        if (client_.connect(instanceId_, 100, process_.dwProcessId, err)) {
            log("ランタイムに接続しました");
            if (config_.seat.is_object()) {
                // Per-model seating adjustments from ukagaka3d.json.
                const auto response = client_.call("configure_seat", config_.seat, config_.requestTimeoutMs, err);
                if (!response)
                    log("着席設定を送信できません: " + err.message);
                else if (!response->value("success", false))
                    log("着席設定（seat）が不正です: " + errorMessage(*response));
            }
            return ok("started", std::to_string(process_.dwProcessId));
        }
    }
    log("ランタイムへの接続がタイムアウトしました（起動は継続）");
    return fail("3D ランタイムの起動待ちがタイムアウトしました", ipc::error::timeout);
}

SaoriResponse SaoriBridge::status() {
    if (!processRunning())
        return ok("stopped");
    ipc::Error err;
    if (!ensureConnected(err))
        return ok("starting", err.message);
    auto response = client_.call("status", ipc::Json::object(), config_.requestTimeoutMs, err);
    if (!response)
        return ok(processRunning() ? "unresponsive" : "stopped", err.message);
    if (!response->value("success", false))
        return fail(errorMessage(*response), errorCode(*response));
    return ok("running", ipc::dump(response->value("result", ipc::Json::object())));
}

SaoriResponse SaoriBridge::forward(const std::string &command, const ipc::Json &params, DWORD timeoutMs) {
    ipc::Error err;
    if (!ensureConnected(err))
        return fail(err.message, err.code);
    auto response = client_.call(command, params, timeoutMs, err);
    if (!response) {
        log(std::format("{} 失敗: {}", command, err.message));
        return fail(err.message, err.code);
    }
    if (!response->value("success", false)) {
        log(std::format("{} エラー: {}", command, errorMessage(*response)));
        return fail(errorMessage(*response), errorCode(*response));
    }
    const auto result = response->value("result", ipc::Json::object());
    if (command == "get_accessory_status")
        return ok(result.value("effectiveVisible", false) ? "visible" :
                  (result.value("attached", false) ? "hidden" : "detached"), ipc::dump(result));
    return ok(command == "get_behavior_status" ? result.value("state", std::string("Idle")) : "OK",
              ipc::dump(result));
}

SaoriResponse SaoriBridge::shutdown() {
    if (!processRunning())
        return ok("stopped");
    ipc::Error err;
    if (ensureConnected(err))
        client_.call("shutdown", ipc::Json::object(), 1000, err);
    client_.close();
    if (WaitForSingleObject(process_.hProcess, 3000) != WAIT_OBJECT_0) {
        log("ランタイムが終了しないため強制終了します");
        TerminateProcess(process_.hProcess, 1);
        WaitForSingleObject(process_.hProcess, 1000);
    }
    log("ランタイム終了");
    closeProcess();
    return ok("stopped");
}

void SaoriBridge::shutdownRuntime() {
    std::lock_guard lock(mutex_);
    if (config_.shutdownOnUnload)
        shutdown();
    else
        closeProcess();
}

SaoriResponse SaoriBridge::execute(const std::vector<std::string> &arguments) {
    std::lock_guard lock(mutex_);
    if (arguments.empty() || arguments[0].empty())
        return fail("コマンド（Argument0）がありません", ipc::error::invalidRequest);
    const std::string command = lower(arguments[0]);
    const auto arg = [&](std::size_t i) -> std::string { return i < arguments.size() ? arguments[i] : std::string(); };
    log("EXECUTE " + command + (arguments.size() > 1 ? " " + arg(1) : std::string()));

    if (command == "launch")
        return launch(arguments);
    if (command == "status")
        return status();
    if (command == "shutdown")
        return shutdown();
    if (command == "ping")
        return forward("ping", ipc::Json::object(), config_.requestTimeoutMs);
    if (command == "version")
        return ok(kVersion);

    std::string error;
    if (command == "list_accessories")
        return forward(command, ipc::Json::object(), config_.requestTimeoutMs);
    if (command == "attach_accessory" || command == "detach_accessory" || command == "unload_accessory" ||
        command == "get_accessory_status" ||
        command == "set_accessory_visible") {
        ipc::Json params{{"id", arg(1)}};
        if (command == "attach_accessory") {
            if (!arg(2).empty())
                params["parentModel"] = arg(2) == "-" ? std::string() : arg(2);
            if (!arg(3).empty())
                params["parentBone"] = arg(3) == "-" ? std::string() : arg(3);
        }
        if (command == "set_accessory_visible") {
            const auto visible = parseBool(arg(2));
            if (!visible)
                return fail("visible は 1 / 0 で指定してください", ipc::error::invalidParams);
            params["visible"] = *visible;
        }
        return forward(command, params, config_.requestTimeoutMs);
    }
    if (command == "load_accessory" || command == "set_accessory_transform") {
        auto params = ipc::Json::parse(arg(2), nullptr, false);
        if (!params.is_object())
            return fail("第2引数には JSON オブジェクトを指定してください", ipc::error::invalidParams);
        params["id"] = arg(1);
        return forward(command, params, command == "load_accessory" ? config_.loadTimeoutMs : config_.requestTimeoutMs);
    }
    if (command == "stop_move" || command == "stand_up" || command == "get_behavior_status" ||
        command == "list_windows")
        return forward(command, ipc::Json::object(), config_.requestTimeoutMs);
    if (command == "configure_seat") {
        // configure_seat [anchorBone] [offsetX] [offsetY] [yaw] [inset] [transitionMs]
        //                [contactBones a,b,c] [useContact 1/0] [reset 1/0]
        // Empty arguments keep the current value ("-" clears anchorBone / contactBones).
        ipc::Json params = ipc::Json::object();
        if (!arg(1).empty())
            params["anchorBone"] = arg(1) == "-" ? std::string() : arg(1);
        const std::pair<std::size_t, const char *> numbers[] = {
            {2, "offsetX"}, {3, "offsetY"}, {4, "yaw"}, {5, "inset"}, {6, "transitionMs"}};
        for (const auto &[index, key] : numbers) {
            const auto text = arg(index);
            if (text.empty())
                continue;
            double value{};
            const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
            if (ec != std::errc{} || end != text.data() + text.size() || !std::isfinite(value))
                return fail(std::string(key) + " は数値で指定してください", ipc::error::invalidParams);
            params[key] = value;
        }
        if (const auto list = arg(7); !list.empty()) {
            auto bones = ipc::Json::array();
            if (list != "-") {
                std::size_t start = 0;
                while (start <= list.size()) {
                    const auto comma = std::min(list.find(',', start), list.size());
                    if (comma > start)
                        bones.push_back(list.substr(start, comma - start));
                    start = comma + 1;
                }
            }
            params["contactBones"] = std::move(bones);
        }
        const std::pair<std::size_t, const char *> flags[] = {{8, "useContact"}, {9, "reset"}};
        for (const auto &[index, key] : flags) {
            if (arg(index).empty())
                continue;
            const auto value = parseBool(arg(index));
            if (!value)
                return fail(std::string(key) + " は 1 / 0 で指定してください", ipc::error::invalidParams);
            params[key] = *value;
        }
        return forward(command, params, config_.requestTimeoutMs);
    }
    if (command == "move_to" || command == "sit_on_window" || command == "set_auto_behavior" ||
        command == "set_cross_monitor" || command == "set_topmost" || command == "configure_grab") {
        ipc::Json params = ipc::Json::object();
        const auto number = [&](std::size_t index, const char *key) {
            const auto text = arg(index);
            if (text.empty())
                return true;
            double value{};
            const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
            if (ec != std::errc{} || end != text.data() + text.size() || !std::isfinite(value)) {
                error = std::string(key) + " は数値で指定してください";
                return false;
            }
            params[key] = value;
            return true;
        };
        const auto boolean = [&](std::size_t index, const char *key) {
            const auto text = arg(index);
            if (text.empty() && std::string_view(key) != "enabled")
                return true;
            const auto value = parseBool(text);
            if (!value) {
                error = std::string(key) + " は 1 / 0 で指定してください";
                return false;
            }
            params[key] = *value;
            return true;
        };
        const auto motion = [&](std::size_t index, const char *key) {
            const auto text = arg(index);
            if (text.empty())
                return true;
            const auto path = resolvePath(text, true, error, true);
            if (!path)
                return false;
            params[key] = pathToUtf8(*path);
            return true;
        };
        const auto string = [&](std::size_t index, const char *key) {
            auto text = arg(index);
            if (!text.empty())
                params[key] = std::move(text);
        };
        bool valid;
        if (command == "move_to") {
            valid = number(1, "x") && number(2, "y") && number(3, "speed") &&
                    boolean(6, "useRootMotion") && boolean(7, "nextMonitor");
            if (!valid)
                return fail(error, ipc::error::invalidParams);
            valid = motion(4, "walkMotion") && motion(5, "idleMotion");
        } else if (command == "sit_on_window") {
            string(1, "target");
            string(2, "position");
            string(5, "anchorBone");
            if (!number(6, "anchorOffsetY"))
                return fail(error, ipc::error::invalidParams);
            valid = motion(3, "sitMotion") && motion(4, "idleMotion");
        } else if (command == "set_auto_behavior") {
            valid = boolean(1, "enabled") && number(2, "waitSeconds") && number(3, "speed") &&
                    number(7, "sitChance") && boolean(8, "useRootMotion");
            if (!valid)
                return fail(error, ipc::error::invalidParams);
            valid = motion(4, "walkMotion") && motion(5, "idleMotion") && motion(6, "sitMotion");
        } else if (command == "configure_grab") {
            const auto text = arg(3);
            if (!text.empty()) {
                int holdMs{};
                const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), holdMs);
                if (ec != std::errc{} || end != text.data() + text.size())
                    return fail("holdMs は整数で指定してください", ipc::error::invalidParams);
                params["holdMs"] = holdMs;
            }
            valid = motion(1, "motion") && motion(2, "idleMotion");
        } else {
            if (!boolean(1, "enabled"))
                return fail(error, ipc::error::invalidParams);
            return forward(command, params, config_.requestTimeoutMs);
        }
        if (!valid)
            return fail(error, ipc::error::forbiddenPath);
        return forward(command, params, config_.loadTimeoutMs);
    }

    if (command == "load_model") {
        const auto path = resolvePath(arg(1), false, error);
        if (!path)
            return fail(error, ipc::error::forbiddenPath);
        return forward("load_model", {{"path", pathToUtf8(*path)}}, config_.loadTimeoutMs);
    }
    if (command == "play_motion") {
        const auto path = resolvePath(arg(1), true, error);
        if (!path)
            return fail(error, ipc::error::forbiddenPath);
        ipc::Json params{{"path", pathToUtf8(*path)}};
        if (!arg(2).empty()) {
            const auto loop = parseBool(arg(2));
            if (!loop)
                return fail("loop は 1 / 0 で指定してください: " + arg(2), ipc::error::invalidParams);
            params["loop"] = *loop;
        }
        if (!arg(3).empty()) {
            const auto next = resolvePath(arg(3), true, error);
            if (!next)
                return fail(error, ipc::error::forbiddenPath);
            params["next"] = {{"path", pathToUtf8(*next)}, {"loop", true}};
        }
        // Argument4: camera track. Empty = <motion>.camera.json next to the VMD if
        // there is one, "none" (or 0) = no camera move, otherwise a .camera.json path.
        const std::string camera = arg(4);
        if (camera == "none" || camera == "0") {
            params["camera"] = false;
        } else if (!camera.empty()) {
            const auto cameraPath = resolvePath(camera, false, error);
            if (!cameraPath)
                return fail(error, ipc::error::forbiddenPath);
            params["camera"] = pathToUtf8(*cameraPath);
        }
        return forward("play_motion", params, config_.loadTimeoutMs);
    }
    if (command == "stop_motion")
        return forward("stop_motion", ipc::Json::object(), config_.requestTimeoutMs);
    if (command == "set_morph") {
        if (arg(1).empty())
            return fail("モーフ名がありません", ipc::error::invalidParams);
        double weight = 1.0;
        if (!arg(2).empty()) {
            const std::string text = arg(2);
            const auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), weight);
            if (ec != std::errc{} || ptr != text.data() + text.size())
                return fail("ウェイトは 0〜1 の数値で指定してください: " + text, ipc::error::invalidParams);
        }
        return forward("set_morph", {{"name", arg(1)}, {"weight", weight}}, config_.requestTimeoutMs);
    }
    if (command == "configure_face") {
        // configure_face [blink 1/0] [gaze 1/0] [gazeRadius px] [blinkMorph]; empty = unchanged
        ipc::Json params = ipc::Json::object();
        for (const auto &[index, key] : {std::pair<std::size_t, const char *>{1, "blink"}, {2, "gaze"}}) {
            if (arg(index).empty())
                continue;
            const auto value = parseBool(arg(index));
            if (!value)
                return fail(std::string(key) + " は 1 / 0 で指定してください", ipc::error::invalidParams);
            params[key] = *value;
        }
        if (!arg(3).empty()) {
            double radius = 0.0;
            const std::string text = arg(3);
            const auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), radius);
            if (ec != std::errc{} || ptr != text.data() + text.size() || radius < 0.0)
                return fail("gazeRadius は 0 以上の数値です（0 = 自動）", ipc::error::invalidParams);
            params["gazeRadius"] = radius;
        }
        if (!arg(4).empty())
            params["blinkMorph"] = arg(4);
        return forward("configure_face", params, config_.requestTimeoutMs);
    }
    if (command == "set_morphs") {
        // set_morphs <reset 1/0> <name> <weight> [<name> <weight> ...]
        const auto reset = parseBool(arg(1));
        if (!reset || arguments.size() % 2 != 0)
            return fail("set_morphs <reset 1/0> <モーフ名> <重み> ... の形で指定してください", ipc::error::invalidParams);
        ipc::Json weights = ipc::Json::object();
        for (std::size_t i = 2; i + 1 < arguments.size(); i += 2) {
            double weight = 0.0;
            const std::string &text = arguments[i + 1];
            const auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), weight);
            if (arguments[i].empty() || ec != std::errc{} || ptr != text.data() + text.size())
                return fail("モーフ名と重み（数値）の組が不正です: " + arguments[i], ipc::error::invalidParams);
            weights[arguments[i]] = weight;
        }
        return forward("set_morphs", {{"weights", weights}, {"reset", *reset}}, config_.requestTimeoutMs);
    }
    if (command == "reset_morph") {
        ipc::Json params = ipc::Json::object();
        if (!arg(1).empty())
            params["name"] = arg(1);
        return forward("reset_morph", params, config_.requestTimeoutMs);
    }
    return fail("不明なコマンドです: " + arguments[0], ipc::error::unknownCommand);
}

} // namespace uka3d::saori
