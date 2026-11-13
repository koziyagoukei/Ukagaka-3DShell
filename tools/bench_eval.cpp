// Headless CPU profile of one simulation tick (no window, no GPU).
//   uka3d_bench <model.pmx> [motion.vmd] [--ticks N]
//
// Breaks the per-tick cost into the stages the runtime executes:
//   animation + IK + morphs, physics (Bullet), CPU skinning inside libmmd
//   (including its per-tick vertex array copy), and the renderer-side copies.
// Each scenario replays the same motion with a fixed 1/60 s step.
#include "mmd/MmdModel.h"
#include "mmd/Skinning.h"

#include <mmd/animation.hpp>
#include <mmd/physics.hpp>

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <functional>
#include <numeric>
#include <string>
#include <vector>

namespace {

double nowMs() {
    static LARGE_INTEGER frequency = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return f;
    }();
    LARGE_INTEGER counter;
    QueryPerformanceCounter(&counter);
    return static_cast<double>(counter.QuadPart) * 1000.0 / static_cast<double>(frequency.QuadPart);
}

struct Stats {
    double mean{}, median{}, p95{}, max{};
};

Stats summarize(std::vector<double> samples) {
    Stats s;
    if (samples.empty())
        return s;
    std::sort(samples.begin(), samples.end());
    s.mean = std::accumulate(samples.begin(), samples.end(), 0.0) / static_cast<double>(samples.size());
    s.median = samples[samples.size() / 2];
    s.p95 = samples[std::min(samples.size() - 1, samples.size() * 95 / 100)];
    s.max = samples.back();
    return s;
}

void print(const char *name, const Stats &s) {
    std::printf("%-46s mean %6.3f  median %6.3f  p95 %6.3f  max %6.3f ms\n", name, s.mean, s.median, s.p95, s.max);
}

struct DynamicVertex {
    float position[3];
    float normal[3];
};

} // namespace

int wmain(int argc, wchar_t **argv) {
    SetConsoleOutputCP(CP_UTF8);
    if (argc < 2) {
        std::printf("usage: uka3d_bench <model.pmx> [motion.vmd] [--ticks N]\n");
        return 2;
    }
    std::filesystem::path modelPath = argv[1];
    std::filesystem::path motionPath;
    int ticks = 600;
    for (int i = 2; i < argc; ++i) {
        if (std::wstring_view(argv[i]) == L"--ticks" && i + 1 < argc)
            ticks = std::max(60, _wtoi(argv[++i]));
        else
            motionPath = argv[i];
    }
    // Keep timings stable: one core, above-normal priority.
    SetThreadAffinityMask(GetCurrentThread(), 1);
    SetPriorityClass(GetCurrentProcess(), ABOVE_NORMAL_PRIORITY_CLASS);

    try {
        const mmd::PmxModel model = mmd::pmx::load(modelPath);
        mmd::VmdMotion motion;
        const bool hasMotion = !motionPath.empty();
        if (hasMotion)
            motion = mmd::loadVmd(motionPath);
        const float lastFrame = hasMotion ? static_cast<float>(motion.lastFrame) : 0.0F;

        std::size_t weightCounts[5]{};
        for (const auto &v : model.vertices)
            ++weightCounts[static_cast<std::size_t>(v.weightType)];
        std::size_t vertexMorphs = 0, vertexMorphOffsets = 0;
        for (const auto &m : model.morphs)
            if (m.type == 1) {
                ++vertexMorphs;
                vertexMorphOffsets += m.offsets.size();
            }
        std::printf("model: vertices=%zu bones=%zu bodies=%zu joints=%zu morphs=%zu (vertex %zu, offsets %zu)\n",
                    model.vertices.size(), model.bones.size(), model.rigidBodies.size(), model.joints.size(),
                    model.morphs.size(), vertexMorphs, vertexMorphOffsets);
        std::printf("weights: BDEF1=%zu BDEF2=%zu BDEF4=%zu SDEF=%zu QDEF=%zu | sizeof(PmxVertex)=%zu (%.1f MB/tick copy)\n",
                    weightCounts[0], weightCounts[1], weightCounts[2], weightCounts[3], weightCounts[4],
                    sizeof(mmd::PmxVertex), model.vertices.size() * sizeof(mmd::PmxVertex) / 1048576.0);
        std::printf("motion: %s lastFrame=%.0f, ticks=%d\n\n", hasMotion ? "yes" : "none", lastFrame, ticks);

        const auto makePhysics = [&] {
            mmd::PhysicsSettings settings;
            settings.compatibility = mmd::PhysicsCompatibilityProfile::mmd275Compatible();
            settings.fixedTimeStep = static_cast<float>(1.0 / 60.0) * 1.0001F;
            settings.collectContactDiagnostics = false;
            return std::make_unique<mmd::MmdPhysics>(model, settings);
        };

        // Runs `ticks` evaluations; the motion advances half a frame per tick and stops at its end
        // (no backward seek, which would reset physics inside libmmd).
        const auto run = [&](bool physicsOn, bool gpuSkinning, mmd::VertexOutput output,
                             const std::function<void(const mmd::AnimatedModelFrame &)> &after) {
            mmd::MmdAnimator animator(model);
            if (hasMotion)
                animator.setMotion(&motion);
            std::unique_ptr<mmd::MmdPhysics> physics;
            if (physicsOn && !model.rigidBodies.empty()) {
                physics = makePhysics();
                if (physics->available())
                    animator.setPhysics(physics.get());
            }
            mmd::AnimatedModelFrame frame;
            std::vector<double> samples;
            samples.reserve(static_cast<std::size_t>(ticks));
            float f = 0.0F;
            for (int i = 0; i < ticks + 30; ++i) {
                const double t0 = nowMs();
                animator.evaluateInto(frame, f, i == 0 ? 0.0F : 1.0F / 60.0F, gpuSkinning, {}, output);
                if (after)
                    after(frame);
                const double t1 = nowMs();
                if (i >= 30) // warm-up
                    samples.push_back(t1 - t0);
                if (lastFrame > 0.0F) {
                    f += 0.5F;
                    if (f > lastFrame)
                        f = lastFrame; // hold the last pose; physics keeps running
                }
            }
            return summarize(samples);
        };

        const Stats full = run(true, false, mmd::VertexOutput::full, {});
        const Stats noPhysics = run(false, false, mmd::VertexOutput::full, {});
        const Stats bonesPhysics = run(true, true, mmd::VertexOutput::omitUnchangedGpu, {});
        const Stats bonesOnly = run(false, true, mmd::VertexOutput::omitUnchangedGpu, {});
        print("A libmmd full tick (CPU skin + physics)", full);
        print("B libmmd CPU skin, physics OFF", noPhysics);
        print("C libmmd bones only + physics (no skinning)", bonesPhysics);
        print("D libmmd bones only, physics OFF (anim+IK+morph)", bonesOnly);
        std::printf("  => physics            ~ %.3f ms (C - D)\n", bonesPhysics.mean - bonesOnly.mean);
        std::printf("  => libmmd CPU skinning ~ %.3f ms (A - C), incl. %.1f MB vertex copy\n",
                    full.mean - bonesPhysics.mean, model.vertices.size() * sizeof(mmd::PmxVertex) / 1048576.0);

        // Renderer-side copies (ModelRenderer::setFrame + upload with interpolation).
        {
            mmd::MmdAnimator animator(model);
            mmd::AnimatedModelFrame frame;
            animator.evaluateInto(frame, 0.0F, 0.0F, false);
            std::vector<DynamicVertex> previous(model.vertices.size()), current(model.vertices.size()),
                gpu(model.vertices.size());
            std::vector<double> copy, lerp, plain;
            for (int i = 0; i < 300; ++i) {
                double t0 = nowMs();
                std::swap(previous, current);
                for (std::size_t v = 0; v < frame.vertices.size(); ++v) {
                    std::memcpy(current[v].position, frame.vertices[v].position.data(), 12);
                    std::memcpy(current[v].normal, frame.vertices[v].normal.data(), 12);
                }
                double t1 = nowMs();
                const float a = 0.5F, b = 0.5F;
                for (std::size_t v = 0; v < current.size(); ++v)
                    for (int c = 0; c < 3; ++c) {
                        gpu[v].position[c] = previous[v].position[c] * b + current[v].position[c] * a;
                        gpu[v].normal[c] = previous[v].normal[c] * b + current[v].normal[c] * a;
                    }
                double t2 = nowMs();
                std::memcpy(gpu.data(), current.data(), current.size() * sizeof(DynamicVertex));
                double t3 = nowMs();
                copy.push_back(t1 - t0);
                lerp.push_back(t2 - t1);
                plain.push_back(t3 - t2);
            }
            print("E renderer setFrame copy (per tick)", summarize(copy));
            print("F renderer upload with interpolation (per frame)", summarize(lerp));
            print("G renderer upload memcpy (alpha = 1)", summarize(plain));
        }

        // Optimised skinner (src/mmd/Skinning): bones from libmmd, vertices skinned here.
        {
            uka3d::Skinner skinner(model);
            std::vector<uka3d::SkinnedVertex> out(model.vertices.size());
            const Stats fast = run(true, true, mmd::VertexOutput::omitUnchangedGpu,
                                   [&](const mmd::AnimatedModelFrame &frame) { skinner.skin(frame, out); });
            print("H bones + physics + uka3d::Skinner", fast);
            std::printf("  => uka3d::Skinner     ~ %.3f ms (H - C)\n", fast.mean - bonesPhysics.mean);
            std::printf("  => tick speed-up      %.2fx (A / H)\n", full.mean / fast.mean);
        }
    } catch (const std::exception &e) {
        std::printf("error: %s\n", e.what());
        return 1;
    }
    return 0;
}
