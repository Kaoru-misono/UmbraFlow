#include "../annotation/test-helpers.hpp"

#include <authoring-edit.hpp>

#include <annotation/authoring-document.hpp>
#include <annotation/content-hash.hpp>

#include <core/types/integer.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <cstddef>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace uf::workbench
{
    namespace
    {
        constexpr auto k_sourceId     = "00000000-0000-0000-0000-000000000201";
        constexpr auto k_anchorId     = "00000000-0000-0000-0000-000000000001";
        constexpr auto k_actionId     = "00000000-0000-0000-0000-000000000002";
        constexpr auto k_awayId       = "00000000-0000-0000-0000-000000000003";
        constexpr auto k_pageId       = "00000000-0000-0000-0000-000000000101";
        constexpr auto k_secondPageId = "00000000-0000-0000-0000-000000000102";
        constexpr auto k_regressionId = "00000000-0000-0000-0000-000000000301";

        [[nodiscard]]
        auto document() -> annotation::AuthoringDocument
        {
            auto const fingerprint = annotation::test::fingerprint(8, 8, 96, 96);
            auto const sourceId    = annotation::test::sourceId(k_sourceId);
            auto const anchorId    = annotation::test::recognizerId(k_anchorId);
            auto const actionId    = annotation::test::recognizerId(k_actionId);
            auto const pageId      = annotation::test::pageId(k_pageId);
            auto const sourceHash = annotation::sha256(
                std::span<std::byte const>{}
            );
            REQUIRE(sourceHash.has_value());
            auto source = annotation::AuthoringSource::create(
                annotation::AuthoringSourceSpec{
                    .m_id          = sourceId,
                    .m_contentHash = *sourceHash,
                    .m_fingerprint = fingerprint,
                    .m_provenance  = annotation::ImportedSourceProvenance{},
                }
            );
            REQUIRE(source.has_value());

            auto const click = annotation::TemplateOffset::create(1, 1, 2, 2);
            REQUIRE(click.has_value());
            auto constexpr classification = (
                annotation::RegressionClassification::Positive
            );
            auto created = annotation::AuthoringDocument::create(
                annotation::test::projectId(),
                fingerprint,
                {*source},
                {
                    annotation::AuthoringRecognizerSpec{
                        .m_definition = annotation::test::recognizer(
                            fingerprint,
                            anchorId,
                            "home_marker",
                            annotation::AnnotationType::PageAnchor,
                            annotation::test::pixelRect(0, 0, 2, 2),
                            annotation::test::pixelRect(0, 0, 4, 4)
                        ),
                        .m_sourceId = sourceId,
                    },
                    annotation::AuthoringRecognizerSpec{
                        .m_definition = annotation::test::recognizer(
                            fingerprint,
                            actionId,
                            "daily_button",
                            annotation::AnnotationType::ActionTarget,
                            annotation::test::pixelRect(4, 4, 2, 2),
                            annotation::test::pixelRect(3, 3, 4, 4),
                            {pageId},
                            *click
                        ),
                        .m_sourceId = sourceId,
                    },
                },
                {annotation::test::page(pageId, "home", {anchorId})},
                {
                    annotation::RegressionCase{
                        annotation::RegressionSpec{
                            .m_id = annotation::test::regressionId(
                                k_regressionId
                            ),
                            .m_sourceId       = sourceId,
                            .m_classification = classification,
                            .m_expectation = annotation::ResolvedRegression{
                                .m_pageId = pageId,
                            },
                        }
                    },
                }
            );
            REQUIRE(created.has_value());
            return *std::move(created);
        }

        // Covers the draft alternatives that document() leaves at their zero
        // value: WGC provenance, a non-Positive classification, an unresolved
        // expectation, and a non-empty forbidden set. Without these a draft
        // round-trip that silently dropped the field would still compare equal.
        [[nodiscard]]
        auto variantDocument() -> annotation::AuthoringDocument
        {
            auto const fingerprint = annotation::test::fingerprint(8, 8, 96, 96);
            auto const sourceId    = annotation::test::sourceId(k_sourceId);
            auto const anchorId    = annotation::test::recognizerId(k_anchorId);
            auto const awayId      = annotation::test::recognizerId(k_awayId);
            auto const pageId      = annotation::test::pageId(k_pageId);
            auto const sourceHash = annotation::sha256(
                std::span<std::byte const>{}
            );
            REQUIRE(sourceHash.has_value());
            auto source = annotation::AuthoringSource::create(
                annotation::AuthoringSourceSpec{
                    .m_id          = sourceId,
                    .m_contentHash = *sourceHash,
                    .m_fingerprint = fingerprint,
                    .m_provenance  = annotation::WgcSourceProvenance{
                        .m_targetGeneration = TargetGeneration::fromValue(7),
                        .m_capturedAt       = "2026-07-23T09:15:00+09:00",
                    },
                }
            );
            REQUIRE(source.has_value());

            auto constexpr classification = (
                annotation::RegressionClassification::Negative
            );
            auto created = annotation::AuthoringDocument::create(
                annotation::test::projectId(),
                fingerprint,
                {*source},
                {
                    annotation::AuthoringRecognizerSpec{
                        .m_definition = annotation::test::recognizer(
                            fingerprint,
                            anchorId,
                            "home_marker",
                            annotation::AnnotationType::PageAnchor,
                            annotation::test::pixelRect(0, 0, 2, 2),
                            annotation::test::pixelRect(0, 0, 4, 4)
                        ),
                        .m_sourceId = sourceId,
                    },
                    annotation::AuthoringRecognizerSpec{
                        .m_definition = annotation::test::recognizer(
                            fingerprint,
                            awayId,
                            "away_marker",
                            annotation::AnnotationType::PageAnchor,
                            annotation::test::pixelRect(4, 4, 2, 2),
                            annotation::test::pixelRect(3, 3, 4, 4)
                        ),
                        .m_sourceId = sourceId,
                    },
                },
                {
                    annotation::test::page(
                        pageId,
                        "home",
                        {anchorId},
                        {awayId}
                    ),
                },
                {
                    annotation::RegressionCase{
                        annotation::RegressionSpec{
                            .m_id = annotation::test::regressionId(
                                k_regressionId
                            ),
                            .m_sourceId       = sourceId,
                            .m_classification = classification,
                            .m_expectation    = annotation::UnknownRegression{},
                        }
                    },
                }
            );
            REQUIRE(created.has_value());
            return *std::move(created);
        }

        // Two pages whose anchors differ, so a conversion that must pick a page
        // to authorize can be observed picking the recognizer's own page rather
        // than the first one. "battle" survives losing its required anchor
        // because it still forbids one.
        [[nodiscard]]
        auto twoPageDocument() -> annotation::AuthoringDocument
        {
            auto const fingerprint = annotation::test::fingerprint(8, 8, 96, 96);
            auto const sourceId    = annotation::test::sourceId(k_sourceId);
            auto const anchorId    = annotation::test::recognizerId(k_anchorId);
            auto const awayId      = annotation::test::recognizerId(k_awayId);
            auto const homeId      = annotation::test::pageId(k_pageId);
            auto const battleId    = annotation::test::pageId(k_secondPageId);
            auto const sourceHash  = annotation::sha256(
                std::span<std::byte const>{}
            );
            REQUIRE(sourceHash.has_value());
            auto source = annotation::AuthoringSource::create(
                annotation::AuthoringSourceSpec{
                    .m_id          = sourceId,
                    .m_contentHash = *sourceHash,
                    .m_fingerprint = fingerprint,
                    .m_provenance  = annotation::ImportedSourceProvenance{},
                }
            );
            REQUIRE(source.has_value());

            auto created = annotation::AuthoringDocument::create(
                annotation::test::projectId(),
                fingerprint,
                {*source},
                {
                    annotation::AuthoringRecognizerSpec{
                        .m_definition = annotation::test::recognizer(
                            fingerprint,
                            anchorId,
                            "home_marker",
                            annotation::AnnotationType::PageAnchor,
                            annotation::test::pixelRect(0, 0, 2, 2),
                            annotation::test::pixelRect(0, 0, 4, 4)
                        ),
                        .m_sourceId = sourceId,
                    },
                    annotation::AuthoringRecognizerSpec{
                        .m_definition = annotation::test::recognizer(
                            fingerprint,
                            awayId,
                            "battle_marker",
                            annotation::AnnotationType::PageAnchor,
                            annotation::test::pixelRect(4, 4, 2, 2),
                            annotation::test::pixelRect(3, 3, 4, 4)
                        ),
                        .m_sourceId = sourceId,
                    },
                },
                {
                    annotation::test::page(homeId, "home", {anchorId}),
                    annotation::test::page(
                        battleId,
                        "battle",
                        {awayId},
                        {anchorId}
                    ),
                },
                {}
            );
            REQUIRE(created.has_value());
            return *std::move(created);
        }

        [[nodiscard]]
        auto recognizerName(
            AuthoringEditHistory const& history,
            std::size_t index
        ) -> std::string
        {
            return history.draft().m_recognizers.at(index).m_name;
        }

        // Reads a recognizer out of a draft by identity rather than by position,
        // so a test states which recognizer it means instead of depending on the
        // catalog's ordering.
        [[nodiscard]]
        auto recognizerIn(
            AuthoringDraft const& draft,
            annotation::RecognizerId id
        ) -> EditableRecognizer
        {
            auto const found = std::ranges::find(
                draft.m_recognizers,
                id,
                &EditableRecognizer::m_id
            );
            REQUIRE(found != draft.m_recognizers.end());
            return *found;
        }

        [[nodiscard]]
        auto pageIn(
            AuthoringDraft const& draft,
            annotation::PageId id
        ) -> EditablePage
        {
            auto const found = std::ranges::find(
                draft.m_pages,
                id,
                &EditablePage::m_id
            );
            REQUIRE(found != draft.m_pages.end());
            return *found;
        }
    }

    TEST_CASE("becoming an action target authorizes a page in the same edit")
    {
        // A page anchor cannot authorize a page and an action target must, so
        // neither half of this change can be committed on its own.
        auto const awayId = annotation::test::recognizerId(k_awayId);
        auto const pageId = annotation::test::pageId(k_pageId);

        auto const retyped = retypeRecognizer(
            makeAuthoringDraft(variantDocument()),
            awayId,
            annotation::AnnotationType::ActionTarget
        );
        REQUIRE(retyped.has_value());
        REQUIRE(retyped->m_authorizedPage.has_value());
        CHECK(*retyped->m_authorizedPage == pageId);

        auto const changed = recognizerIn(retyped->m_draft, awayId);
        CHECK(changed.m_annotationType == annotation::AnnotationType::ActionTarget);
        REQUIRE(changed.m_allowedPageIds.size() == 1U);
        CHECK(changed.m_allowedPageIds.front() == pageId);

        // Only a page anchor may hold a signature role, so the recognizer is
        // withdrawn from the page it was forbidden on.
        auto const page = pageIn(retyped->m_draft, pageId);
        CHECK(page.m_forbidden.empty());
        CHECK(page.m_required.size() == 1U);
        CHECK(retyped->m_withdrawnRoles == 1U);
        CHECK(retyped->m_clearedAuthorizations == 0U);
        CHECK_FALSE(retyped->m_clearedClick);

        CHECK(buildAuthoringDocument(retyped->m_draft).has_value());
    }

    TEST_CASE("an action target is authorized on the page it anchored")
    {
        // The recognizer anchors the second page, so authorizing the first would
        // be arbitrary: it is known to appear on its own page and nowhere else.
        auto const awayId   = annotation::test::recognizerId(k_awayId);
        auto const homeId   = annotation::test::pageId(k_pageId);
        auto const battleId = annotation::test::pageId(k_secondPageId);

        auto const draft = makeAuthoringDraft(twoPageDocument());
        REQUIRE(draft.m_pages.front().m_id == homeId);

        auto const retyped = retypeRecognizer(
            draft,
            awayId,
            annotation::AnnotationType::ActionTarget
        );
        REQUIRE(retyped.has_value());
        REQUIRE(retyped->m_authorizedPage.has_value());
        CHECK(*retyped->m_authorizedPage == battleId);

        auto const changed = recognizerIn(retyped->m_draft, awayId);
        REQUIRE(changed.m_allowedPageIds.size() == 1U);
        CHECK(changed.m_allowedPageIds.front() == battleId);

        CHECK(buildAuthoringDocument(retyped->m_draft).has_value());
    }

    TEST_CASE("becoming a page anchor drops the click and the authorizations")
    {
        auto const actionId = annotation::test::recognizerId(k_actionId);

        auto const retyped = retypeRecognizer(
            makeAuthoringDraft(document()),
            actionId,
            annotation::AnnotationType::PageAnchor
        );
        REQUIRE(retyped.has_value());
        CHECK_FALSE(retyped->m_authorizedPage.has_value());

        auto const changed = recognizerIn(retyped->m_draft, actionId);
        CHECK(changed.m_annotationType == annotation::AnnotationType::PageAnchor);
        CHECK(changed.m_allowedPageIds.empty());
        CHECK_FALSE(changed.m_defaultClick.has_value());

        // The conversion is lossy in both fields, so both are reported.
        CHECK(retyped->m_clearedAuthorizations == 1U);
        CHECK(retyped->m_clearedClick);
        CHECK(retyped->m_withdrawnRoles == 0U);

        CHECK(buildAuthoringDocument(retyped->m_draft).has_value());
    }

    TEST_CASE("becoming an info region keeps the authorizations without the click")
    {
        auto const actionId = annotation::test::recognizerId(k_actionId);
        auto const pageId   = annotation::test::pageId(k_pageId);

        auto const retyped = retypeRecognizer(
            makeAuthoringDraft(document()),
            actionId,
            annotation::AnnotationType::InfoRegion
        );
        REQUIRE(retyped.has_value());
        CHECK_FALSE(retyped->m_authorizedPage.has_value());

        auto const changed = recognizerIn(retyped->m_draft, actionId);
        CHECK(changed.m_annotationType == annotation::AnnotationType::InfoRegion);
        REQUIRE(changed.m_allowedPageIds.size() == 1U);
        CHECK(changed.m_allowedPageIds.front() == pageId);
        CHECK_FALSE(changed.m_defaultClick.has_value());
        CHECK(retyped->m_clearedAuthorizations == 0U);
        CHECK(retyped->m_clearedClick);

        CHECK(buildAuthoringDocument(retyped->m_draft).has_value());
    }

    TEST_CASE("retyping the only recognizer a page names is refused")
    {
        // Withdrawing the anchor would leave the page naming nothing, which no
        // repair inside this recognizer can fix.
        auto const anchorId = annotation::test::recognizerId(k_anchorId);

        auto const retyped = retypeRecognizer(
            makeAuthoringDraft(document()),
            anchorId,
            annotation::AnnotationType::ActionTarget
        );
        REQUIRE_FALSE(retyped.has_value());
    }

    TEST_CASE("becoming an action target with no page to authorize is refused")
    {
        auto const anchorId = annotation::test::recognizerId(k_anchorId);

        // A freshly captured project: recognizers but no page yet.
        auto draft = makeAuthoringDraft(document());
        draft.m_pages.clear();
        for (auto& recognizer : draft.m_recognizers)
        {
            recognizer.m_allowedPageIds.clear();
        }

        auto const retyped = retypeRecognizer(
            std::move(draft),
            anchorId,
            annotation::AnnotationType::ActionTarget
        );
        REQUIRE_FALSE(retyped.has_value());
    }

    TEST_CASE("retyping to the type already held changes nothing")
    {
        auto const anchorId = annotation::test::recognizerId(k_anchorId);
        auto const original = document();

        auto const retyped = retypeRecognizer(
            makeAuthoringDraft(original),
            anchorId,
            annotation::AnnotationType::PageAnchor
        );
        REQUIRE(retyped.has_value());
        CHECK_FALSE(retyped->m_authorizedPage.has_value());

        auto const rebuilt = buildAuthoringDocument(retyped->m_draft);
        REQUIRE(rebuilt.has_value());
        CHECK(
            annotation::serializeAuthoringDocument(*rebuilt)
            == annotation::serializeAuthoringDocument(original)
        );
    }

    TEST_CASE("deleting a recognizer withdraws it from the pages that name it")
    {
        auto const awayId   = annotation::test::recognizerId(k_awayId);
        auto const battleId = annotation::test::pageId(k_secondPageId);

        auto const deleted = deleteRecognizer(
            makeAuthoringDraft(twoPageDocument()),
            awayId
        );
        REQUIRE(deleted.has_value());
        CHECK(deleted->m_withdrawnRoles == 1U);
        CHECK(deleted->m_draft.m_recognizers.size() == 1U);
        CHECK(pageIn(deleted->m_draft, battleId).m_required.empty());

        CHECK(buildAuthoringDocument(deleted->m_draft).has_value());
    }

    TEST_CASE("deleting the only recognizer a page names is refused")
    {
        // The page would be left identifying no screen, and only the author can
        // say whether the page goes too or another anchor takes over.
        auto const anchorId = annotation::test::recognizerId(k_anchorId);

        auto const deleted = deleteRecognizer(
            makeAuthoringDraft(document()),
            anchorId
        );
        REQUIRE_FALSE(deleted.has_value());
    }

    TEST_CASE("deleting a page clears it from the recognizers that authorize it")
    {
        // The action target authorizes the page and one other, so the deletion
        // leaves it authorized somewhere and may proceed.
        auto const actionId = annotation::test::recognizerId(k_actionId);
        auto const homeId   = annotation::test::pageId(k_pageId);
        auto const battleId = annotation::test::pageId(k_secondPageId);

        auto widened = makeAuthoringDraft(document());
        auto const spot = std::ranges::find(
            widened.m_recognizers,
            actionId,
            &EditableRecognizer::m_id
        );
        REQUIRE(spot != widened.m_recognizers.end());
        spot->m_allowedPageIds.emplace_back(battleId);
        // Two pages may not carry the same signature, so the second page forbids
        // the anchor the first requires.
        widened.m_pages.emplace_back(
            EditablePage{
                .m_id        = battleId,
                .m_name      = "battle",
                .m_required  = {},
                .m_forbidden = {annotation::test::recognizerId(k_anchorId)},
            }
        );
        // The document's regression expects the page under test to resolve, which
        // is refused on its own; this case is about the authorizations.
        REQUIRE(widened.m_regressions.size() == 1U);
        widened.m_regressions.at(0).m_expectation = annotation::UnknownRegression{};

        auto const deleted = deletePage(std::move(widened), homeId);
        REQUIRE(deleted.has_value());
        CHECK(deleted->m_clearedAuthorizations == 1U);
        CHECK(deleted->m_draft.m_pages.size() == 1U);
        CHECK(
            recognizerIn(deleted->m_draft, actionId).m_allowedPageIds.size() == 1U
        );
    }

    TEST_CASE("deleting an action target's only authorized page is refused")
    {
        auto const pageId = annotation::test::pageId(k_pageId);

        auto const deleted = deletePage(makeAuthoringDraft(document()), pageId);
        REQUIRE_FALSE(deleted.has_value());
    }

    TEST_CASE("deleting a page a regression expects to resolve is refused")
    {
        // The recorded expectation is the point of the case, so rewriting it
        // silently would destroy what the case was written to pin.
        auto const pageId = annotation::test::pageId(k_pageId);

        auto draft = makeAuthoringDraft(document());
        for (auto& recognizer : draft.m_recognizers)
        {
            recognizer.m_allowedPageIds.clear();
            recognizer.m_annotationType = annotation::AnnotationType::PageAnchor;
        }
        REQUIRE(draft.m_regressions.size() == 1U);
        draft.m_regressions.at(0).m_expectation = annotation::ResolvedRegression{
            .m_pageId = pageId,
        };

        auto const deleted = deletePage(std::move(draft), pageId);
        REQUIRE_FALSE(deleted.has_value());
    }

    TEST_CASE("deleting a source removes the regression cases recorded on it")
    {
        auto const sourceId = annotation::test::sourceId(k_sourceId);

        auto draft = makeAuthoringDraft(document());
        draft.m_recognizers.clear();
        draft.m_pages.clear();
        REQUIRE(draft.m_regressions.size() == 1U);

        auto const deleted = deleteSource(std::move(draft), sourceId);
        REQUIRE(deleted.has_value());
        CHECK(deleted->m_removedRegressions == 1U);
        CHECK(deleted->m_draft.m_sources.empty());
        CHECK(deleted->m_draft.m_regressions.empty());

        CHECK(buildAuthoringDocument(deleted->m_draft).has_value());
    }

    TEST_CASE("deleting a source still carrying recognizers is refused")
    {
        // A recognizer's rectangles are only meaningful against the image they
        // were drawn on, so the source cannot leave without them.
        auto const sourceId = annotation::test::sourceId(k_sourceId);

        auto const deleted = deleteSource(
            makeAuthoringDraft(document()),
            sourceId
        );
        REQUIRE_FALSE(deleted.has_value());
    }

    TEST_CASE("retyping a recognizer the draft does not hold is refused")
    {
        auto const strangerId = annotation::test::recognizerId(k_awayId);

        auto const retyped = retypeRecognizer(
            makeAuthoringDraft(document()),
            strangerId,
            annotation::AnnotationType::InfoRegion
        );
        REQUIRE_FALSE(retyped.has_value());
    }

    TEST_CASE("authoring draft preserves the complete canonical document")
    {
        auto const documents = std::vector<annotation::AuthoringDocument>{
            document(),
            variantDocument(),
        };
        for (auto const& original : documents)
        {
            auto const rebuilt = buildAuthoringDocument(
                makeAuthoringDraft(original)
            );
            REQUIRE(rebuilt.has_value());
            CHECK(
                annotation::serializeAuthoringDocument(*rebuilt)
                == annotation::serializeAuthoringDocument(original)
            );
        }
    }

    TEST_CASE("authoring history applies validated edits and supports undo and redo")
    {
        auto history = AuthoringEditHistory{document()};
        auto renamed = history.draft();

        renamed.m_recognizers.at(0).m_name = "renamed_marker";

        auto const applied = history.apply(renamed);
        REQUIRE(applied.has_value());
        CHECK(*applied);
        CHECK(recognizerName(history, 0) == "renamed_marker");
        CHECK(history.canUndo());
        CHECK_FALSE(history.canRedo());

        CHECK(history.undo());
        CHECK(recognizerName(history, 0) == "home_marker");
        CHECK_FALSE(history.canUndo());
        CHECK(history.canRedo());
        CHECK(
            annotation::serializeAuthoringDocument(history.document())
            == annotation::serializeAuthoringDocument(document())
        );

        CHECK(history.redo());
        CHECK(recognizerName(history, 0) == "renamed_marker");
        CHECK(history.canUndo());
        CHECK_FALSE(history.canRedo());
    }

    TEST_CASE("authoring history rejects invalid drafts without changing history")
    {
        auto history = AuthoringEditHistory{document()};
        auto renamed = history.draft();

        renamed.m_recognizers.at(0).m_name = "renamed_marker";
        REQUIRE(history.apply(renamed).has_value());
        REQUIRE(history.undo());
        REQUIRE(history.canRedo());

        auto invalid = history.draft();

        invalid.m_recognizers.at(0).m_name.clear();

        auto const applied = history.apply(invalid);
        REQUIRE_FALSE(applied.has_value());
        CHECK(recognizerName(history, 0) == "home_marker");
        CHECK_FALSE(history.canUndo());
        CHECK(history.canRedo());
        CHECK(history.redo());
        CHECK(recognizerName(history, 0) == "renamed_marker");
    }

    TEST_CASE("authoring history ignores identical edits and clears abandoned redo")
    {
        auto history = AuthoringEditHistory{document()};

        auto const unchanged = history.apply(history.draft());
        REQUIRE(unchanged.has_value());
        CHECK_FALSE(*unchanged);
        CHECK_FALSE(history.canUndo());

        auto first = history.draft();

        first.m_recognizers.at(0).m_name = "first_name";
        REQUIRE(history.apply(first).has_value());
        REQUIRE(history.undo());
        CHECK(history.canRedo());

        auto const pending = history.apply(history.draft());
        REQUIRE(pending.has_value());
        CHECK_FALSE(*pending);
        CHECK(history.canRedo());
        CHECK_FALSE(history.canUndo());

        auto branch = history.draft();

        branch.m_recognizers.at(0).m_name = "branch_name";
        REQUIRE(history.apply(branch).has_value());
        CHECK(recognizerName(history, 0) == "branch_name");
        CHECK_FALSE(history.canRedo());
    }

    TEST_CASE("authoring history replays redo entries in reverse order")
    {
        auto history = AuthoringEditHistory{document()};
        CHECK_FALSE(history.redo());

        auto first = history.draft();

        first.m_recognizers.at(0).m_name = "first_name";
        REQUIRE(history.apply(first).has_value());

        auto second = history.draft();

        second.m_recognizers.at(0).m_name = "second_name";
        REQUIRE(history.apply(second).has_value());

        REQUIRE(history.undo());
        REQUIRE(history.undo());
        CHECK(recognizerName(history, 0) == "home_marker");

        CHECK(history.redo());
        CHECK(recognizerName(history, 0) == "first_name");
        CHECK(history.redo());
        CHECK(recognizerName(history, 0) == "second_name");
        CHECK_FALSE(history.redo());
    }

    TEST_CASE("authoring history retains the configured undo boundary")
    {
        auto history = AuthoringEditHistory{document()};
        for (
            auto index = std::size_t{0};
            index < k_maximumAuthoringUndoEntries + 1U;
            ++index
        )
        {
            auto next = history.draft();

            next.m_recognizers.at(0).m_name = "marker_" + std::to_string(index);

            auto const applied = history.apply(next);
            REQUIRE(applied.has_value());
            REQUIRE(*applied);
        }

        for (
            auto index = std::size_t{0};
            index < k_maximumAuthoringUndoEntries;
            ++index
        )
        {
            REQUIRE(history.undo());
        }
        CHECK_FALSE(history.undo());
        CHECK(recognizerName(history, 0) == "marker_0");
    }
}
