#pragma once

#include <core/error/result.hpp>

#include <domain/content-hash.hpp>

#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>

namespace uf::task
{
    // Upper bound on a task script's on-disk size, enforced before the bytes are
    // read so a malformed or hostile project cannot force an unbounded read. A
    // task is human-written Luau, orders of magnitude smaller than this; the cap
    // states this module's own capped-read discipline rather than any script
    // property. (Corrected 2026-08-09: it cited
    // engine::k_maximumRuntimeManifestBytes, which went with the engine's
    // runtime manifest loader when the catalog left C++.)
    inline constexpr auto k_maximumTaskSourceBytes = std::size_t{4} * 1024U * 1024U;

    // One task resolved from its owning project by name
    // (docs/plans/2026-07-29-three-layer-task-system.md section 6): the validated
    // task name, the whole script source, and the content hash the host stamps
    // into the trace at load time. ContentHash has no default state, so the
    // aggregate has no meaningful default; build it through loadTask.
    struct LoadedTask final
    {
        std::string name{};
        std::string source{};
        ContentHash hash;
    };

    // Resolves `taskName` to <projectRoot>/tasks/<taskName>.luau and loads it. The
    // name must be one safe path segment -- non-empty and drawn only from
    // [A-Za-z0-9_-] -- so a separator, a dot, "..", or an absolute path can never
    // escape the tasks directory; an illegal name fails InvalidResource. A name
    // with no matching file fails InvalidResource too, and the message lists the
    // task names that do exist so the operator can correct the spelling. The
    // source is read under k_maximumTaskSourceBytes and its content hash computed
    // with the repository's sha256 facility. The CLI never executes a
    // loose-path script: a task is always addressed as (project, name).
    [[nodiscard]]
    auto loadTask(
        std::filesystem::path const& projectRoot,
        std::string_view taskName
    ) -> Result<LoadedTask>;

    // The Luau compiler/runtime version stamp the host records in a task's trace
    // beside the script hash: the bytecode target version, which is the only
    // stable version constant the vendored VM exposes. Process-global, not
    // per-task; the constant is read behind the ffi boundary so no Luau header
    // reaches this public header.
    [[nodiscard]]
    auto luauRuntimeVersion() -> std::string;
}
