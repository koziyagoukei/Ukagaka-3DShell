#pragma once

#include "app/CommandLine.h"

namespace uka3d {

enum ExitCode : int {
    exitOk = 0,
    exitUsage = 1,
    exitLoadError = 2,
    exitGraphicsError = 3,
    exitUnexpected = 4,
    exitIpcError = 5,
};

struct RunEnvironment {
    // true when no terminal is attached (e.g. launched from Explorer):
    // errors are additionally shown in a message box.
    bool showErrorDialogs{false};
};

int runApp(const Options &options, const RunEnvironment &environment);

} // namespace uka3d
