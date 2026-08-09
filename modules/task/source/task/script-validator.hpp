#pragma once

#include <task/page-model-file.hpp>

#include <core/error/result.hpp>

#include <string>
#include <string_view>
#include <vector>

namespace uf::task
{

    // The resource references one task script makes, enumerated by the pre-VM
    // AST validator. Both lists are deduplicated and sorted, so a host trace and
    // a test can assert on a stable set independent of source order or repeated
    // uses. A script that touches no resources yields two empty lists and still
    // validates. No Luau type appears here: the parse and traversal live behind
    // the ffi boundary and are reached only through validateScriptResources.
    struct ScriptResourceReport final
    {
        std::vector<std::string> targets{};
        std::vector<std::string> surfaces{};
    };

    // Proves, before any Luau VM is created, that `source` (compiled under chunk
    // name `chunkName`) reaches the uf namespace only through the canonical
    // literal spellings -- uf.targets.<id>, uf.surfaces.<id> and
    // uf.errors.<kind> direct member access -- and that every named resource is
    // one `model` declares. Every reference is enumerated and closed here, so a
    // script naming a target or a surface the project file does not declare fails
    // before the VM exists (docs/plans/2026-07-31-script-owned-page-model.md 6).
    //
    // An error-kind leaf is resolved against AutomationErrorKind's wire spellings,
    // so a typo fails here rather than becoming a nil that silently loses every
    // comparison. It contributes nothing to the report: the kinds are host
    // vocabulary fixed for the binary, not project resources.
    //
    // Every other contact with the uf global is rejected: a namespace or handle
    // alias (local r = uf, local r = uf.targets), a computed index
    // (uf.targets[name]), dynamic traversal (pairs(uf)), passing or returning it,
    // and a method call (uf:anything(...)) -- the root carries data alone, so
    // there is no verb form to approve. A syntax error is likewise rejected. Every
    // failure is AutomationErrorKind::InvalidResource and names the offending
    // source location.
    //
    // The framework's own `ctx` is out of scope here: it exposes no resource name.
    [[nodiscard]]
    auto validateScriptResources(
        std::string_view source,
        std::string_view chunkName,
        RuntimeModelEnvelope const& model
    ) -> Result<ScriptResourceReport>;
}
