#include <ocr/ctc-decode.hpp>
#include <ocr/text.hpp>

#include <core/safety/checked-access.hpp>
#include <core/types/integer.hpp>

#include <doctest/doctest.h>

#include <array>
#include <cstddef>
#include <span>
#include <string>
#include <vector>

namespace uf::ocr
{
    namespace
    {
        struct Timestep final
        {
            std::size_t selected{};
            float       probability{};
        };

        [[nodiscard]]
        auto scoresFor(std::span<Timestep const> path, std::size_t classes)
            -> std::vector<float>
        {
            auto scores = std::vector<float>(path.size() * classes, 0.0F);
            for (auto step = std::size_t{0}; step < path.size(); ++step)
            {
                REQUIRE(path[step].selected < classes);
                checkedAt(scores, step * classes + path[step].selected) = path[step].probability;
            }
            return scores;
        }
    }

    TEST_CASE("CTC character scores retain emitted Unicode tokens blank resets and spaces")
    {
        auto const dictionary = std::array{std::string{"甲"}, std::string{"𠮷"}, std::string{"é"}};
        auto const path = std::array{
            Timestep{0, 0.99F}, Timestep{1, 0.60F}, Timestep{1, 0.95F},
            Timestep{0, 0.98F}, Timestep{1, 0.70F}, Timestep{2, 0.80F},
            Timestep{4, 0.90F}, Timestep{4, 0.99F}, Timestep{0, 0.99F},
            Timestep{3, 0.75F},
        };
        auto const scores = scoresFor(path, 5);
        auto const decoded = detail::decodeCtc(scores, path.size(), 5, dictionary);
        REQUIRE(decoded.has_value());
        CHECK(decoded->text == "甲甲𠮷 é");
        CHECK(decoded->confidenceBp == 7500U);
        CHECK(
            decoded->characters == std::vector<TextCharacter>{
                {.text = "甲", .confidenceBp = 6000},
                {.text = "甲", .confidenceBp = 7000},
                {.text = "𠮷", .confidenceBp = 8000},
                {.text = " ", .confidenceBp = 9000},
                {.text = "é", .confidenceBp = 7500},
            }
        );
    }

    TEST_CASE("CTC line confidence averages raw emitted scores before rounding")
    {
        auto const dictionary = std::array{std::string{"a"}, std::string{"b"}, std::string{"c"}};
        auto const path = std::array{
            Timestep{1, 0.500049F}, Timestep{2, 0.500049F}, Timestep{3, 0.500099F},
        };
        auto const scores = scoresFor(path, 4);
        auto const decoded = detail::decodeCtc(scores, path.size(), 4, dictionary);
        REQUIRE(decoded.has_value());
        CHECK(decoded->text == "abc");
        CHECK(
            decoded->characters == std::vector<TextCharacter>{
                {.text = "a", .confidenceBp = 5000},
                {.text = "b", .confidenceBp = 5000},
                {.text = "c", .confidenceBp = 5001},
            }
        );
        // Averaging rounded character scores would produce 5000, not 5001.
        CHECK(decoded->confidenceBp == 5001U);
    }

    TEST_CASE("CTC empty and blank-only paths have no invented character")
    {
        auto const dictionary = std::array{std::string{"甲"}};
        auto const paths = std::array{
            std::vector<Timestep>{},
            std::vector<Timestep>{{0, 0.9F}, {0, 0.8F}},
        };
        for (auto const& path : paths)
        {
            auto const scores = scoresFor(path, 3);
            auto const decoded = detail::decodeCtc(scores, path.size(), 3, dictionary);
            REQUIRE(decoded.has_value());
            CHECK(decoded->text.empty());
            CHECK(decoded->characters.empty());
            CHECK(decoded->confidenceBp == 0U);
        }
    }

    TEST_CASE("CTC refuses inconsistent tensor views and invalid emitted probabilities")
    {
        auto const dictionary = std::array{std::string{"甲"}};
        auto const valid = std::array{0.1F, 0.8F, 0.1F};
        CHECK_FALSE(detail::decodeCtc(valid, 2, 3, dictionary).has_value());
        CHECK_FALSE(detail::decodeCtc(valid, 3, 1, dictionary).has_value());
        auto const invalid = std::array{0.1F, 1.2F, 0.1F};
        CHECK_FALSE(detail::decodeCtc(invalid, 1, 3, dictionary).has_value());
    }
}
