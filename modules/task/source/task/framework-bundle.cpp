#include "framework-bundle.hpp"

#include <script/engine.hpp>

#include <string>
#include <vector>

namespace uf::task
{
    namespace
    {
        // The framework modules whose frozen exports become project globals of
        // the same name. Spelled once so the bundle and the whitelist agree.
        //
        // `ctx` is what a task does while it runs; `task` is what it declares
        // about itself beforehand. `task` is the transitional spelling of the
        // design's `uf.task`, which cannot be a member of the frozen `uf` table
        // the host installs after the bundle has already loaded.
        constexpr auto k_contextModule     = "ctx";
        constexpr auto k_declarationModule = "task";
        // The script-owned page model: nouns, verbs and persistence live in
        // trusted Luau and project scripts consume them by name.
        constexpr auto k_modelModule   = "model";
        constexpr auto k_observeModule = "observe";
        constexpr auto k_projectModule = "project";
        // The capture-free operations on what a capture produced: pick the line
        // saying X out of a list of them, aim a click at a known offset from a
        // hit. Neither confers anything -- `hits.offset` mints only from a hit the
        // framework already minted and inherits its cycle, so it can express no
        // click a task could not already have asked for at the line itself.
        constexpr auto k_hitsModule = "hits";
        // navigation carries Edge/Graph/stack for layer-three scripts. mint is
        // deliberately NOT here: it is model/navigation's shared internals and
        // every function on it is reachable through a published constructor.
        // `evidence` is not here either, and for a stronger reason than mint's:
        // it is the ledger saying which hits and receipts this framework minted
        // and on which frame, so a project that could name it could mint a hit
        // claiming interact on an element no page ever authorised and
        // observe.click would accept it.
        constexpr auto k_navigationModule = "navigation";
        // The falsification matrix. All four are published for `model`'s reason --
        // a project that grows its own screens or reads a verdict names them --
        // and the routine `umbra-flow check` runs reaches them through this list
        // rather than through a private route.
        //
        // `reading` and `recognition` mint nothing -- their verbs read a region or
        // resolve a page through the caller's own ctx and compare claims already
        // in the file -- which is why they are published where `mint` is not.
        constexpr auto k_oracleModule      = "oracle";
        constexpr auto k_readingModule     = "reading";
        constexpr auto k_recognitionModule = "recognition";
        constexpr auto k_regressModule     = "regress";
        // The two modules ONLY the exploration environment publishes. `explore`
        // is the forwards for the privileged primitives -- the bare-coordinate
        // click, the crop and the probe -- and `scribe` is the measure-and-author
        // loop built on top of them. Neither is in the run list, which is the
        // whole of how a business script is kept from naming a bare click: the
        // project environment is a whitelist with no metatable, so a name that is
        // not published is not reachable by any route
        // (docs/plans/2026-07-29-three-layer-task-system.md 7).
        constexpr auto k_exploreModule = "explore";
        constexpr auto k_scribeModule  = "scribe";
    }

    auto frameworkScriptModules() -> std::vector<script::FrameworkModule>
    {
        auto const entries = frameworkBundleEntries();

        auto modules = std::vector<script::FrameworkModule>{};
        modules.reserve(entries.size());
        for (auto const& entry : entries)
        {
            modules.emplace_back(
                script::FrameworkModule{
                    .name   = entry.name,
                    .source = entry.source,
                }
            );
        }
        return modules;
    }

    auto frameworkProjectGlobals() -> std::vector<std::string>
    {
        return std::vector<std::string>{
            std::string{k_contextModule},
            std::string{k_declarationModule},
            std::string{k_modelModule},
            std::string{k_observeModule},
            std::string{k_projectModule},
            std::string{k_hitsModule},
            std::string{k_navigationModule},
            std::string{k_oracleModule},
            std::string{k_readingModule},
            std::string{k_recognitionModule},
            std::string{k_regressModule},
        };
    }

    auto explorationProjectGlobals() -> std::vector<std::string>
    {
        auto names = frameworkProjectGlobals();
        names.emplace_back(k_exploreModule);
        names.emplace_back(k_scribeModule);
        return names;
    }
}
