// uka3d_motiongen - writes idle / walk / sit / grab VMDs fitted to a PMX model
// and checks them (foot sliding, mesh below the floor).
//
//   uka3d_motiongen <model.pmx> <outDir> [--speed px/s] [--width px] [--height px] [--fit height|all]
//                   [--step ratio] [--lift ratio] [--start-crossing] [--check] [--compare <dir>]
//
// --step is the step length / leg length (default 0.5); --lift the swing foot's
// clearance / leg length (default 0.055). Long skirts and robes stay down with
// small steps and a low lift, e.g. --speed 110 --step 0.28 --lift 0.03 --start-crossing
// (--start-crossing begins the loop with the legs passing each other, so the
// step out of standing does not throw the skirt).
//
// --speed/--width/--height describe the 3D window at 96 dpi (defaults: the
// sample ghost, 180 px/s in a 384x682 window). The walk cycle is timed so the
// planted foot moves exactly as fast as the window walks: no sliding.
// Exit code 2: the model has no usable legs (keep the other motions).
#include "mmd/MmdModel.h"
#include "motion/MotionGenerator.h"
#include "renderer/Camera.h"

#include <mmd/pmx.hpp>
#include <mmd/vmd.hpp>

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <limits>
#include <string>

namespace fs = std::filesystem;
using namespace uka3d;

namespace {

std::string utf8(const wchar_t *text) {
    const int n = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<std::size_t>(std::max(0, n - 1)), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text, -1, out.data(), n, nullptr, nullptr);
    return out;
}

// Pixels on screen per model unit of forward movement while walking, as the
// runtime draws it (camera fitted to the bind pose, turned 65 degrees).
float pixelsPerUnit(const mmd::PmxModel &model, float width, float height, bool fitHeight) {
    Camera camera;
    camera.fitToBounds(MmdModel::computeBounds(model.vertices),width / height, fitHeight ? 1.02F : 1.08F, fitHeight);
    camera.yaw += DirectX::XMConvertToRadians(65.0F);
    const auto a = camera.project({0.0F, 0.0F, 0.0F}, width, height);
    const auto b = camera.project({0.0F, 0.0F, -1.0F}, width, height);
    return std::hypot(b.x - a.x, b.y - a.y);
}

void printWalk(const char *label, const motion::ContactReport &r, float speed, float legLength) {
    std::printf("%s walk: planted foot slide mean %.2f%% / max %.1f%% (frame %.2f) of walking speed, %d samples; "
                "mesh below floor %.3f units (%.1f%% of leg)\n",
                label, 100.0F * r.slideMean / speed, 100.0F * r.slideMax / speed, r.worstFrame, r.samples, r.penetration,
                100.0F * r.penetration / legLength);
}

void printDeepest(const mmd::PmxModel &model, const motion::ContactReport &r) {
    if (r.penetration <= 0.0F || r.deepestVertex >= model.vertices.size())
        return;
    const auto &v = model.vertices[r.deepestVertex];
    std::printf("  deepest: vertex %zu at frame %.2f, bind (%.2f, %.2f, %.2f), bones", r.deepestVertex, r.deepestFrame,
                v.position[0], v.position[1], v.position[2]);
    for (int i = 0; i < 4; ++i)
        if (v.bones[i] >= 0 && v.weights[i] > 0.0F)
            std::printf(" %s:%.2f", model.bones[static_cast<std::size_t>(v.bones[i])].name.c_str(), v.weights[i]);
    std::printf("\n");
}

void printIdle(const char *label, const motion::ContactReport &r) {
    std::printf("%s idle: foot drift %.3f units, mesh below floor %.3f units\n", label, r.drift, r.penetration);
}

} // namespace

int wmain(int argc, wchar_t **argv) {
    SetConsoleOutputCP(CP_UTF8);
    if (argc < 3) {
        std::fputs("usage: uka3d_motiongen <model.pmx> <outDir> [--speed px/s] [--width px] [--height px] "
                   "[--fit height|all] [--step ratio] [--lift ratio] [--start-crossing] [--check] [--compare <dir>]\n",
                   stderr);
        return 1;
    }
    const fs::path modelPath = argv[1];
    const fs::path outDir = argv[2];
    float speed = 180.0F, width = 384.0F, height = 682.0F, step = 0.5F, lift = 0.055F;
    bool fitHeight = true, doCheck = false, startCrossing = false;
    fs::path compareDir;
    for (int i = 3; i < argc; ++i) {
        const std::wstring arg = argv[i];
        const auto number = [&](float &out) {
            if (i + 1 < argc)
                out = std::stof(argv[++i]);
        };
        if (arg == L"--speed")
            number(speed);
        else if (arg == L"--step")
            number(step);
        else if (arg == L"--lift")
            number(lift);
        else if (arg == L"--width")
            number(width);
        else if (arg == L"--height")
            number(height);
        else if (arg == L"--fit" && i + 1 < argc)
            fitHeight = std::wstring(argv[++i]) != L"all";
        else if (arg == L"--start-crossing")
            startCrossing = true;
        else if (arg == L"--check")
            doCheck = true;
        else if (arg == L"--compare" && i + 1 < argc)
            compareDir = argv[++i];
    }

    try {
        const auto model = mmd::pmx::load(modelPath);
        const auto rig = motion::analyze(model);
        std::printf("%s\n", rig.describe().c_str());
        if (!rig.valid)
            return 2;

        // Time the walk so the feet keep pace with the window.
        const float ppu = pixelsPerUnit(model, width, height, fitHeight);
        const float unitsPerSecond = speed / std::max(1e-3F, ppu);
        auto params = motion::fitWalk(rig, unitsPerSecond, step);
        params.lift = lift;
        params.startCrossing = startCrossing;
        motion::WalkInfo info;
        const auto walk = motion::makeWalk(rig, params, info);
        std::printf("walk: %u frames (%.2f s), step %.2f x leg, stride %.2f units, %.2f units/s = %.0f px/s at 96 dpi "
                    "(window %.0f px/s, %.1f px per unit)\n",
                    params.frames, info.seconds, params.stepRatio, info.stride, info.speed, info.speed * ppu, speed, ppu);

        const auto idle = motion::makeIdle(rig);
        fs::create_directories(outDir);
        mmd::saveVmd(outDir / L"idle.vmd", idle);
        mmd::saveVmd(outDir / L"walk.vmd", walk);
        mmd::saveVmd(outDir / L"sit.vmd", motion::makeSit(rig));
        mmd::saveVmd(outDir / L"grab.vmd", motion::makeGrab(rig));
        std::printf("wrote idle.vmd walk.vmd sit.vmd grab.vmd to %s\n", utf8(outDir.c_str()).c_str());

        if (doCheck) {
            const auto walkReport = motion::measureContact(model, rig, walk, info.speed);
            printWalk("check", walkReport, info.speed, rig.legLength);
            printDeepest(model, walkReport);
            printIdle("check", motion::measureContact(model, rig, idle, 0.0F));
        }
        if (!compareDir.empty()) {
            // Other walks know nothing about the window: measure against its speed.
            printWalk("compare", motion::measureContact(model, rig, mmd::loadVmd(compareDir / L"walk.vmd"), unitsPerSecond),
                      unitsPerSecond, rig.legLength);
            printIdle("compare", motion::measureContact(model, rig, mmd::loadVmd(compareDir / L"idle.vmd"), 0.0F));
        }
    } catch (const std::exception &e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
    return 0;
}
