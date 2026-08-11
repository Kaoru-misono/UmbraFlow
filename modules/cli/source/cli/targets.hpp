#pragma once

#include <core/error/result.hpp>
#include <core/types/integer.hpp>

#include <span>
#include <string>
#include <vector>

namespace uf::cli
{
    // One window an operator could name, in the vocabulary this subcommand
    // prints and --hwnd consumes. It restates the controller's TargetCandidate
    // in plain types deliberately: this declaration is read on every host, and
    // the module owning the strong types builds on Windows alone.
    struct TargetListing final
    {
        intptr      handle{};
        std::string windowClass{};
        std::string title{};

        uint32 clientWidth{};
        uint32 clientHeight{};
        uint32 dpi{};

        // Carried rather than filtered on: a window that is there but cannot be
        // captured has to read differently from one that is gone.
        bool isIconic{};
    };

    // Implemented per host: Windows enumerates the desktop, every other host
    // reports the path as unsupported, as `explore` does.
    [[nodiscard]] auto targetsProduct() -> Result<std::vector<TargetListing>>;

    // Separate from the enumeration so the shape an operator and a script both
    // read is testable on a host with no desktop to enumerate. The handle leads
    // and the title trails, because a title is the one field containing spaces.
    [[nodiscard]]
    auto formatTargetListings(std::span<TargetListing const> listings) -> std::string;
}
