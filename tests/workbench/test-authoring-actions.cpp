#include "../annotation/test-helpers.hpp"

#include <authoring-actions.hpp>
#include <authoring-edit.hpp>
#include <panel-state.hpp>
#include <app/workbench-app.hpp>

#include <annotation/authoring-document.hpp>
#include <annotation/catalog.hpp>
#include <annotation/content-hash.hpp>

#include <core/error/result.hpp>

#include <doctest/doctest.h>

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace uf::workbench
{
    namespace
    {
        constexpr auto k_sourceId = "00000000-0000-0000-0000-000000000701";
        constexpr auto k_anchorId = "00000000-0000-0000-0000-000000000711";
        constexpr auto k_pageId   = "00000000-0000-0000-0000-000000000721";
        constexpr auto k_absentId = "00000000-0000-0000-0000-0000000007ff";

        [[nodiscard]]
        auto document() -> annotation::AuthoringDocument
        {
            auto const fingerprint = annotation::test::fingerprint(8, 8, 96, 96);
            auto const sourceId    = annotation::test::sourceId(k_sourceId);
            auto const anchorId    = annotation::test::recognizerId(k_anchorId);
            auto const pageId      = annotation::test::pageId(k_pageId);
            auto const sourceHash  = annotation::sha256(std::span<std::byte const>{});
            REQUIRE(sourceHash.has_value());

            auto source = annotation::AuthoringSource::create(
                annotation::AuthoringSourceSpec{
                    .id          = sourceId,
                    .contentHash = *sourceHash,
                    .fingerprint = fingerprint,
                    .provenance  = annotation::ImportedSourceProvenance{},
                }
            );
            REQUIRE(source.has_value());

            auto created = annotation::AuthoringDocument::create(
                annotation::test::projectId(),
                fingerprint,
                {*source},
                {
                    annotation::test::anchorElement(
                        fingerprint,
                        anchorId,
                        "home_marker",
                        sourceId,
                        annotation::test::pixelRect(0, 0, 2, 2),
                        annotation::test::pixelRect(0, 0, 4, 4)
                    ),
                },
                {annotation::test::page(pageId, "home", {anchorId})},
                {},
                {}
            );
            REQUIRE(created.has_value());
            return *std::move(created);
        }

        [[nodiscard]]
        auto appState() -> AppState
        {
            return AppState{std::filesystem::path{"personal.workbench"}, document(), {}};
        }

        // A draft that differs from the current document, so applying it is a
        // genuine change rather than a no-op the history is right to ignore.
        [[nodiscard]]
        auto renamedDraft(AppState const& state, std::string name) -> AuthoringDraft
        {
            auto draft = state.draft();
            REQUIRE_FALSE(draft.recognizers.empty());
            draft.recognizers.front().name = std::move(name);
            return draft;
        }
    }

    TEST_CASE("a short id is the leading eight characters")
    {
        auto const id = annotation::test::recognizerId(k_anchorId);
        CHECK(shortId(id.value()) == "00000000");
        CHECK(shortId(id.value()).size() == 8U);
    }

    TEST_CASE("a requested edit is parked, not committed")
    {
        auto state = appState();
        auto ui    = PanelUiState{};

        requestEdit(ui, renamedDraft(state, "renamed"), "renamed the marker");

        // Panels are still borrowing into the document at this point, so the
        // rename must not have replaced it yet.
        CHECK(ui.pendingEdit.has_value());
        CHECK(state.document().catalog().recognizers().front().name().value() == "home_marker");
        CHECK(ui.statusLine.empty());
    }

    TEST_CASE("the parked edit is committed once and then cleared")
    {
        auto state = appState();
        auto ui    = PanelUiState{};

        requestEdit(ui, renamedDraft(state, "renamed"), "renamed the marker");
        applyPendingEdit(state, ui);

        CHECK_FALSE(ui.pendingEdit.has_value());
        CHECK(state.document().catalog().recognizers().front().name().value() == "renamed");
        CHECK(ui.statusLine == "renamed the marker");
        CHECK(state.canUndo());

        // A second apply in the next frame must not repeat it.
        ui.statusLine.clear();
        applyPendingEdit(state, ui);
        CHECK(ui.statusLine.empty());
    }

    TEST_CASE("a second request in the same frame does not displace the first")
    {
        auto state = appState();
        auto ui    = PanelUiState{};

        requestEdit(ui, renamedDraft(state, "first"), "first edit");
        requestEdit(ui, renamedDraft(state, "second"), "second edit");
        applyPendingEdit(state, ui);

        // One frame commits one edit, so the second press is dropped rather than
        // silently overwriting an edit the author already made.
        CHECK(state.document().catalog().recognizers().front().name().value() == "first");
        CHECK(ui.statusLine == "first edit");
    }

    TEST_CASE("an invalid draft is reported and leaves the document untouched")
    {
        auto state = appState();
        auto ui    = PanelUiState{};

        // An empty resource name cannot survive ResourceName::create, so the
        // rebuild fails and the current version has to stay whole.
        requestEdit(ui, renamedDraft(state, ""), "renamed the marker");
        applyPendingEdit(state, ui);

        CHECK(ui.statusLine.find("edit rejected") != std::string::npos);
        CHECK(state.document().catalog().recognizers().front().name().value() == "home_marker");
        CHECK_FALSE(state.canUndo());
        CHECK_FALSE(ui.pendingEdit.has_value());
    }

    TEST_CASE("an entity is selected only once its edit has landed")
    {
        auto state = appState();
        auto ui    = PanelUiState{};
        auto const anchorId = annotation::test::recognizerId(k_anchorId);

        requestEditSelecting(
            ui,
            renamedDraft(state, "renamed"),
            "renamed the marker",
            anchorId,
            std::optional<annotation::SourceId>{annotation::test::sourceId(k_sourceId)}
        );

        // Selecting before the commit would leave the selection pointing at an
        // id a rejected edit never added.
        CHECK_FALSE(state.selectedRecognizerId().has_value());

        applyPendingEdit(state, ui);

        REQUIRE(state.selectedRecognizerId().has_value());
        CHECK(*state.selectedRecognizerId() == anchorId);
        REQUIRE(state.selectedSourceId().has_value());
        CHECK(*state.selectedSourceId() == annotation::test::sourceId(k_sourceId));
    }

    TEST_CASE("a rejected edit does not move the selection")
    {
        auto state = appState();
        auto ui    = PanelUiState{};

        requestEditSelecting(
            ui,
            renamedDraft(state, ""),
            "renamed the marker",
            annotation::test::recognizerId(k_anchorId),
            std::nullopt
        );
        applyPendingEdit(state, ui);

        CHECK_FALSE(state.selectedRecognizerId().has_value());
    }

    TEST_CASE("committing an edit abandons a model check of the old document")
    {
        auto state = appState();
        auto ui    = PanelUiState{};

        ui.modelCheck.startWith(
            [](std::stop_token) -> Result<ModelCheck>
            {
                return ModelCheck{};
            }
        );
        while (ui.modelCheck.running())
        {
            std::this_thread::sleep_for(std::chrono::milliseconds{1});
        }

        requestEdit(ui, renamedDraft(state, "renamed"), "renamed the marker");
        applyPendingEdit(state, ui);

        // Its verdict would describe marks that have since moved, so it must not
        // survive to be collected against the new document.
        CHECK_FALSE(ui.modelCheck.takeResult().has_value());
    }

    TEST_CASE("a refused deletion is reported and parks nothing")
    {
        auto ui = PanelUiState{};

        requestDeletion(
            ui,
            fail(AutomationErrorKind::ActionRejected, "the page would have no signature"),
            "page"
        );

        CHECK_FALSE(ui.pendingEdit.has_value());
        CHECK(ui.statusLine.find("page") != std::string::npos);
    }

    TEST_CASE("a deletion states what it withdrew along the way")
    {
        auto const state = appState();

        auto const quiet = deletionSummary(
            "recognizer",
            DeletedEntity{.draft = state.draft(), .withdrawnRoles = 0U}
        );
        auto const noisy = deletionSummary(
            "recognizer",
            DeletedEntity{.draft = state.draft(), .withdrawnRoles = 3U}
        );

        // A membership the author did not ask to remove must not be silent.
        CHECK(quiet != noisy);
        CHECK(noisy.find("3") != std::string::npos);
    }

    TEST_CASE("a recognizer resolves to the screen it was authored against")
    {
        auto const state = appState();

        auto const found = sourceOfRecognizer(
            state,
            annotation::test::recognizerId(k_anchorId)
        );
        REQUIRE(found.has_value());
        CHECK(*found == annotation::test::sourceId(k_sourceId));

        CHECK_FALSE(
            sourceOfRecognizer(state, annotation::test::recognizerId(k_absentId)).has_value()
        );
    }

    TEST_CASE("a page name falls back to its id rather than to nothing")
    {
        auto const state = appState();

        CHECK(pageName(state, annotation::test::pageId(k_pageId)) == "home");

        // It goes straight into a label, so an absent page still has to read as
        // something the author can match against the panel.
        CHECK_FALSE(pageName(state, annotation::test::pageId(k_absentId)).empty());
    }
}
