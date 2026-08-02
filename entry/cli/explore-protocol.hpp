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
    // The longest queue line the exploration protocol accepts.
    //
    // Larger than the operator protocol's, because the payload is different in
    // kind: an operator line is a handful of scalars, while an agent line is a
    // program. A quarter of a megabyte is far above any chunk written by hand or
    // by a model -- the longest authoring snippet this repository has seen is a
    // few kilobytes -- and far below a line that could only be a mistake.
    inline constexpr auto k_maxExploreLineBytes = std::size_t{256} * 1024U;

    // The longest chunk id the protocol accepts, in bytes.
    //
    // An id is what an agent matches a result line back to the chunk it sent, so
    // it has to be readable and it has to be short: it also labels the chunk in
    // compile diagnostics and in a raised error's traceback, where a paragraph
    // would drown the message it was meant to locate.
    inline constexpr auto k_maxExploreIdBytes = std::size_t{128};

    // THE EXPLORATION QUEUE PROTOCOL.
    //
    // One JSON object per line, with exactly two members:
    //
    //   {"id":"step-3","chunk":"local t = ctx:cycle_open() ... return 'home'"}
    //
    // WHY A CHUNK AND NOT A COMMAND. This is the formal carrier of "the agent
    // calls verbs one at a time" (docs/plans/2026-08-01-three-layers-and-agent-
    // operator.md 3). An operator protocol can post one primitive per line
    // because an operator has no composition to express; an agent's smallest
    // useful act is already a composition -- open a cycle, crop a corner, close
    // it -- and a protocol that made it three lines would put the framework's
    // control flow in the queue file, where nothing can check it.
    //
    // WHY THE ID IS REQUIRED. A session answers every line, including one it
    // refuses, and the agent has to know WHICH line an answer is about. Inventing
    // an ordinal here would make the answer depend on how many lines the reader
    // had seen, which is not something an agent that reconnects can know.
    struct ExploreChunk final
    {
        std::string id{};
        std::string chunk{};

        auto operator==(ExploreChunk const&) const -> bool = default;
    };

    // Reads one queue line.
    //
    // Strict, on the operator protocol's terms and for its reason: an unknown
    // member, a repeated member, a missing member, a non-string value, an
    // unescaped control byte and trailing content are all REFUSED rather than
    // tolerated. A line that is nearly right runs code against a live target, so
    // it must fail loudly instead of being interpreted generously.
    [[nodiscard]]
    auto parseExploreChunk(std::string_view line) -> Result<ExploreChunk>;

    // One chunk's result as its line reports it.
    //
    // The value members carry what the chunk returned, and exactly one of them is
    // set on a successful line -- or none, for a chunk that returned nothing.
    // They are separate members rather than one stringly-typed field because an
    // agent reading `"value":"3"` cannot tell the number three from the text
    // "3", and the distinction is the difference between a count and a page name.
    struct ExploreResult final
    {
        std::string id{};
        bool        ok{};

        std::optional<bool>        boolean{};
        std::optional<double>      number{};
        std::optional<std::string> text{};

        // The domain's own wire spelling of the failure kind, so an agent reads
        // the same string the trace line and a Tier B error carry.
        std::optional<std::string> errorKind{};
        std::optional<std::string> message{};

        // The VM's memory ledger after this chunk, rendered as
        // `"heap":{"used":U,"ceiling":C}`.
        //
        // EVERY CHUNK CARRIES IT, and that is the point: the ceiling is measured
        // against garbage as well as live data, so an agent that only learns the
        // figure when a chunk fails learns it one chunk too late. A ceiling of
        // zero means the VM was built without one.
        //
        // Absent only on a line that answered no chunk -- a queue line that
        // could not be parsed never reached a VM, so there is no reading to
        // report.
        std::optional<script::HeapUsage> heap{};
    };

    [[nodiscard]]
    auto serializeExploreResult(ExploreResult const& result) -> std::string;

    // The successful line for `value`, which is where a ScriptValue becomes the
    // three optional members above. One place, so a chunk returning false and a
    // chunk returning nothing cannot come to be rendered the same way.
    [[nodiscard]]
    auto exploreSuccess(
        std::string_view id,
        script::ScriptValue const& value,
        script::HeapUsage heap
    ) -> std::string;

    // The line for a chunk that failed, built from the error it failed with.
    [[nodiscard]]
    auto exploreFailure(
        std::string_view id,
        Error const& error,
        script::HeapUsage heap
    ) -> std::string;

    // The line for a queue line that could not be parsed at all. It names no id,
    // because the line did not successfully carry one -- and inventing one would
    // attribute the refusal to a chunk the agent may not have sent.
    [[nodiscard]]
    auto serializeExploreParseFailure(Error const& error) -> std::string;
}
