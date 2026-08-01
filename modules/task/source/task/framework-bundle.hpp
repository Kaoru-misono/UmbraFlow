#pragma once

#include <core/error/result.hpp>

#include <script/engine.hpp>

#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace uf::task
{
    // One module of the trusted Luau framework, embedded into the binary at
    // build time by scripts/embed_luau.py from modules/task/runtime.
    //
    // Lifetime contract: every view below borrows a string literal with static
    // storage duration inside the generated translation unit. An entry and the
    // views it carries therefore stay valid for the whole process and may be
    // stored without any further arrangement. This is the deliberate exception
    // to the project's "do not store views" rule -- the backing owner is the
    // program image itself, which outlives every possible observer.
    struct FrameworkBundleEntry final
    {
        // The Luau module name: the .luau file stem, never a path.
        std::string_view name{};

        // The module's UTF-8 source text, byte for byte as it is on disk.
        std::string_view source{};

        // Lowercase hex SHA-256 of `source`, computed by the generator at build
        // time. tests/task/test-framework-bundle.cpp recomputes it with
        // annotation::sha256, which is what keeps the build-time and run-time
        // hash definitions from drifting apart.
        std::string_view sourceHash{};
    };

    // The framework bundle, ordered by the generator's sort over the source
    // paths, so the sequence is identical on every machine and every build.
    [[nodiscard]]
    auto frameworkBundleEntries() noexcept -> std::span<FrameworkBundleEntry const>;

    // Lowercase hex SHA-256 over the whole bundle.
    //
    // Recipe, recomputable by hand: concatenate, for each entry of
    // frameworkBundleEntries() in order, the module name in UTF-8, one 0x00
    // separator byte, then that module's source bytes; the bundle hash is the
    // SHA-256 of that byte string. NUL occurs in neither a module name nor a
    // source, so the concatenation is unambiguous.
    //
    // What this hash is for, stated plainly: stamping a trace so one run is
    // attributable to an exact framework build, and catching a bundle that went
    // accidentally stale. It is NOT a security property and must not be
    // described as one. The digest is compiled into the same binary as the bytes
    // it certifies, so it proves nothing against anyone able to rewrite that
    // binary -- they would simply rewrite the digest too.
    [[nodiscard]]
    auto frameworkBundleHash() noexcept -> std::string_view;

    // The framework's semantic version, declared as [embed].luau_version in
    // modules/task/manifest.txt and stamped into the bundle at build time.
    [[nodiscard]]
    auto frameworkVersion() noexcept -> std::string_view;

    // The same bundle, in the shape script::EngineConfig::frameworkModules
    // takes, so every VM that boots this framework builds the list one way.
    //
    // The views it carries are the bundle's own, so they live for the whole
    // process and the returned vector may be handed to a config that outlives
    // this call. Only the vector itself is fresh, and each call makes one --
    // this runs once per VM, not once per script call.
    [[nodiscard]]
    auto frameworkScriptModules() -> std::vector<script::FrameworkModule>;

    // The framework module names a project script may name, in the shape
    // script::EngineConfig::frameworkProjectGlobals takes: `ctx`, the context
    // object a task uses while it runs, and `task`, the declaration surface it
    // registers its interrupts through. Publishing them is what let the bare
    // verbs leave the project environment.
    //
    // The names are spelled here rather than derived, because the generator
    // takes them from file stems and a C++ constant cannot read one. A rename
    // that missed this list fails VM creation with InternalInvariant naming the
    // missing entry, so the two cannot drift apart silently.
    [[nodiscard]]
    auto frameworkProjectGlobals() -> std::vector<std::string>;

    // The same list for an exploration VM, plus `explore` and `scribe`.
    //
    // THE DIFFERENCE BETWEEN THE TWO LISTS IS THE ISOLATION. A project
    // environment is an explicit whitelist with no metatable, so it holds no
    // route to anything not named in it; publishing a framework export copies
    // the value rather than opening a chain. So the run environment does not
    // merely refuse `explore.click_point` -- it has no `explore` at all, and no
    // path from anything it does have to the module's table. That is section 7 of
    // docs/plans/2026-07-29-three-layer-task-system.md doing the work section 2
    // of the agent-operator document asks for.
    //
    // Both modules load in EVERY VM, because the bundle is one bundle. What they
    // reach differs: `explore` forwards primitives that are absent from a run
    // VM's private surface, so on such a VM its verbs raise instead of acting --
    // and nothing on a run VM can call them anyway, since nothing names it.
    [[nodiscard]]
    auto explorationProjectGlobals() -> std::vector<std::string>;

    // Parses one framework module with the same vendored Luau parser the host
    // uses at load time, and reports the first syntax error together with its
    // line and column. `chunkName` only labels the diagnostic.
    //
    // This is the repository's syntax gate for .luau sources: the bundle test
    // runs it over every embedded entry, so a malformed framework module fails
    // under `ctest -L CI` at the same moment it would otherwise have reached the
    // VM. A failure is InternalInvariant rather than InvalidResource, because
    // the framework is first-party and compiled into the binary -- a syntax
    // error there is a broken host, not bad user input.
    //
    // No Luau type appears in this header. The parse lives behind the module's
    // ffi boundary, exactly as validateScriptResources does.
    [[nodiscard]]
    auto checkFrameworkModuleSyntax(
        std::string_view source,
        std::string_view chunkName
    ) -> Status;
}
