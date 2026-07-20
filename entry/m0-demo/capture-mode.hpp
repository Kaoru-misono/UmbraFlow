#pragma once

#include "args.hpp"

#include <core/error/result.hpp>

#include <span>
#include <string>

namespace uf::m0_demo
{
    [[nodiscard]]
    auto validateCaptureOutputPaths(CaptureArgs const& args) -> Status;

    [[nodiscard]]
    auto runCapture(
        std::span<std::string const> raw
    ) -> Status;
}
