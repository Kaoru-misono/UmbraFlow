#pragma once

#include <script/engine.hpp>

#include <core/error/result.hpp>
#include <core/safety/annotations.hpp>

#include <annotation/catalog.hpp>

#include <cstddef>
#include <span>
#include <string>
#include <vector>

namespace uf::task
{
    class TaskContext;

    // One action target the catalog exposes to scripts under
    // umbra.recognizers.<name>. It pairs the recognizer's validated Luau
    // member-key name with its stable identity; the name becomes the table key
    // and the id is baked opaquely into the userdata handle. A plain value type,
    // freely copyable, so the installer can own its own snapshot.
    struct RecognizerHandleSpec final
    {
        std::string              name;
        annotation::RecognizerId id;
    };

    // One page the catalog exposes to scripts under umbra.pages.<name>, with the
    // same shape and role as RecognizerHandleSpec.
    struct PageHandleSpec final
    {
        std::string        name;
        annotation::PageId id;
    };

    // Builds and owns the script-visible capability surface for one project's
    // recognition catalog: the recursively read-only umbra.recognizers and
    // umbra.pages name tables of opaque handles. Construction validates every
    // exposed name (fail-closed) and captures the {name, id} pairs; installer()
    // then vends a script::HostTableInstaller that materializes the frozen umbra
    // table on a task VM.
    //
    // No Luau type appears in this header: the Luau work lives behind the ffi
    // boundary and is reached only through the returned installer. The resource
    // handles built here carry the recognizer / page identity the observation and
    // action verbs consume; installer(TaskContext&) wires those verbs onto the
    // same umbra table, while installer() vends a resource-only table with no
    // bound session (used where only the name closure matters).
    class CapabilitySurface final
    {
        std::vector<RecognizerHandleSpec> m_recognizers;
        std::vector<PageHandleSpec>       m_pages;

        CapabilitySurface(
            std::vector<RecognizerHandleSpec> recognizers,
            std::vector<PageHandleSpec> pages
        ) noexcept;

    public:
        // Enumerates the catalog's action-target recognizers and its pages,
        // checks each exposed name is unique within its table, and captures the
        // {name, id} pairs. A duplicate name fails InvalidResource rather than
        // silently overwriting a handle (annotation-design 3.4). Names are
        // already valid direct Luau member keys by annotation::ResourceName's
        // construction invariant, so the surface never observes an illegal key.
        //
        // Page anchors never enter umbra.recognizers: scripts reference anchors
        // only through pages, and frame:find authorizes action targets alone. An
        // info_region recognizer has no script verb in this wave, so it is not
        // exposed either; it joins umbra.recognizers when its read verb lands.
        [[nodiscard]]
        static auto create(
            annotation::RecognitionCatalog const& catalog
        ) -> Result<CapabilitySurface>;

        // A host-table installer suitable for script::EngineConfig::installHostTables.
        // Invoked once per task VM before the sandbox freezes the globals, it
        // builds the frozen global umbra table. The returned installer owns its
        // own copy of the handle specs, so it stays valid independently of this
        // surface's lifetime. This overload registers the resource tables only:
        // umbra.recognizers and umbra.pages, with no observation or action verbs.
        [[nodiscard]]
        auto installer() const -> script::HostTableInstaller;

        // The full installer for a live task: the resource tables plus the
        // umbra:capture / frame:resolve_page / outcome:resolved / frame:find /
        // umbra:click / page:is verbs, each bound to `context`'s EngineSession.
        //
        // The returned installer captures a raw pointer to `context`. The caller
        // MUST keep `context` alive for at least as long as the script::Engine the
        // installer configures, because the VM's host functions dereference that
        // pointer on every verb call; the TaskContext is non-movable so the
        // address stays stable. The installer still owns its own copy of the specs.
        [[nodiscard]]
        auto installer(TaskContext& context) const -> script::HostTableInstaller;

        [[nodiscard]]
        auto recognizerCount() const noexcept -> std::size_t;

        [[nodiscard]]
        auto pageCount() const noexcept -> std::size_t;

        // The action-target recognizer handles this surface exposes under
        // umbra.recognizers, in catalog order. These are exactly the names an
        // umbra.recognizers.<name> literal may resolve against, so the pre-VM
        // script validator (script-validator.hpp) checks every reference here
        // rather than against the wider catalog, which also holds page anchors
        // that never become findable handles. The returned span borrows this
        // surface's storage and stays valid only while the surface is alive.
        [[nodiscard]]
        auto recognizers() const noexcept UF_LIFETIME_BOUND
            -> std::span<RecognizerHandleSpec const>;

        // The page handles this surface exposes under umbra.pages, the resolution
        // set for every umbra.pages.<name> literal, with the same borrow contract
        // as recognizers().
        [[nodiscard]]
        auto pages() const noexcept UF_LIFETIME_BOUND -> std::span<PageHandleSpec const>;
    };
}
