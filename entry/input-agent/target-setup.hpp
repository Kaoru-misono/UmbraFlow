#pragma once

#include <controller/capture.hpp>
#include <controller/discovery.hpp>
#include <controller/target.hpp>
#include <core/error/result.hpp>
#include <core/types/integer.hpp>

#include <optional>
#include <string>

namespace uf::input_agent
{
    // How a command line names one window. It sits beside buildSelector rather
    // than in either program's argument file because both spell the same four
    // flags and both hand the result to the same controller selector.
    struct SelectorArgs final
    {
        std::optional<uint32>      process{};
        std::optional<intptr>      windowHandle{};
        std::optional<std::string> windowClass{};
        std::optional<std::string> title{};

        auto operator==(SelectorArgs const&) const -> bool = default;
    };

    [[nodiscard]] auto ensureClientAreaUsable(ClientSize client) -> Status;

    [[nodiscard]] auto buildSelector(SelectorArgs const& selector) -> TargetSelector;

    [[nodiscard]]
    auto createCaptureSession(
        ResolvedTarget const& target,
        CaptureSessionId sessionId,
        WgcCaptureOptions options = {}
    ) -> Result<WgcCaptureSession>;

    // Whether a revalidated target is still the one an observation came from.
    // Every outcome other than Unchanged ends the caller's work, because the
    // window it was launched against is gone or was replaced.
    [[nodiscard]] auto requireUnchangedTarget(RevalidateOutcome outcome) -> Status;
}
