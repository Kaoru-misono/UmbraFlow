#include "targets.hpp"

#include <core/types/integer.hpp>

#include <format>
#include <span>
#include <string>

namespace uf::cli
{
    auto formatTargetListings(std::span<TargetListing const> listings) -> std::string
    {
        // Said rather than left blank. An empty desktop and a desktop whose
        // windows were all filtered out print the same nothing otherwise, and
        // the operator's next move differs.
        if (listings.empty())
        {
            return "no visible window on this desktop\n";
        }

        auto text = std::string{};
        for (auto const& listing : listings)
        {
            auto const handleText = std::format(
                "{:#x}",
                static_cast<uintptr>(listing.handle)
            );
            text += std::format(
                "{:<18}  {:<20}  {:>5}x{:<5}  {:>4} dpi  {:<9}  {}\n",
                handleText,
                listing.windowClass,
                listing.clientWidth,
                listing.clientHeight,
                listing.dpi,
                listing.isIconic ? "minimized" : "",
                listing.title
            );
        }
        return text;
    }
}
