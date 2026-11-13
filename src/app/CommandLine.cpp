#include "app/CommandLine.h"

#include "ipc/Protocol.h"
#include "platform/Utf.h"

#include <charconv>
#include <format>
#include <optional>

namespace uka3d {
namespace {

std::optional<int> parseInt(std::wstring_view text) {
    const std::string utf8 = wideToUtf8(text);
    int value = 0;
    const auto [ptr, ec] = std::from_chars(utf8.data(), utf8.data() + utf8.size(), value);
    if (ec != std::errc{} || ptr != utf8.data() + utf8.size())
        return std::nullopt;
    return value;
}

} // namespace

bool parseFpsValue(std::wstring_view text, int &fps) {
    if (text == L"unlimited" || text == L"UNLIMITED" || text == L"Unlimited") {
        fps = 0;
        return true;
    }
    const auto number = parseInt(text);
    if (!number || *number < kMinFps || *number > kMaxFps)
        return false;
    fps = *number;
    return true;
}

ParseResult parseCommandLine(std::span<const std::wstring_view> args) {
    ParseResult result;
    auto &opt = result.options;
    const auto fail = [&](std::string message) {
        result.ok = false;
        result.error = std::move(message);
        return result;
    };

    for (std::size_t i = 0; i < args.size(); ++i) {
        std::wstring_view arg = args[i];
        std::wstring_view inlineValue;
        bool hasInlineValue = false;
        if (arg.starts_with(L"--")) {
            if (const auto eq = arg.find(L'='); eq != std::wstring_view::npos) {
                inlineValue = arg.substr(eq + 1);
                arg = arg.substr(0, eq);
                hasInlineValue = true;
            }
        }
        const std::string name = wideToUtf8(arg);
        const auto takeValue = [&]() -> std::optional<std::wstring_view> {
            if (hasInlineValue)
                return inlineValue;
            if (i + 1 >= args.size())
                return std::nullopt;
            return args[++i];
        };
        const auto rejectValue = [&]() { return hasInlineValue; };

        if (name == "--help" || name == "-h" || name == "/?") {
            opt.help = true;
        } else if (name == "--version") {
            opt.version = true;
        } else if (name == "--exit-after") {
            const auto value = takeValue();
            const auto number = value ? parseInt(*value) : std::nullopt;
            if (!number || *number < 1 || *number > 86400)
                return fail("--exit-after には 1〜86400 の秒数が必要です");
            opt.exitAfterSeconds = *number;
        } else if (name == "--ipc") {
            const auto value = takeValue();
            if (!value || !ipc::isValidInstanceId(wideToUtf8(*value)))
                return fail("--ipc にはインスタンスID（英数字と . _ - の 1〜64 文字）が必要です");
            opt.ipcInstance = wideToUtf8(*value);
        } else if (name == "--follow-window-class") {
            const auto value = takeValue();
            if (!value || value->empty() || value->size() > 128)
                return fail("--follow-window-class にはウィンドウクラス名が必要です");
            opt.followWindowClass = wideToUtf8(*value);
        } else if (name == "--fit") {
            const auto value = takeValue();
            if (!value || (*value != L"all" && *value != L"height"))
                return fail("--fit には all か height を指定してください");
            opt.fitHeight = *value == L"height";
        } else if (name == "--zoom") {
            const auto value = takeValue();
            const std::string text = value ? wideToUtf8(*value) : std::string();
            float zoom = 0.0F;
            const auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), zoom);
            if (!value || ec != std::errc{} || ptr != text.data() + text.size() || zoom < 0.2F || zoom > 5.0F)
                return fail("--zoom には 0.2〜5 の数値を指定してください");
            opt.zoom = zoom;
        } else if (name == "--blink" || name == "--no-blink" || name == "--gaze" || name == "--no-gaze") {
            if (rejectValue())
                return fail(std::format("{} は値を取りません", name));
            if (name == "--blink" || name == "--no-blink")
                opt.blink = name == "--blink";
            else
                opt.gaze = name == "--gaze";
        } else if (name == "--blink-morph") {
            const auto value = takeValue();
            if (!value || value->empty())
                return fail("--blink-morph にはモーフ名が必要です");
            opt.blinkMorph = wideToUtf8(*value);
        } else if (name == "--gaze-radius") {
            const auto value = takeValue();
            const std::string text = value ? wideToUtf8(*value) : std::string();
            float radius = -1.0F;
            const auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), radius);
            if (!value || ec != std::errc{} || ptr != text.data() + text.size() || radius < 0.0F || radius > 10000.0F)
                return fail("--gaze-radius には 0〜10000 のピクセル数を指定してください（0 = 自動）");
            opt.gazeRadius = radius;
        } else if (name == "--parent-pid") {
            const auto value = takeValue();
            const auto number = value ? parseInt(*value) : std::nullopt;
            if (!number || *number <= 0)
                return fail("--parent-pid にはプロセスIDが必要です");
            opt.parentPid = static_cast<unsigned long>(*number);
        } else if (name == "--allowed-root") {
            const auto value = takeValue();
            if (!value || value->empty())
                return fail("--allowed-root にはフォルダのパスが必要です");
            opt.allowedRoots.emplace_back(std::wstring(*value));
        } else if (name == "--model" || name == "--motion" || name == "--log" || name == "--capture" ||
                   name == "--hit-regions") {
            const auto value = takeValue();
            if (!value || value->empty())
                return fail(std::format("{} にはファイルパスが必要です", name));
            std::filesystem::path path{std::wstring(*value)};
            if (name == "--model")
                opt.modelPath = std::move(path);
            else if (name == "--motion")
                opt.motionPath = std::move(path);
            else if (name == "--capture")
                opt.capturePath = std::move(path);
            else if (name == "--hit-regions")
                opt.hitRegionsPath = std::move(path);
            else
                opt.logPath = std::move(path);
        } else if (name == "--width" || name == "--height") {
            const auto value = takeValue();
            if (!value)
                return fail(std::format("{} には数値が必要です", name));
            const auto number = parseInt(*value);
            if (!number || *number < 64 || *number > 8192)
                return fail(std::format("{} の値が不正です（64〜8192）: {}", name, wideToUtf8(*value)));
            (name == "--width" ? opt.width : opt.height) = *number;
        } else if (name == "--fps") {
            const auto value = takeValue();
            if (!value || !parseFpsValue(*value, opt.fps))
                return fail(std::format("--fps には {}〜{} の整数か unlimited を指定してください{}", kMinFps, kMaxFps,
                                        value ? ": " + wideToUtf8(*value) : std::string()));
        } else if (name == "--physics") {
            if (rejectValue())
                return fail("--physics は値を取りません");
            opt.physics = true;
        } else if (name == "--no-physics") {
            if (rejectValue())
                return fail("--no-physics は値を取りません");
            opt.physics = false;
        } else if (name == "--transparent") {
            if (rejectValue())
                return fail("--transparent は値を取りません");
            opt.windowMode = WindowMode::transparent;
        } else if (name == "--windowed") {
            if (rejectValue())
                return fail("--windowed は値を取りません");
            opt.windowMode = WindowMode::windowed;
        } else if (name == "--loop") {
            if (rejectValue())
                return fail("--loop は値を取りません");
            opt.loop = true;
        } else if (name == "--no-loop") {
            if (rejectValue())
                return fail("--no-loop は値を取りません");
            opt.loop = false;
        } else if (name == "--topmost") {
            if (rejectValue())
                return fail("--topmost は値を取りません");
            opt.topmost = true;
        } else if (name == "--debug") {
            if (rejectValue())
                return fail("--debug は値を取りません");
            opt.debug = true;
        } else {
            return fail(std::format("不明な引数です: {}", name));
        }
    }

    if (!opt.help && !opt.version && opt.modelPath.empty())
        return fail("--model でPMXモデルを指定してください");
    return result;
}

#ifndef UKA3D_VERSION
#define UKA3D_VERSION "0.1.1-beta"
#endif

std::string usageText() {
    return std::string("ukagaka3d - 伺か向け3Dシェル基盤 v") + UKA3D_VERSION + R"(

使い方:
  ukagaka3d.exe --model <model.pmx> [--motion <motion.vmd>] [オプション]

オプション:
  --model <path>    PMXモデルのパス（必須）
  --motion <path>   VMDモーションのパス（任意。未指定時は静止表示）
  --transparent     背景透過ウィンドウで表示（既定）
  --windowed        通常ウィンドウで表示
  --width <px>      ウィンドウ幅（既定 480）
  --height <px>     ウィンドウ高さ（既定 720）
  --loop            モーションをループ再生（既定）
  --no-loop         モーションを最終フレームで停止
  --topmost         常に最前面に表示
  --no-blink        自動まばたきをしない（既定はする。モーフ「まばたき」など）
  --blink-morph <名前>  まばたきに使うモーフ
  --no-gaze         マウスを目で追わない（既定は近くに来たら目と頭を向ける）
  --gaze-radius <px>  視線が反応する頭からの距離（96dpi 換算、0 = 自動）
  --fit <all|height>  初期カメラ: モデル全体を収める（既定）/ 身長に合わせて大きく表示
  --zoom <倍率>     カメラの寄り（1 = 既定、2 = 2 倍に拡大）
  --fps <n>         描画フレームレートの上限（30 / 60 / 120 など 10〜240、または unlimited。既定 60）
  --physics         物理演算を有効化（既定）
  --no-physics      物理演算を無効化
  --debug           デバッグログを有効化
  --log <path>      ログファイルの出力先（既定: 実行ファイルと同じフォルダの ukagaka3d.log）
  --version         バージョンを表示
  --help            この使い方を表示

伺か連携（通常は ukagaka3d_saori.dll が指定します）:
  --ipc <id>        名前付きパイプ \\.\pipe\ukagaka3d.<id> で制御コマンドを受け付ける
  --parent-pid <n>  指定プロセスの終了時に自動終了し、そのプロセスからの接続だけを許可する
  --allowed-root <dir>  IPC で指定できるファイルの範囲（複数指定可）
  --hit-regions <path>  当たり判定プロファイルの JSON（起動元ゴーストへのイベント用）
  --follow-window-class <class>  起動元プロセスの指定クラスのウィンドウ（SSP 本体側シェル: Tmainform）に
                    位置と大きさを合わせる（ドラッグするとそのウィンドウを動かす）

開発・検証用:
  --exit-after <秒> 指定秒数後に自動終了
  --capture <png>   終了時に最終フレームをPNG（アルファ付き）で保存

操作:
  左ドラッグ        ウィンドウ移動（透過モードではモデル部分をドラッグ）
  右クリック        メニュー（再生/一時停止・先頭に戻す・物理演算・最前面・終了）
  Space             再生/一時停止
  P                 物理演算 ON/OFF
  Home              先頭フレームに戻す
  Esc               終了
)";
}

} // namespace uka3d
