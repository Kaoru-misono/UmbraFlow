#include <domain/ids.hpp>

#include <core/types/integer.hpp>

#include <doctest/doctest.h>

#include <limits>
#include <string>
#include <type_traits>
#include <unordered_map>

static_assert(!std::is_same_v<uf::EngineRunId, uf::TaskRunId>);
static_assert(!std::is_same_v<uf::SessionId, uf::FrameId>);
static_assert(!std::is_convertible_v<uf::FrameId, uf::uint64>);
static_assert(!std::is_convertible_v<uf::uint64, uf::FrameId>);

TEST_CASE("domain identifiers are distinct value types")
{
    auto const seven = uf::FrameId{uf::uint64{7}};
    auto const anotherSeven = uf::FrameId{uf::uint64{7}};
    auto const eight = uf::FrameId{uf::uint64{8}};

    CHECK(seven == anotherSeven);
    CHECK(seven != eight);

    using FrameMap = std::unordered_map<
        uf::FrameId,
        int,
        uf::StrongValueHash<uf::FrameId>
    >;
    auto frames = FrameMap{};
    frames.emplace(seven, 42);
    CHECK(frames.at(anotherSeven) == 42);
}

TEST_CASE("target generation increments without wrapping")
{
    auto const initial = uf::TargetGeneration{};
    auto const next = initial.next();

    REQUIRE(next.has_value());
    CHECK(*next != initial);
    CHECK(next->value() == uf::uint64{1});
    CHECK(initial < *next);
}

TEST_CASE("target generation overflow is an internal invariant error")
{
    auto const exhausted = uf::TargetGeneration::fromValue(
        std::numeric_limits<uf::uint64>::max()
    );
    auto const result = exhausted.next();

    REQUIRE_FALSE(result.has_value());
    auto const kind = uf::automationErrorKind(result.error());
    REQUIRE(kind.has_value());
    CHECK(kind == uf::AutomationErrorKind::InternalInvariant);
}

TEST_CASE("labels preserve their text and strong identity")
{
    auto const label = uf::Label::create("home-marker.png");
    auto const equalLabel = uf::Label::create("home-marker.png");
    REQUIRE(label.has_value());
    REQUIRE(equalLabel.has_value());

    CHECK(uf::toString(*label) == "home-marker.png");

    using LabelMap = std::unordered_map<
        uf::Label,
        int,
        uf::StrongValueHash<uf::Label>
    >;
    auto labels = LabelMap{};
    labels.emplace(*label, 1);
    CHECK(labels.at(*equalLabel) == 1);
}

TEST_CASE("labels accept valid UTF-8 and reject invalid bytes")
{
    auto const invalid = uf::Label::create(
        std::string{1, static_cast<char>(0xFF)}
    );
    REQUIRE_FALSE(invalid.has_value());
    auto const kind = uf::automationErrorKind(invalid.error());
    REQUIRE(kind.has_value());
    CHECK(kind == uf::AutomationErrorKind::InternalInvariant);

    auto const validText = std::string{"caf\xC3\xA9"};
    auto const valid = uf::Label::create(validText);
    REQUIRE(valid.has_value());
    CHECK(uf::toString(*valid) == validText);
}
