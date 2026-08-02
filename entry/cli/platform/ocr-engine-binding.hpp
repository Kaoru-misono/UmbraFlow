#pragma once

#include <core/error/result.hpp>

#include <ocr/engine.hpp>

#include <filesystem>
#include <memory>
#include <optional>

namespace uf::cli::platform
{
    // Builds the OCR engine the text reads run on, from `--ocr-models`, or returns
    // null when the operator did not pass that flag. The directory must carry both
    // the recognition model `cycle_read` needs and the detection model
    // `cycle_read_lines` needs; half a payload fails at startup rather than
    // surprising a run three minutes in. A directory that WAS supplied but fails to
    // build an engine is a Result failure, never a silent null.
    //
    // Shared by all four subcommands so the front-ends cannot run cycle_read under
    // different guarantees, which is why this declaration lives outside the
    // platform-gated block its Windows definition sits in. Implemented per host, as
    // runProduct is: elsewhere a supplied directory is refused by name and an absent
    // one still yields the null every subcommand handles, which keeps `check`
    // measuring template cells on every host.
    [[nodiscard]]
    auto bindOcrEngine(
        std::optional<std::filesystem::path> const& modelDirectory
    ) -> Result<std::unique_ptr<ocr::IOcrEngine>>;
}
