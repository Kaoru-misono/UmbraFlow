#pragma once

// The repository root, found from this file's own location and then from the
// working directory, so a test reading a checked-in document works from either.
//
// Five other test translation units carry a copy of this function
// (tests/core/test-json-text.cpp and tests/operator/test-*-contract.cpp). This
// header is one spelling for the files under tests/json/ rather than a sixth
// copy; folding the other five into it belongs to whoever owns them.

#include <filesystem>
#include <string_view>

namespace uf::json
{
    [[nodiscard]]
    inline auto repositoryRoot(std::string_view knownFile) -> std::filesystem::path
    {
        auto source = std::filesystem::path{__FILE__};
        if (source.is_relative())
        {
            source = std::filesystem::absolute(source);
        }

        auto candidate = source.parent_path().parent_path().parent_path();
        if (std::filesystem::is_regular_file(candidate / knownFile))
        {
            return candidate;
        }

        candidate = std::filesystem::current_path();
        while (!candidate.empty())
        {
            if (std::filesystem::is_regular_file(candidate / knownFile))
            {
                return candidate;
            }
            auto const parent = candidate.parent_path();
            if (parent == candidate)
            {
                break;
            }
            candidate = parent;
        }

        return {};
    }
}
