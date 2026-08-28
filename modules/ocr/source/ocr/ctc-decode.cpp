#include "ctc-decode.hpp"

#include <core/error/contracts.hpp>
#include <core/error/result.hpp>
#include <core/numeric/checked-arithmetic.hpp>
#include <core/numeric/checked-cast.hpp>
#include <core/types/integer.hpp>

#include <domain/error.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <ranges>
#include <span>
#include <string>
#include <utility>

namespace uf::ocr::detail
{
    namespace
    {
        constexpr auto k_basisPointScale = double{10000.0};
        constexpr auto k_ctcBlankClass = std::size_t{0};

        [[nodiscard]]
        auto basisPoints(double probability) -> uint32
        {
            // Only probabilities already checked in [0, 1], or their mean,
            // reach this conversion. Rounding therefore stays in [0, 10000].
            auto const rounded = checkedCast<uint32>(std::lround(probability * k_basisPointScale));
            UF_CHECK(rounded.has_value());
            return *rounded;
        }
    }

    auto decodeCtc(
        std::span<float const> scores,
        std::size_t timesteps,
        std::size_t classes,
        std::span<std::string const> dictionary
    ) -> Result<DecodedLine>
    {
        auto const expected = checkedMultiply(timesteps, classes);
        if (!expected || scores.size() != *expected || classes <= dictionary.size())
        {
            return fail(
                AutomationErrorKind::ExternalFailure,
                "recognition scores do not match the CTC tensor or dictionary dimensions"
            );
        }
        auto decoded    = DecodedLine{};
        auto confidence = double{0.0};
        auto emitted    = std::size_t{0};
        auto previous   = classes;
        for (auto step = std::size_t{0}; step < timesteps; ++step)
        {
            auto const row = scores.subspan(step * classes, classes);
            auto const bestIndex = checkedCast<std::size_t>(
                std::ranges::distance(row.begin(), std::ranges::max_element(row))
            );
            UF_CHECK(bestIndex.has_value());
            auto const best = *bestIndex;
            if (best != k_ctcBlankClass && best != previous)
            {
                auto const probability = static_cast<double>(row[best]);
                if (!std::isfinite(probability) || probability < 0 || probability > 1)
                {
                    return fail(
                        AutomationErrorKind::ExternalFailure,
                        "recognition model emitted a CTC score outside probability bounds"
                    );
                }
                auto const entry = best - 1U;
                auto character = TextCharacter{
                    .text = entry < dictionary.size() ? dictionary[entry] : std::string{" "},
                    .confidenceBp = basisPoints(probability),
                };
                decoded.text += character.text;
                decoded.characters.emplace_back(std::move(character));
                confidence += probability;
                ++emitted;
            }
            previous = best;
        }
        if (emitted != 0U)
        {
            auto const mean = confidence / static_cast<double>(emitted);
            decoded.confidenceBp = basisPoints(mean);
        }
        return decoded;
    }
}
