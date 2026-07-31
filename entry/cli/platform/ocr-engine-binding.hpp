#pragma once

#include <core/error/result.hpp>

#include <ocr/engine.hpp>

#include <filesystem>
#include <memory>
#include <optional>

namespace uf::cli::platform
{
    // Builds the OCR engine cycle_read runs on, from `--ocr-models`, or returns
    // null when the operator did not pass that flag.
    //
    // Shared by `run` and `drive` for the same reason target-binding.hpp is:
    // the two front-ends must not be able to run cycle_read under different
    // guarantees. A directory that WAS supplied but fails to build an engine is
    // a Result failure, never a silent null -- the operator asked for text
    // reads and must learn at startup that they will not work, not discover it
    // the first time cycle_read refuses mid-run.
    [[nodiscard]]
    auto bindOcrEngine(
        std::optional<std::filesystem::path> const& modelDirectory
    ) -> Result<std::unique_ptr<ocr::IOcrEngine>>;
}
