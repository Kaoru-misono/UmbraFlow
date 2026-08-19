#pragma once

#include <core/error/error.hpp>
#include <core/error/result.hpp>

#include <script/engine.hpp>

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

namespace uf::cli
{
    // Sized for a line that is a program rather than a handful of scalars: far
    // above any hand- or model-written chunk, far below a line that could only
    // be a mistake. (The retired operator protocol carried scalars and had a far
    // lower ceiling; this one never inherited it.)
    inline constexpr auto k_maxExploreLineBytes = std::size_t{256} * 1024U;

    // An id also labels the chunk in compile diagnostics and tracebacks, where a
    // paragraph would drown the message it was meant to locate.
    inline constexpr auto k_maxExploreIdBytes = std::size_t{128};

    // The exploration queue protocol: one JSON object per line, two required
    // members and one optional.
    //
    //   {"id":"step-3","chunk":"return explore.cycle(function(c) ... end)"}
    //   {"id":"step-4","chunk":"return 'done'","end":true}
    //
    // A line carries a chunk rather than one command because an agent's smallest
    // useful act is already a composition, and splitting it across lines would put
    // the framework's control flow in the queue file, where nothing can check it
    // (docs/archive/plans/2026-08-01-three-layers-and-agent-operator.md 3). The id is
    // required because a session answers every line, including one it refuses.
    struct ExploreChunk final
    {
        std::string id{};
        std::string chunk{};

        // Whether this is the last line the agent means to send. A session
        // otherwise ends by falling silent for the idle timeout, which spends
        // that timeout every time and puts run.finished a default later than
        // the work actually stopped. It is a MODIFIER on a chunk and not a
        // command of its own, so the rule above holds: every line runs a chunk,
        // and an agent with nothing left to run sends one that returns nothing.
        bool endsSession{false};

        auto operator==(ExploreChunk const&) const -> bool = default;
    };

    // An unknown, repeated or missing required member, a value of the wrong
    // type, an unescaped control byte and trailing content are all REFUSED: a
    // line that is nearly right runs code against a live target.
    [[nodiscard]]
    auto parseExploreChunk(std::string_view line) -> Result<ExploreChunk>;

    // Exactly one value member is set on a successful line -- or none, for a chunk
    // that returned nothing. They are separate members rather than one
    // stringly-typed field because `"value":"3"` cannot tell the number three from
    // the text "3".
    struct ExploreResult final
    {
        std::string id{};
        bool        ok{};

        std::optional<bool>        boolean{};
        std::optional<double>      number{};
        std::optional<std::string> text{};

        // The domain's own wire spelling, so an agent reads the same string the
        // trace line and a Tier B error carry.
        std::optional<std::string> errorKind{};
        std::optional<std::string> message{};

        // Rendered as `"heap":{"used":U,"ceiling":C}`. Every chunk carries it
        // because the ceiling is measured against garbage as well as live data, so
        // an agent that learns the figure only when a chunk fails learns it one
        // chunk too late. Ceiling zero means the VM was built without one; absent
        // only on a line that answered no chunk.
        std::optional<script::HeapUsage> heap{};

        // Rendered as `"ended":true`, and only then. A results file is read
        // without the queue beside it, so "the agent said this was the last
        // one" and "the session stopped hearing from anyone" have to be
        // distinguishable in the answers themselves.
        bool ended{false};
    };

    [[nodiscard]]
    auto serializeExploreResult(ExploreResult const& result) -> std::string;

    // Where a ScriptValue becomes the three optional members above. One place, so
    // a chunk returning false and a chunk returning nothing cannot be rendered the
    // same way.
    [[nodiscard]]
    auto exploreSuccess(
        std::string_view id,
        script::ScriptValue const& value,
        script::HeapUsage heap,
        bool ended
    ) -> std::string;

    [[nodiscard]]
    auto exploreFailure(
        std::string_view id,
        Error const& error,
        script::HeapUsage heap,
        bool ended
    ) -> std::string;

    // For a queue line that could not be parsed at all. It names no id: inventing
    // one would attribute the refusal to a chunk the agent may not have sent.
    [[nodiscard]]
    auto serializeExploreParseFailure(Error const& error) -> std::string;
}
