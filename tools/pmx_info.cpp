// Prints a physics-oriented summary of a PMX model (rigid bodies, joints).
// Development aid for diagnosing physics behaviour; prints only, writes nothing.
//   pmx_info <model.pmx> [name-filter | --morphs]
#include <mmd/pmx.hpp>

#include <windows.h>

#include <cmath>
#include <cstdio>
#include <map>
#include <string>

namespace {
constexpr float kRadToDeg = 57.29578F;
}

int wmain(int argc, wchar_t **argv) {
    SetConsoleOutputCP(CP_UTF8);
    if (argc < 2) {
        std::printf("usage: pmx_info <model.pmx> [name-filter]\n");
        return 1;
    }
    std::string filter;
    if (argc >= 3) {
        const int n = WideCharToMultiByte(CP_UTF8, 0, argv[2], -1, nullptr, 0, nullptr, nullptr);
        filter.resize(static_cast<std::size_t>(n > 0 ? n - 1 : 0));
        WideCharToMultiByte(CP_UTF8, 0, argv[2], -1, filter.data(), n, nullptr, nullptr);
    }
    try {
        const auto model = mmd::pmx::load(argv[1]);
        std::printf("model: %s  bones=%zu bodies=%zu joints=%zu\n", model.metadata.modelName.c_str(),
                    model.bones.size(), model.rigidBodies.size(), model.joints.size());
        if (filter == "--morphs") {
            for (std::size_t i = 0; i < model.morphs.size(); ++i)
                std::printf("morph %3zu panel=%u type=%u %s\n", i, model.morphs[i].panel, model.morphs[i].type,
                            model.morphs[i].name.c_str());
            return 0;
        }
        std::map<int, int> modes;
        for (const auto &body : model.rigidBodies)
            ++modes[body.mode];
        for (const auto &[mode, count] : modes)
            std::printf("  mode %d: %d bodies\n", mode, count);

        for (std::size_t i = 0; i < model.rigidBodies.size(); ++i) {
            const auto &b = model.rigidBodies[i];
            if (!filter.empty() && b.name.find(filter) == std::string::npos)
                continue;
            const char *bone = b.bone >= 0 && static_cast<std::size_t>(b.bone) < model.bones.size()
                                   ? model.bones[static_cast<std::size_t>(b.bone)].name.c_str()
                                   : "-";
            std::printf("body %3zu %-16s bone=%-14s mode=%u shape=%u size=(%.2f %.2f %.2f) mass=%.2f damp=(%.2f %.2f) "
                        "group=%u mask=0x%04X pos=(%.2f %.2f %.2f)\n",
                        i, b.name.c_str(), bone, b.mode, b.shape, b.size[0], b.size[1], b.size[2], b.mass,
                        b.linearDamping, b.angularDamping, b.group, b.collisionMask, b.position[0], b.position[1],
                        b.position[2]);
        }
        for (std::size_t i = 0; i < model.joints.size(); ++i) {
            const auto &j = model.joints[i];
            const auto bodyName = [&](int index) {
                return index >= 0 && static_cast<std::size_t>(index) < model.rigidBodies.size()
                           ? model.rigidBodies[static_cast<std::size_t>(index)].name
                           : std::string("-");
            };
            const std::string a = bodyName(j.bodyA);
            const std::string b = bodyName(j.bodyB);
            if (!filter.empty() && j.name.find(filter) == std::string::npos && a.find(filter) == std::string::npos &&
                b.find(filter) == std::string::npos)
                continue;
            std::printf("joint %3zu %-16s %s <-> %s rot=[%.0f..%.0f, %.0f..%.0f, %.0f..%.0f]deg "
                        "spring(rot)=(%.1f %.1f %.1f)\n",
                        i, j.name.c_str(), a.c_str(), b.c_str(), j.rotationMinimum[0] * kRadToDeg,
                        j.rotationMaximum[0] * kRadToDeg, j.rotationMinimum[1] * kRadToDeg,
                        j.rotationMaximum[1] * kRadToDeg, j.rotationMinimum[2] * kRadToDeg,
                        j.rotationMaximum[2] * kRadToDeg, j.rotationSpring[0], j.rotationSpring[1],
                        j.rotationSpring[2]);
        }
    } catch (const std::exception &e) {
        std::printf("error: %s\n", e.what());
        return 2;
    }
    return 0;
}
