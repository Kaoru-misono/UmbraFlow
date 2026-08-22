#pragma once

#include "args.hpp"
#include "observe.hpp"

#include <core/error/result.hpp>
#include <core/types/integer.hpp>

#include <filesystem>
#include <optional>
#include <string>

namespace uf::cli
{
    // What one Tool call left in the Tool Runtime's durable record.
    //
    // Plain strings and integers for ReclaimedRuntime's reason: operator is a
    // private dependency of this module, so nothing a caller of this header
    // reads obliges it to link the authority that answered. The Operator's own
    // ToolCallReplay is restated here rather than carried.
    struct ToolInvokeReport final
    {
        std::filesystem::path project{};
        std::filesystem::path runtimeArtifactRoot{};

        std::string deployment{};
        std::string pluginId{};

        // The --actor word this call was presented at, the name that transport
        // carried, and the key the durable root request is idempotent on. All
        // three, because a reader going looking for the row this run opened
        // finds it by the principal that opened it and the key it opened under.
        std::string actor{};
        std::string toolName{};
        std::string requestKey{};

        // The durable call's state in the Operator's own wire vocabulary, and
        // the two counters that say which attempt answered it. A rerun under
        // the same --request-key resolves to this same row, so two runs are
        // compared on the revision rather than on the wall clock.
        std::string state{};

        uint64 revision{};
        uint64 activeAdmissionAttempt{};

        // The provider's exact canonical result and evidence documents, absent
        // when the call recorded none. Carried and printed whole rather than
        // summarized, as observe carries its StateResolution: C++ interprets no
        // field of either, and these are the bytes the Tool Runtime stored.
        //
        // Absent and empty are different answers. A Tool that recorded no
        // evidence and a Tool that recorded an empty document are two states of
        // one durable row, and a report that spelled both as "" would hide the
        // difference from the only reader able to act on it.
        std::optional<std::string> payload{};
        std::optional<std::string> evidence{};

        std::filesystem::path trace{};
    };

    // Starts one Tool call over `sources` and reports the replay the Tool
    // Runtime recorded for it.
    //
    // Toward the Operator: ProductLifecycle is the single production
    // construction site, exactly as it is for observeProject. The lifecycle
    // starts writable because start acquires the control lease before it
    // returns, so a mutating Tool is reachable here -- which is the whole
    // difference between this verb and the read-only observation beside it.
    //
    // ObserveSources is reused rather than restated. It is the three ports one
    // session runs over plus the geometry the target presented, and a second
    // struct spelling the same four members would be a second answer to one
    // question.
    //
    // The sources are consumed: EngineSession takes ownership of all three
    // ports, and a caller that kept one would hold a borrow of the session's
    // state with no contract keeping it alive.
    [[nodiscard]]
    auto invokeTool(
        InvokeArgs const& args,
        ObserveSources sources
    ) -> Result<ToolInvokeReport>;

    // The composition root: binds the live window --hwnd names and the OCR
    // engine --ocr-models names, then runs the function above. Windows only,
    // as observe and explore are.
    [[nodiscard]]
    auto invokeToolProduct(InvokeArgs const& args) -> Result<ToolInvokeReport>;

    // Separate from the call so the shape an operator and a script both read
    // is testable without a target, as formatObservedState is.
    [[nodiscard]]
    auto formatToolInvoke(ToolInvokeReport const& report) -> std::string;
}
