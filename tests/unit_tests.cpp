// Minimal self-contained unit tests for ukagaka3d (no external framework).
//   uka3d_tests <test-assets-dir>            run unit tests
//   uka3d_tests --verify-captures <dir>      verify PNG frames written by GUI smoke tests
#include "app/CommandLine.h"
#include "app/DesktopBehavior.h"
#include "app/Interaction.h"
#include "app/Playback.h"
#include "platform/FramePacer.h"
#include "mmd/MmdModel.h"
#include "platform/Utf.h"
#include "platform/SstpEvents.h"
#include "renderer/Camera.h"
#include "renderer/TextureLoader.h"

#include <windows.h>

#include <psapi.h>

#include <DirectXMath.h>

#include <cmath>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace uka3d;

namespace {

int g_failures = 0;
int g_checks = 0;

void report(bool ok, const char *expr, const char *file, int line) {
    ++g_checks;
    if (!ok) {
        ++g_failures;
        std::printf("  FAILED: %s (%s:%d)\n", expr, file, line);
    }
}
#define CHECK(expr) report(static_cast<bool>(expr), #expr, __FILE__, __LINE__)

template <typename F> bool throwsLoadError(F &&f) {
    try {
        f();
    } catch (const LoadError &e) {
        std::printf("    (expected LoadError) %s\n", e.what());
        return true;
    } catch (...) {
        return false;
    }
    return false;
}

ParseResult parse(std::initializer_list<const wchar_t *> args) {
    std::vector<std::wstring_view> views(args.begin(), args.end());
    return parseCommandLine(views);
}

void testCommandLine() {
    {
        const auto r = parse({L"--help"});
        CHECK(r.ok && r.options.help);
    }
    {
        const auto r = parse({});
        CHECK(!r.ok);
    }
    {
        const auto r = parse({L"--model", L"モデル/テスト.pmx", L"--motion", L"待機.vmd", L"--width=640", L"--height",
                              L"800", L"--windowed", L"--no-loop", L"--debug", L"--topmost"});
        CHECK(r.ok);
        CHECK(r.options.modelPath == fs::path(L"モデル/テスト.pmx"));
        CHECK(r.options.motionPath == fs::path(L"待機.vmd"));
        CHECK(r.options.width == 640 && r.options.height == 800);
        CHECK(r.options.windowMode == WindowMode::windowed);
        CHECK(!r.options.loop && r.options.debug && r.options.topmost);
    }
    {
        const auto r = parse({L"--model", L"a.pmx"});
        CHECK(r.ok && r.options.windowMode == WindowMode::transparent && r.options.loop);
    }
    CHECK(!parse({L"--model", L"a.pmx", L"--width", L"abc"}).ok);
    CHECK(!parse({L"--model", L"a.pmx", L"--width", L"10"}).ok);
    CHECK(!parse({L"--model"}).ok);
    CHECK(!parse({L"--model", L"a.pmx", L"--unknown"}).ok);
    CHECK(!parse({L"--model", L"a.pmx", L"--windowed=1"}).ok);
    CHECK(parse({L"--model", L"a.pmx", L"--exit-after", L"5"}).options.exitAfterSeconds == 5.0);
    // Phase 1: IPC / SSP options
    {
        const auto r = parse({L"--model", L"a.pmx", L"--ipc", L"ghost-1a2b.3_x", L"--parent-pid", L"1234",
                              L"--allowed-root", L"C:\\ghost", L"--allowed-root", L"D:\\shared",
                              L"--follow-window-class", L"Tmainform", L"--fit", L"height", L"--zoom", L"1.5"});
        CHECK(r.ok);
        CHECK(r.options.ipcInstance == "ghost-1a2b.3_x" && r.options.parentPid == 1234);
        CHECK(r.options.allowedRoots.size() == 2 && r.options.followWindowClass == "Tmainform");
        CHECK(r.options.fitHeight && r.options.zoom == 1.5F);
    }
    CHECK(!parse({L"--model", L"a.pmx", L"--ipc", L"bad id"}).ok);
    CHECK(!parse({L"--model", L"a.pmx", L"--ipc", L"\\\\.\\pipe\\x"}).ok);
    CHECK(!parse({L"--model", L"a.pmx", L"--parent-pid", L"-3"}).ok);
    CHECK(!parse({L"--model", L"a.pmx", L"--fit", L"width"}).ok);
    CHECK(!parse({L"--model", L"a.pmx", L"--zoom", L"100"}).ok);
    CHECK(!parse({L"--model", L"a.pmx", L"--allowed-root"}).ok);
}

void testUtf() {
    const std::string text = "日本語フォルダ/テクスチャ①.png";
    CHECK(wideToUtf8(utf8ToWide(text)) == text);
    CHECK(pathToUtf8(utf8ToPath(text)) == text);
    CHECK(utf8ToWide("").empty());
}

void testModel(const fs::path &assets) {
    const fs::path modelPath = assets / L"テスト用モデル" / L"テストキャラ.pmx";
    const fs::path motionPath = assets / L"モーション" / L"待機テスト.vmd";

    MmdModel model;
    model.loadModel(modelPath);
    CHECK(model.loaded());
    CHECK(model.model().vertices.size() > 100);
    CHECK(model.model().materials.size() == 5);
    CHECK(model.model().bones.size() == 5);
    CHECK(model.model().metadata.modelName == "テストキャラ");
    CHECK(fs::exists(model.resolveTexturePath(0)));  // tex\顔.png
    CHECK(fs::exists(model.resolveTexturePath(1)));  // tex\光輪.tga
    CHECK(!fs::exists(model.resolveTexturePath(3))); // intentionally missing
    CHECK(model.resolveTexturePath(0).filename() == fs::path(L"顔.png"));

    const Bounds bounds = model.bindPoseBounds();
    CHECK(bounds.valid);
    CHECK(bounds.maximum[1] > 18.0F && bounds.minimum[1] < 0.5F);

    // Static pose: evaluating without motion keeps bind-pose vertices.
    const auto staticFrame = model.evaluate(0.0F);
    CHECK(staticFrame.vertices.size() == model.model().vertices.size());
    float staticDelta = 0.0F;
    for (std::size_t i = 0; i < staticFrame.vertices.size(); ++i)
        staticDelta = std::max(staticDelta, std::abs(staticFrame.vertices[i].position[0] -
                                                     model.model().vertices[i].position[0]));
    CHECK(staticDelta < 1e-4F);

    model.loadMotion(motionPath);
    CHECK(model.hasMotion());
    CHECK(model.motionLastFrame() == 120.0F);
    const std::vector<mmd::PmxVertex> frame0 = model.evaluate(0.0F).vertices;
    const std::vector<mmd::PmxVertex> frame30 = model.evaluate(30.0F).vertices;
    CHECK(frame0.size() == frame30.size());
    float maxDelta = 0.0F;
    for (std::size_t i = 0; i < frame0.size(); ++i)
        for (int a = 0; a < 3; ++a)
            maxDelta = std::max(maxDelta, std::abs(frame0[i].position[a] - frame30[i].position[a]));
    std::printf("    motion: max vertex displacement frame0->30 = %.3f\n", maxDelta);
    CHECK(maxDelta > 0.2F);

    // Error paths
    CHECK(throwsLoadError([&] { MmdModel m; m.loadModel(assets / L"存在しない.pmx"); }));
    CHECK(throwsLoadError([&] { MmdModel m; m.loadModel(assets / L"broken" / L"壊れたモデル.pmx"); }));
    CHECK(throwsLoadError([&] { MmdModel m; m.loadModel(assets / L"broken" / L"偽物.pmx"); }));
    CHECK(throwsLoadError([&] { MmdModel m; m.loadModel(assets / L"テスト用モデル"); })); // directory
    CHECK(throwsLoadError([&] {
        MmdModel m;
        m.loadModel(modelPath);
        m.loadMotion(assets / L"broken" / L"壊れたモーション.vmd");
    }));
}

void testImages(const fs::path &assets) {
    const fs::path tex = assets / L"テスト用モデル" / L"tex";
    const Image png = decodeImageFile(tex / L"顔.png");
    CHECK(png.width == 128 && png.height == 128 && !png.hasTranslucency());
    const Image tga = decodeImageFile(tex / L"光輪.tga");
    CHECK(tga.width == 128 && tga.hasTranslucency());
    const Image bmp = decodeImageFile(tex / L"体.bmp");
    CHECK(bmp.width == 64 && bmp.rgba.size() == 64u * 64u * 4u);
    bool threw = false;
    try {
        static_cast<void>(decodeImageFile(tex / L"存在しない.png"));
    } catch (const std::exception &) {
        threw = true;
    }
    CHECK(threw);
}

void testCamera() {
    Bounds bounds;
    bounds.valid = true;
    bounds.minimum = {-5.0F, 0.0F, -2.0F};
    bounds.maximum = {5.0F, 20.0F, 2.0F};
    for (float aspect : {0.5F, 1.0F, 2.0F}) {
        Camera camera;
        camera.fitToBounds(bounds, aspect);
        const DirectX::XMMATRIX vp = camera.view() * camera.projection(aspect);
        bool inside = true;
        for (int i = 0; i < 8; ++i) {
            const DirectX::XMVECTOR p = DirectX::XMVectorSet(i & 1 ? 5.0F : -5.0F, i & 2 ? 20.0F : 0.0F,
                                                             i & 4 ? 2.0F : -2.0F, 1.0F);
            DirectX::XMFLOAT4 clip;
            DirectX::XMStoreFloat4(&clip, DirectX::XMVector4Transform(p, vp));
            const float x = clip.x / clip.w, y = clip.y / clip.w, z = clip.z / clip.w;
            inside = inside && clip.w > 0 && std::abs(x) <= 1.0F && std::abs(y) <= 1.0F && z >= 0 && z <= 1;
        }
        CHECK(inside);
    }
    // Height-only framing gets closer than whole-model framing for a wide model.
    {
        Bounds wide;
        wide.valid = true;
        wide.minimum = {-10.0F, 0.0F, -1.0F};
        wide.maximum = {10.0F, 16.0F, 1.0F};
        Camera all;
        all.fitToBounds(wide, 0.56F);
        Camera height;
        height.fitToBounds(wide, 0.56F, 1.02F, true);
        CHECK(height.distance < all.distance * 0.6F);
    }
}

// ---------------------------------------------------------------------------
// Phase 0.5: FPS options, fixed-step playback, physics
// ---------------------------------------------------------------------------
void testFpsOptions() {
    int fps = -1;
    CHECK(parseFpsValue(L"30", fps) && fps == 30);
    CHECK(parseFpsValue(L"60", fps) && fps == 60);
    CHECK(parseFpsValue(L"120", fps) && fps == 120);
    CHECK(parseFpsValue(L"unlimited", fps) && fps == 0);
    CHECK(!parseFpsValue(L"0", fps));
    CHECK(!parseFpsValue(L"5", fps));
    CHECK(!parseFpsValue(L"1000", fps));
    CHECK(!parseFpsValue(L"abc", fps));
    CHECK(!parseFpsValue(L"60.5", fps));
    CHECK(!parseFpsValue(L"", fps));

    CHECK(parse({L"--model", L"a.pmx"}).options.fps == 60); // default
    CHECK(parse({L"--model", L"a.pmx", L"--fps", L"30"}).options.fps == 30);
    CHECK(parse({L"--model", L"a.pmx", L"--fps=120"}).options.fps == 120);
    CHECK(parse({L"--model", L"a.pmx", L"--fps", L"unlimited"}).options.fps == 0);
    CHECK(!parse({L"--model", L"a.pmx", L"--fps", L"abc"}).ok);
    CHECK(!parse({L"--model", L"a.pmx", L"--fps", L"0"}).ok);
    CHECK(!parse({L"--model", L"a.pmx", L"--fps"}).ok);

    CHECK(parse({L"--model", L"a.pmx"}).options.physics); // default on
    CHECK(!parse({L"--model", L"a.pmx", L"--no-physics"}).options.physics);
    CHECK(parse({L"--model", L"a.pmx", L"--no-physics", L"--physics"}).options.physics);
    CHECK(!parse({L"--model", L"a.pmx", L"--physics=1"}).ok);
}

Playback makePlayback(bool fixed, bool motion = true, float last = 120.0F, bool loop = true) {
    Playback::Config config;
    config.hasMotion = motion;
    config.lastFrame = last;
    config.loop = loop;
    config.fixedStep = fixed;
    return Playback(config);
}

void testPlayback() {
    // First update is always a seek (initialises pose and physics).
    {
        Playback p = makePlayback(true);
        const auto first = p.update(0.016);
        CHECK(first.kind == Playback::Kind::seek && first.frame == 0.0F && first.seconds == 0.0F);
    }
    // 30 fps rendering: two 1/60 s ticks per frame, one evaluation each frame.
    {
        Playback p = makePlayback(true);
        p.update(0.0);
        int evaluations = 0;
        for (int i = 0; i < 60; ++i) { // 2 seconds
            const auto s = p.update(1.0 / 30.0);
            if (s.kind != Playback::Kind::none) {
                ++evaluations;
                CHECK(s.ticks == 2 || s.ticks == 1 || s.ticks == 3);
            }
        }
        std::printf("    30fps: ticks=%llu evaluations=%d frame=%.2f\n",
                    static_cast<unsigned long long>(p.totalTicks()), evaluations, p.frame());
        CHECK(p.totalTicks() >= 119 && p.totalTicks() <= 120);
        CHECK(std::abs(p.frame() - 60.0) < 0.6); // 2 s * 30 frames/s
    }
    // 120 fps rendering: one tick every other frame -> physics is not over-advanced.
    {
        Playback p = makePlayback(true);
        p.update(0.0);
        int idle = 0;
        for (int i = 0; i < 240; ++i) {
            const auto s = p.update(1.0 / 120.0);
            if (s.kind == Playback::Kind::none)
                ++idle;
            else
                CHECK(s.ticks == 1);
            const float alpha = p.interpolationAlpha();
            CHECK(alpha >= 0.0F && alpha <= 1.0F);
        }
        std::printf("    120fps: ticks=%llu idle-frames=%d\n", static_cast<unsigned long long>(p.totalTicks()),
                    idle);
        CHECK(p.totalTicks() >= 119 && p.totalTicks() <= 120);
        CHECK(idle >= 119 && idle <= 121);
    }
    // A 2-second stall is clamped to kMaxTicksPerUpdate ticks; the rest is dropped.
    {
        Playback p = makePlayback(true);
        p.update(0.0);
        const auto s = p.update(2.0);
        CHECK(s.ticks == Playback::kMaxTicksPerUpdate);
        CHECK(p.droppedSeconds() > 1.8);
        Playback v = makePlayback(false);
        v.update(0.0);
        const auto sv = v.update(2.0);
        CHECK(sv.seconds <= Playback::kMaxVariableStep + 1e-6);
    }
    // Pause: nothing is evaluated and the simulation does not advance; resume continues.
    {
        Playback p = makePlayback(true);
        p.update(0.0);
        for (int i = 0; i < 30; ++i)
            p.update(1.0 / 60.0);
        const double frame = p.frame();
        const auto ticks = p.totalTicks();
        p.setPaused(true);
        bool anything = false;
        for (int i = 0; i < 120; ++i)
            anything |= p.update(1.0 / 60.0).kind != Playback::Kind::none;
        CHECK(!anything);
        CHECK(p.frame() == frame && p.totalTicks() == ticks);
        p.setPaused(false);
        const auto s = p.update(1.0 / 60.0);
        CHECK(s.kind == Playback::Kind::advance && s.ticks == 1);
        CHECK(std::abs(p.frame() - (frame + 0.5)) < 1e-6);
    }
    // Rewind -> seek to frame 0 with no simulation time.
    {
        Playback p = makePlayback(true);
        p.update(0.0);
        for (int i = 0; i < 90; ++i)
            p.update(1.0 / 60.0);
        p.rewind();
        const auto s = p.update(1.0 / 60.0);
        CHECK(s.kind == Playback::Kind::seek && s.frame == 0.0F && s.seconds == 0.0F);
        CHECK(p.frame() == 0.0);
    }
    // Loop boundary: frame wraps and the step is flagged as a wrap (physics continuity).
    {
        Playback p = makePlayback(true, true, 120.0F, true);
        p.update(0.0);
        int wraps = 0;
        double previous = 0.0;
        for (int i = 0; i < 60 * 9; ++i) { // 9 seconds of a 4 second loop
            const auto s = p.update(1.0 / 60.0);
            if (s.kind == Playback::Kind::wrap) {
                ++wraps;
                CHECK(s.frame < previous);
                CHECK(s.frame >= 0.0F && s.frame < 1.0F);
            }
            previous = p.frame();
        }
        CHECK(wraps == 2 && p.loopCount() == 2);
        // Without looping the frame stays at the end (physics keeps settling).
        Playback once = makePlayback(true, true, 120.0F, false);
        once.update(0.0);
        for (int i = 0; i < 60 * 6; ++i)
            CHECK(once.update(1.0 / 60.0).kind != Playback::Kind::wrap);
        CHECK(once.frame() == 120.0);
    }
    // Physics off, no motion: nothing to evaluate after the initial seek (idle, no redraws).
    {
        Playback p = makePlayback(false, false);
        CHECK(p.update(0.016).kind == Playback::Kind::seek);
        CHECK(p.update(0.016).kind == Playback::Kind::none);
    }
    // Physics toggle forces a re-synchronising seek at the current frame.
    {
        Playback p = makePlayback(false);
        p.update(0.0);
        p.update(1.0);
        const double frame = p.frame();
        p.setFixedStep(true);
        const auto s = p.update(0.016);
        CHECK(s.kind == Playback::Kind::seek && std::abs(s.frame - frame) < 1e-4);
    }
}

void testFramePacer() {
    using Mode = FramePacer::Mode;
    {
        FramePacer p(60);
        p.configure(120.0);
        CHECK(p.mode() == Mode::vsync && p.syncInterval() == 2);
        p.configure(59.0); // 59.94 Hz reported as 59
        CHECK(p.mode() == Mode::vsync && p.syncInterval() == 1);
        p.configure(144.0); // not an integer multiple -> timer, no VSync wait
        CHECK(p.mode() == Mode::timer && p.syncInterval() == 0);
        p.configure(240.0);
        CHECK(p.mode() == Mode::vsync && p.syncInterval() == 4);
        p.configure(0.0); // unknown refresh rate
        CHECK(p.mode() == Mode::timer);
    }
    {
        FramePacer p(30);
        p.configure(120.0);
        CHECK(p.mode() == Mode::vsync && p.syncInterval() == 4);
        p.configure(165.0);
        CHECK(p.mode() == Mode::timer);
    }
    {
        FramePacer p(120);
        p.configure(60.0); // target above refresh: display caps it
        CHECK(p.mode() == Mode::vsync && p.syncInterval() == 1);
    }
    {
        FramePacer p(0);
        p.configure(120.0);
        CHECK(p.mode() == Mode::unlimited && p.syncInterval() == 0 && p.frameDue());
    }
    // Timer mode paces with a waitable timer: ~60 frames/s, little CPU time.
    {
        FramePacer p(60);
        p.configure(144.0);
        LARGE_INTEGER f, t0, t1;
        QueryPerformanceFrequency(&f);
        FILETIME c, e, k0, u0, k1, u1;
        GetProcessTimes(GetCurrentProcess(), &c, &e, &k0, &u0);
        QueryPerformanceCounter(&t0);
        int frames = 0;
        for (;;) {
            QueryPerformanceCounter(&t1);
            if (static_cast<double>(t1.QuadPart - t0.QuadPart) / f.QuadPart >= 1.0)
                break;
            if (!p.frameDue()) {
                p.waitForFrameOrMessage();
                continue;
            }
            ++frames;
            p.framePresented();
        }
        GetProcessTimes(GetCurrentProcess(), &c, &e, &k1, &u1);
        const auto ft = [](const FILETIME &x) {
            return static_cast<double>((static_cast<unsigned long long>(x.dwHighDateTime) << 32) | x.dwLowDateTime) /
                   1e7;
        };
        const double cpu = ft(k1) - ft(k0) + ft(u1) - ft(u0);
        std::printf("    timer mode: %d frames in 1 s, CPU %.1f%%\n", frames, cpu * 100.0);
        CHECK(frames >= 57 && frames <= 62);
        CHECK(cpu < 0.2); // no busy waiting
    }
}

// Centroid of the vertices skinned to `boneName` in an evaluated frame.
mmd::Float3 boneCentroid(const MmdModel &model, const mmd::AnimatedModelFrame &frame, const std::string &boneName) {
    const auto &bones = model.model().bones;
    int index = -1;
    for (std::size_t i = 0; i < bones.size(); ++i)
        if (bones[i].name == boneName)
            index = static_cast<int>(i);
    mmd::Float3 sum{};
    int count = 0;
    for (std::size_t i = 0; i < frame.vertices.size(); ++i) {
        if (model.model().vertices[i].bones[0] != index)
            continue;
        for (int a = 0; a < 3; ++a)
            sum[a] += frame.vertices[i].position[a];
        ++count;
    }
    if (count > 0)
        for (auto &v : sum)
            v /= static_cast<float>(count);
    return sum;
}

float distance(const mmd::Float3 &a, const mmd::Float3 &b) {
    return std::sqrt((a[0] - b[0]) * (a[0] - b[0]) + (a[1] - b[1]) * (a[1] - b[1]) + (a[2] - b[2]) * (a[2] - b[2]));
}

// Drives a model through Playback exactly like the app does.
mmd::Float3 simulate(MmdModel &model, Playback &playback, double renderDt, double seconds) {
    const int frames = static_cast<int>(std::lround(seconds / renderDt));
    for (int i = -1; i < frames; ++i) {
        // i == -1: consume a pending initial seek without spending simulated time.
        const auto s = playback.update(i < 0 ? 0.0 : renderDt);
        switch (s.kind) {
        case Playback::Kind::seek:
            model.resetPhysics();
            model.evaluate(s.frame, 0.0F);
            break;
        case Playback::Kind::advance:
            model.evaluate(s.frame, s.seconds);
            break;
        case Playback::Kind::wrap:
            model.evaluateAcrossLoop(s.frame, s.seconds);
            break;
        case Playback::Kind::none:
            break;
        }
    }
    return boneCentroid(model, model.evaluate(static_cast<float>(playback.frame()), 0.0F), "髪3");
}

void testPhysics(const fs::path &assets) {
    const fs::path physicsModel = assets / L"テスト用モデル" / L"物理テストキャラ.pmx";
    const fs::path plainModel = assets / L"テスト用モデル" / L"テストキャラ.pmx";
    const fs::path motion = assets / L"モーション" / L"待機テスト.vmd";
    const mmd::Float3 bindTip{0.0F, 16.3F, 6.4F};

    // 4) Physics model loads with its rigid bodies and joints.
    MmdModel model;
    model.loadModel(physicsModel);
    CHECK(model.model().rigidBodies.size() == 4 && model.model().joints.size() == 3);
    CHECK(model.setupPhysics({}) == PhysicsStatus::active);
    CHECK(model.physicsAvailable() && model.physicsEnabled());

    // 3) Physics ON: the horizontal ponytail falls under gravity.
    {
        Playback p = makePlayback(true, false);
        const auto tip = simulate(model, p, 1.0 / 60.0, 2.0);
        std::printf("    physics ON  tip=(%.2f, %.2f, %.2f)\n", tip[0], tip[1], tip[2]);
        CHECK(tip[1] < bindTip[1] - 2.0F);
        CHECK(std::isfinite(tip[0]) && std::isfinite(tip[1]) && std::isfinite(tip[2]));
    }
    // 3) Physics OFF: animation only, the ponytail stays in its bind pose.
    {
        CHECK(!model.setPhysicsEnabled(false));
        Playback p = makePlayback(false, false);
        const auto tip = simulate(model, p, 1.0 / 60.0, 1.0);
        std::printf("    physics OFF tip=(%.2f, %.2f, %.2f)\n", tip[0], tip[1], tip[2]);
        CHECK(distance(tip, bindTip) < 0.05F);
        CHECK(model.setPhysicsEnabled(true));
    }

    // 5) Fixed step: the same motion rendered at 30 fps and 120 fps ends in the same state.
    mmd::Float3 tip30{}, tip120{};
    {
        MmdModel a;
        a.loadModel(physicsModel);
        a.loadMotion(motion);
        a.setupPhysics({});
        Playback pa = makePlayback(true, true, a.motionLastFrame());
        tip30 = simulate(a, pa, 1.0 / 30.0, 3.0);
        MmdModel b;
        b.loadModel(physicsModel);
        b.loadMotion(motion);
        b.setupPhysics({});
        Playback pb = makePlayback(true, true, b.motionLastFrame());
        tip120 = simulate(b, pb, 1.0 / 120.0, 3.0);
        std::printf("    30fps tip=(%.3f, %.3f, %.3f) 120fps tip=(%.3f, %.3f, %.3f) diff=%.4f ticks %llu/%llu\n",
                    tip30[0], tip30[1], tip30[2], tip120[0], tip120[1], tip120[2], distance(tip30, tip120),
                    static_cast<unsigned long long>(pa.totalTicks()), static_cast<unsigned long long>(pb.totalTicks()));
        CHECK(pa.totalTicks() == pb.totalTicks());
        // Same number of 1/60 s ticks. At 30 fps libmmd simulates two ticks in one
        // evaluate() and interpolates the kinematic (head) bodies between them,
        // so a small difference (~1% of the 5-unit chain) remains.
        CHECK(distance(tip30, tip120) < 0.15F);
    }

    // 6) Pause: no evaluation -> the simulated state does not move.
    {
        MmdModel m;
        m.loadModel(physicsModel);
        m.loadMotion(motion);
        m.setupPhysics({});
        Playback p = makePlayback(true, true, m.motionLastFrame());
        simulate(m, p, 1.0 / 60.0, 1.0);
        const auto before = boneCentroid(m, m.evaluate(static_cast<float>(p.frame()), 0.0F), "髪3");
        p.setPaused(true);
        const auto during = simulate(m, p, 1.0 / 60.0, 2.0);
        CHECK(distance(before, during) < 1e-4F);
        p.setPaused(false);
        const auto after = simulate(m, p, 1.0 / 60.0, 0.5);
        CHECK(std::isfinite(after[1]));
    }

    // 7) Rewind: the physics state is re-initialised from the frame-0 pose.
    {
        MmdModel m;
        m.loadModel(physicsModel);
        m.setupPhysics({});
        Playback p = makePlayback(true, false);
        const auto fallen = simulate(m, p, 1.0 / 60.0, 2.0);
        CHECK(fallen[1] < bindTip[1] - 2.0F);
        p.rewind();
        const auto s = p.update(1.0 / 60.0);
        CHECK(s.kind == Playback::Kind::seek);
        m.resetPhysics();
        const auto reset = boneCentroid(m, m.evaluate(s.frame, 0.0F), "髪3");
        std::printf("    after rewind tip=(%.2f, %.2f, %.2f)\n", reset[0], reset[1], reset[2]);
        CHECK(distance(reset, bindTip) < 0.05F);
    }

    // 8) Loop boundary: dynamic bodies keep their state across the wrap;
    //    a plain backward evaluate would reset them (the ponytail would jump).
    {
        MmdModel m;
        m.loadModel(physicsModel);
        m.loadMotion(motion);
        m.setupPhysics({});
        const float last = m.motionLastFrame();
        // Run to just before the end of the motion.
        for (float f = 0.0F; f <= last - 0.5F; f += 0.5F)
            m.evaluate(f, 1.0F / 60.0F);
        const auto before = boneCentroid(m, m.evaluate(last - 0.5F, 0.0F), "髪3");
        const auto acrossLoop = boneCentroid(m, m.evaluateAcrossLoop(0.0F, 1.0F / 60.0F), "髪3");
        const float smooth = distance(before, acrossLoop);

        MmdModel naive;
        naive.loadModel(physicsModel);
        naive.loadMotion(motion);
        naive.setupPhysics({});
        for (float f = 0.0F; f <= last - 0.5F; f += 0.5F)
            naive.evaluate(f, 1.0F / 60.0F);
        const auto reset = boneCentroid(naive, naive.evaluate(0.0F, 1.0F / 60.0F), "髪3");
        const float jump = distance(before, reset);
        std::printf("    loop wrap: preserved move=%.3f, naive reset move=%.3f\n", smooth, jump);
        CHECK(smooth < 0.5F);
        CHECK(jump > 2.0F);
    }

    // 9) Backend unavailable / no rigid bodies: safe fallback to animation only.
    {
        MmdModel m;
        m.loadModel(physicsModel);
        m.loadMotion(motion);
        PhysicsOptions options;
        options.simulateMissingBackend = true;
        CHECK(m.setupPhysics(options) == PhysicsStatus::unavailable);
        CHECK(!m.physicsAvailable() && !m.physicsEnabled());
        CHECK(!m.setPhysicsEnabled(true));
        Playback p = makePlayback(m.physicsEnabled(), true, m.motionLastFrame());
        const auto tip = simulate(m, p, 1.0 / 60.0, 1.0);
        CHECK(std::isfinite(tip[1]) && std::abs(tip[1] - bindTip[1]) < 3.0F); // head motion only, no falling
        CHECK(p.totalTicks() == 0);                                           // variable step without physics

        MmdModel plain;
        plain.loadModel(plainModel);
        CHECK(plain.setupPhysics({}) == PhysicsStatus::noRigidBodies);
        CHECK(!plain.physicsEnabled());
        CHECK(plain.evaluate(0.0F, 1.0F / 60.0F).vertices.size() == plain.model().vertices.size());

        MmdModel disabled;
        disabled.loadModel(physicsModel);
        PhysicsOptions off;
        off.enabled = false;
        CHECK(disabled.setupPhysics(off) == PhysicsStatus::disabledByUser);
        CHECK(disabled.physicsAvailable() && !disabled.physicsEnabled());
        CHECK(disabled.setPhysicsEnabled(true));
    }

    // 10) Repeated load / simulate / destroy: no crash and no unbounded memory growth.
    {
        const auto privateBytes = [] {
            PROCESS_MEMORY_COUNTERS_EX memory{};
            memory.cb = sizeof(memory);
            GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS *>(&memory),
                                 sizeof(memory));
            return static_cast<double>(memory.PrivateUsage) / 1048576.0;
        };
        const auto cycle = [&] {
            MmdModel m;
            m.loadModel(physicsModel);
            m.loadMotion(motion);
            m.setupPhysics({});
            Playback p = makePlayback(true, true, m.motionLastFrame());
            simulate(m, p, 1.0 / 60.0, 0.5);
            m.setPhysicsEnabled(false);
            m.setPhysicsEnabled(true);
        };
        for (int i = 0; i < 5; ++i)
            cycle(); // warm-up (allocator pools)
        const double start = privateBytes();
        for (int i = 0; i < 50; ++i)
            cycle();
        const double growth = privateBytes() - start;
        std::printf("    50 load/destroy cycles: private bytes growth %.2f MB\n", growth);
        CHECK(growth < 5.0);
    }
}

int verifyCaptures(const fs::path &dir) {
    const auto stats = [](const Image &img, auto predicate) {
        std::size_t count = 0;
        for (std::size_t i = 0; i < img.rgba.size(); i += 4)
            count += predicate(&img.rgba[i]) ? 1 : 0;
        return count;
    };
    for (const wchar_t *name : {L"windowed_static.png", L"windowed_motion.png"}) {
        const Image img = decodeImageFile(dir / name);
        // Background clear colour is (0.18, 0.20, 0.24) -> (46, 51, 61).
        const std::size_t model = stats(img, [](const std::uint8_t *p) {
            return std::abs(p[0] - 46) + std::abs(p[1] - 51) + std::abs(p[2] - 61) > 30;
        });
        const double ratio = static_cast<double>(model) / (img.width * img.height);
        std::printf("    %ls: %ux%u model pixels %.1f%%\n", name, img.width, img.height, ratio * 100.0);
        CHECK(ratio > 0.05 && ratio < 0.9);
    }
    {
        const Image img = decodeImageFile(dir / L"transparent_motion.png");
        const std::size_t opaque = stats(img, [](const std::uint8_t *p) { return p[3] == 255; });
        const std::size_t clear = stats(img, [](const std::uint8_t *p) { return p[3] == 0; });
        const std::size_t partial = img.width * img.height - opaque - clear;
        const double total = img.width * img.height;
        std::printf("    transparent_motion.png: opaque %.1f%% clear %.1f%% partial %.1f%%\n", opaque / total * 100,
                    clear / total * 100, partial / total * 100);
        CHECK(opaque / total > 0.05);     // the model is drawn
        CHECK(clear / total > 0.3);       // the background really is transparent
        CHECK(img.rgba[3] == 0);          // top-left corner transparent
        CHECK(partial > 0);               // anti-aliased edges / translucent halo
    }
    return 0;
}

void testDesktopGeometry() {
    const auto afterOneSecond = [](int fps) {
        DesktopWalk walk{{-200.0, 50.0}, {400.0, 50.0}, 180.0};
        for (int i = 0; i < fps; ++i)
            CHECK(!walk.advance(1.0 / fps));
        return walk.position.x;
    };
    CHECK(std::abs(afterOneSecond(30) - afterOneSecond(120)) < 0.0001);
    DesktopWalk finalStep{{-1.0, 50.0}, {0.0, 50.0}, 180.0};
    CHECK(finalStep.advance(0.1) && finalStep.position.x == 0.0);

    const RECT secondary{-1600, -100, 0, 900};
    const SIZE size{384, 682};
    const POINT edge = clampToWorkArea({100, 1000}, size, secondary);
    CHECK(edge.x == -384 && edge.y == 218);
    const RECT target{-1200, 450, -400, 800};
    CHECK(seatPoint(target, SeatPosition::left, 40).x == -1160);
    CHECK(seatPoint(target, SeatPosition::center, 40).x == -800);
    CHECK(seatPoint(target, SeatPosition::right, 60).x == -461); // 150% DPI inset
    const POINT origin = seatOrigin(seatPoint(target, SeatPosition::center, 40), {192, 320}, size, secondary);
    CHECK(origin.x == -992 && origin.y == 130);
    const POINT maximized = seatOrigin({-800, -100}, {192, 320}, size, secondary);
    CHECK(maximized.y == secondary.top);
    Camera camera;
    const auto projected = camera.project({camera.target.x, camera.target.y, camera.target.z}, 384.0F, 682.0F);
    CHECK(std::abs(projected.x - 192.0F) < 0.01F && std::abs(projected.y - 341.0F) < 0.01F);
}

void testHitRegions(const fs::path &assets) {
    MmdModel model;
    model.loadModel(assets / L"テスト用モデル/テストキャラ.pmx");
    model.loadMotion(assets / L"モーション/待機テスト.vmd");
    const auto head = model.findBone("頭");
    const auto chest = model.findBone("上半身");
    CHECK(head && chest);
    if (!head || !chest)
        return;

    constexpr float width = 480.0F;
    constexpr float height = 720.0F;
    Camera camera;
    camera.fitToBounds(model.bindPoseBounds(), width / height);
    const auto rayAt = [&](const mmd::Float3 &p) {
        const auto client = camera.project(p, width, height);
        return camera.ray(client.x, client.y, width, height);
    };
    InteractionProfile profile;
    profile.spheres = {
        {"Body", "頭", {0, 0, -0.4F}, 0.2F, 0},
        {"Head", "頭", {}, 0.1F, 5},
        {"Bust", "上半身", {}, 0.5F, 5},
        {"Missing", "無いボーン", {}, 2.0F, 10},
    };
    HitRegions hits;
    hits.bind(profile, model);
    model.evaluate(0.0F);
    CHECK(hits.pick(rayAt(model.bonePosition(*head, true)), model) == "Head");
    CHECK(hits.pick(rayAt(model.bonePosition(*chest, true)), model) == "Bust");
    CHECK(hits.pick(camera.ray(0, 0, width, height), model).empty());

    profile.spheres = {{"Offset", "頭", {1.0F, 0, 0}, 0.045F, 0}};
    hits.bind(profile, model);
    const auto old = model.transformBoneOffset(*head, {1.0F, 0, 0});
    CHECK(hits.pick(rayAt(old), model) == "Offset");
    bool moved = false;
    for (float frame = 15.0F; frame <= model.motionLastFrame(); frame += 15.0F) {
        model.evaluate(frame);
        const auto current = model.transformBoneOffset(*head, {1.0F, 0, 0});
        if (hits.pick(rayAt(old), model).empty()) {
            CHECK(hits.pick(rayAt(current), model) == "Offset");
            moved = true;
            break;
        }
    }
    CHECK(moved);
}

PointerSample pointer(float x, std::string region = "Head", bool overModel = true) {
    return {static_cast<long>(x), 10, x, 10, std::move(region), 1.0F, overModel};
}

void testInteractionInput() {
    using Kind = MouseEventKind;
    std::vector<Kind> events;
    InteractionInput input({50, 4, 300, 2, 500}, false, 500,
                           [&](Kind kind, const PointerSample &) { events.push_back(kind); });
    const auto head = pointer(0);
    input.press(head, 0, false);
    input.release(head, 30, false);
    input.tick(499);
    CHECK(events.empty());
    input.tick(500);
    CHECK(events == std::vector{Kind::click});
    events.clear();

    input.press(head, 600, false);
    input.release(head, 620, false);
    input.press(head, 700, true);
    input.release(head, 720, false);
    input.tick(1300);
    CHECK(events == std::vector{Kind::doubleClick});
    events.clear();

    const auto transparent = pointer(10, "", false);
    input.press(transparent, 1400, false);
    input.release(transparent, 1420, false);
    input.tick(2000);
    CHECK(events.empty());

    const auto stroke = [&](std::uint64_t start, bool buttonsDown) {
        input.move(pointer(0), start, buttonsDown);
        input.move(pointer(20), start + 30, buttonsDown);
        input.move(pointer(0), start + 60, buttonsDown);
        input.move(pointer(20), start + 90, buttonsDown);
    };
    input.move(pointer(0), 2200, false);
    input.move(pointer(20), 2220, false);
    input.move(pointer(40), 2240, false);
    input.move(pointer(60), 2260, false);
    input.move(pointer(0, "Face"), 2280, false);
    CHECK(events.empty()); // one direction and a region change are not a stroke
    stroke(2350, false);
    CHECK(events == std::vector{Kind::pet});
    stroke(2500, false);
    CHECK(events.size() == 1);
    stroke(3000, true);
    CHECK(events.size() == 1);
    stroke(3200, false);
    CHECK(events.size() == 2 && events.back() == Kind::pet);
    events.clear();

    input.press(head, 4000, false);
    input.startDrag(4180);
    stroke(4200, false);
    input.endDrag(pointer(20, "Body"));
    input.tick(4700);
    CHECK((events == std::vector{Kind::dragStart, Kind::dragEnd}));
    events.clear();
    input.press(head, 4800, false);
    input.release(head, 5000, true);
    input.tick(5400);
    CHECK(events.empty()); // stationary long press also suppresses clicks

    for (std::uint64_t i = 0; i < 5000; ++i)
        stroke(6000 + i * 700, false);
    CHECK(events.size() == 5000);

    events.clear();
    InteractionInput hover({}, true, 500, [&](Kind kind, const PointerSample &) { events.push_back(kind); });
    hover.move(pointer(10), 0, false);
    hover.move(pointer(20), 30, false);
    hover.move(pointer(20), 100, false); // no movement in screen coordinates
    hover.move(pointer(30), 110, false);
    CHECK((events == std::vector{Kind::move, Kind::move}));
}

LRESULT CALLBACK sstpTestWindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == WM_COPYDATA) {
        const auto &data = *reinterpret_cast<const COPYDATASTRUCT *>(lParam);
        auto &received = *reinterpret_cast<std::string *>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (data.dwData != 9801 || data.cbData == 0)
            return FALSE;
        received.assign(static_cast<const char *>(data.lpData), data.cbData);
        return TRUE;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

void testSstpMouseEvents() {
    auto request = sstpMouseRequest(MouseEventKind::pet, pointer(50, "頭"));
    CHECK(request.starts_with("SEND SSTP/1.4\r\nCharset: UTF-8\r\n"));
    CHECK(request.find("Event: OnUk3dPet\r\n") != std::string::npos);
    CHECK(request.find("Reference0: 50\r\n") != std::string::npos);
    CHECK(request.find("Reference4: 頭\r\n") != std::string::npos);
    CHECK(request.ends_with("Reference6: mouse\r\n\r\n"));

    const std::string records =
        "other.hwnd\1" "456\r\nother.ghostpath\1" "C:\\SSP\\ghost\\Other\\\r\n"
        "sample.hwnd\1" "123\r\nsample.ghostpath\1" "C:\\SSP\\ghost\\Uka\\\r\n";
    CHECK(sstpGhostWindow(records, L"c:/ssp/ghost/uka") == reinterpret_cast<HWND>(std::uintptr_t{123}));
    CHECK(sstpGhostWindow(records, L"C:/SSP/ghost/Unknown") == nullptr);
    CHECK(sstpGhostWindow(records + "again.ghostpath\1" "C:/SSP/ghost/Uka\r\n", L"C:/SSP/ghost/Uka") == nullptr);

    constexpr wchar_t className[] = L"Uka3dTestSstp";
    const HINSTANCE instance = GetModuleHandleW(nullptr);
    WNDCLASSW windowClass{};
    windowClass.hInstance = instance;
    windowClass.lpfnWndProc = sstpTestWindowProc;
    windowClass.lpszClassName = className;
    CHECK(RegisterClassW(&windowClass) != 0);
    const HWND window = CreateWindowExW(0, className, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, instance, nullptr);
    CHECK(window != nullptr);
    if (window) {
        std::string received;
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&received));
        CHECK(sendDirectSstp(window, window, request));
        CHECK(received.size() == request.size() + 1 && received.back() == '\0');
        CHECK(std::string_view(received).substr(0, request.size()) == request);
        DestroyWindow(window);
    }
    UnregisterClassW(className, instance);
}

void run(const char *name, const std::function<void()> &test) {
    std::printf("[%s]\n", name);
    const int before = g_failures;
    try {
        test();
    } catch (const std::exception &e) {
        ++g_failures;
        std::printf("  FAILED: unexpected exception: %s\n", e.what());
    }
    std::printf("  %s\n", g_failures == before ? "ok" : "NG");
}

} // namespace

int wmain(int argc, wchar_t **argv) {
    SetConsoleOutputCP(CP_UTF8);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (argc >= 3 && std::wstring_view(argv[1]) == L"--verify-captures") {
        run("verify_captures", [&] { verifyCaptures(argv[2]); });
    } else if (argc >= 2) {
        const fs::path assets = argv[1];
        run("command_line", testCommandLine);
        run("utf", testUtf);
        run("camera", testCamera);
        run("desktop_geometry", testDesktopGeometry);
        run("hit_regions", [&] { testHitRegions(assets); });
        run("interaction_input", testInteractionInput);
        run("sstp_mouse_events", testSstpMouseEvents);
        run("images", [&] { testImages(assets); });
        run("model", [&] { testModel(assets); });
        run("fps_options", testFpsOptions);
        run("playback", testPlayback);
        run("frame_pacer", testFramePacer);
        run("physics", [&] { testPhysics(assets); });
    } else {
        std::printf("usage: uka3d_tests <assets-dir> | --verify-captures <dir>\n");
        return 2;
    }
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    CoUninitialize();
    return g_failures == 0 ? 0 : 1;
}
