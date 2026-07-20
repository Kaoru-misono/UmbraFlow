#pragma once

#include <controller/capture.hpp>
#include <core/error/result.hpp>
#include <domain/frame.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <vector>

namespace uf::m0_demo
{
    [[nodiscard]]
    auto indexedOutputPath(
        std::filesystem::path const& output,
        std::uint32_t index,
        std::uint32_t frameCount
    ) -> std::filesystem::path;

    [[nodiscard]]
    auto writeFramePng(
        Frame const& frame,
        std::filesystem::path const& output
    ) -> Status;

    [[nodiscard]]
    auto encodeFramePng(
        Frame const& frame,
        std::filesystem::path const& output
    ) -> Result<std::vector<std::byte>>;

    [[nodiscard]]
    auto captureFramePng(
        WgcCaptureSession& session,
        std::filesystem::path const& output
    ) -> Result<Frame>;
}
