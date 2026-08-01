#pragma once

#include <core/error/result.hpp>

#include <ocr/engine.hpp>

#include <filesystem>
#include <memory>
#include <optional>

namespace uf::cli::platform
{
    // Builds the OCR engine the text reads run on, from `--ocr-models`, or
    // returns null when the operator did not pass that flag.
    //
    // "the text reads", plural, since 2026-08-01: the directory carries the
    // recognition model `cycle_read` needs AND the detection model
    // `cycle_read_lines` needs, and both are named unconditionally. Half a
    // payload is a failure at startup rather than a working single-line read and
    // a surprise three minutes into a run.
    //
    // Shared by all four subcommands for the same reason target-binding.hpp is
    // shared by three of them: the front-ends must not be able to run cycle_read
    // under different guarantees. `check` is the fourth and the newest -- the
    // falsification matrix measures a cell no template can answer by reading it
    // -- and it is why this declaration lives outside the platform-gated block
    // its Windows definition sits in. A directory that WAS supplied but fails to
    // build an engine is a Result failure, never a silent null: the operator
    // asked for text reads and must learn at startup that they will not work,
    // not discover it the first time cycle_read refuses mid-run.
    //
    // Implemented per host, exactly as runProduct is. The adapter builds only
    // where its inference runtime does, so elsewhere a supplied directory is
    // refused by name and an absent one still yields the null every subcommand
    // already handles -- which is what keeps `check` measuring template cells on
    // every host.
    [[nodiscard]]
    auto bindOcrEngine(
        std::optional<std::filesystem::path> const& modelDirectory
    ) -> Result<std::unique_ptr<ocr::IOcrEngine>>;
}
