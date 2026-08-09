#pragma once

#include <core/error/result.hpp>

#include <ocr/engine.hpp>

#include <filesystem>
#include <memory>
#include <optional>

namespace uf::cli::platform
{
    // Builds the optional OCR engine used by privileged exploration. A supplied
    // directory must carry both recognition and detection models; incomplete
    // payloads fail before a target is bound.
    [[nodiscard]]
    auto bindOcrEngine(
        std::optional<std::filesystem::path> const& modelDirectory
    ) -> Result<std::unique_ptr<ocr::IOcrEngine>>;
}
