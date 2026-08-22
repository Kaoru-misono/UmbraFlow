#pragma once

#include "manifest.hpp"
#include "project-plugin.hpp"
#include "tool-invocation.hpp"

#include <script/scoped-tool-program.hpp>

#include <json/value.hpp>

#include <core/error/result.hpp>
#include <core/safety/annotations.hpp>

#include <domain/content-hash.hpp>

#include <map>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace uf::operator_runtime
{
    // The scoped execution environment a loaded Project generation runs in:
    // everything script::scopedToolEnvironmentMaterial() attests to -- the
    // trusted bridge, the compiler options, every ceiling, the scoped module
    // catalog and the Tool Runtime facade contract -- plus the exact source
    // bytes, depth and Project visibility of every Framework module the scoped
    // closure admits, and the reserved resource name the pinned Tool catalog is
    // read from.
    //
    // It is deliberately a second identity beside
    // currentProjectPluginEnvironmentMaterial() and not a replacement for it. A
    // registration's plugin_environment_hash pins the pure closure both program
    // types share, and this pins what only the scoped type adds; a build that
    // moved a scoped facade's bytes moves this and moves that one not at all.
    // Nothing folds the two into one digest, because a project that ships no
    // handler would then have its identity moved by facades it can never load.
    [[nodiscard]]
    auto currentScopedToolEnvironmentMaterial() -> Result<std::string>;

    [[nodiscard]] auto currentScopedToolEnvironmentHash() -> Result<ContentHash>;

    // The run's pinned Tool catalog, as the read-only Framework resource
    // @umbraflow/tools reads its discovery table from. It is a resource rather
    // than a native call because discovery must cost no Tool-call budget and
    // must be identical on replay: these bytes are fixed at program
    // construction, so a description cannot move under a running script.
    //
    // It carries BOTH catalogs, because both are callable from a scoped run:
    // the Framework's Tools are how a handler reaches the world, and the
    // Project's own are how one Tool composes another.
    //
    // It is a free function of the two catalogs rather than a step inside one
    // loader because both loaders of a scoped closure need exactly these bytes.
    // A second copy would be a second discovery table, and a scoped run has
    // one.
    [[nodiscard]]
    auto pinnedToolCatalogResource(
        FrameworkToolCatalogOwner const& frameworkCatalog,
        ProjectToolCatalogSchemaOwner const& projectCatalog
    ) -> Result<script::PureDataProgram::Resource>;

} // namespace uf::operator_runtime
