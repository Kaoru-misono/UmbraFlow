#include "../annotation/test-helpers.hpp"

#include <authoring-edit.hpp>

#include <annotation/authoring-document.hpp>
#include <annotation/content-hash.hpp>

#include <core/types/integer.hpp>

#include <doctest/doctest.h>

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

        [[nodiscard]]
        auto recognizerName(
            AuthoringEditHistory const& history,
            std::size_t index
        ) -> std::string
        {
            return history.draft().m_recognizers.at(index).m_name;
        }
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
