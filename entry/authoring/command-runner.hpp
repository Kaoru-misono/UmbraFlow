#pragma once

#include "command.hpp"

#include <core/error/result.hpp>

#include <string>

namespace uf::authoring
{
    // Executes one parsed command and returns the complete JSON document the
    // process writes to stdout.
    //
    // Every mutation is a load, one edit, and a full republish through
    // workbench::saveAndGenerateAuthoringProject. The process holds no session
    // between invocations, so nothing an agent authors can bypass
    // AuthoringDocument's validation, and no command can leave the generated
    // runtime closure describing a document that is no longer on disk.
    [[nodiscard]]
    auto runAuthoringCommand(
        AuthoringCommand const& command
    ) -> Result<std::string>;

    // The JSON document stdout carries when a command fails, so a caller parses
    // one shape whatever happened. The rendered prose goes to stderr separately,
    // through the product CLI's formatRunError.
    [[nodiscard]] auto authoringErrorJson(Error const& error) -> std::string;
}
