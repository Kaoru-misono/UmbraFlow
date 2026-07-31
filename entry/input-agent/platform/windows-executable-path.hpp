#pragma once

#include <core/error/result.hpp>

#include <filesystem>

namespace uf::input_agent::platform
{
    // The directory the running executable was loaded from.
    //
    // It is what the agent resolves its shipped payload against, so a model that
    // travels beside the binary is found wherever the binary is launched from
    // and whatever working directory the launcher happened to have. An
    // annotation session is started detached by another program, so the working
    // directory is not something this process may depend on.
    //
    // A failure here is the process being unable to name its own image, which is
    // not the same condition as a payload being absent: the payload is resolved
    // lazily and reported on a results line, while this is reported once at
    // startup.
    [[nodiscard]] auto executableDirectory() -> Result<std::filesystem::path>;
}
