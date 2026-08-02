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
        std::vector<std::string> elements{};
        std::vector<std::string> pages{};
    };

    // Proves, before any Luau VM is created, that `source` (compiled under chunk
    // name `chunkName`) reaches the uf namespace only through the canonical
    // literal spellings -- uf.elements.<name>, uf.pages.<name> and
    // uf.errors.<kind> direct member access -- and that every named resource is
    // one `model` declares. Every reference is enumerated and closed here, so a
    // script naming a page or an element the project file does not declare fails
    // before the VM exists (docs/plans/2026-07-31-script-owned-page-model.md 6).
    //
    // An error-kind leaf is resolved against AutomationErrorKind's wire spellings,
    // so a typo fails here rather than becoming a nil that silently loses every
    // comparison. It contributes nothing to the report: the kinds are host
    // vocabulary fixed for the binary, not project resources.
    //
    // Every other contact with the uf global is rejected: a namespace or handle
    // alias (local r = uf, local r = uf.elements), a computed index
    // (uf.elements[name]), dynamic traversal (pairs(uf)), passing or returning it,
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
        PageModelFacts const& model
    ) -> Result<ScriptResourceReport>;
}
