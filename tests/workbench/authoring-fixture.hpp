#pragma once

#include "../annotation/test-helpers.hpp"

#include <annotation/authoring-document.hpp>
#include <annotation/capabilities.hpp>
#include <annotation/catalog.hpp>
#include <annotation/resource.hpp>
#include <annotation/appearance.hpp>

#include <core/types/integer.hpp>

#include <domain/space.hpp>

#include <doctest/doctest.h>

#include <optional>
#include <string>
#include <utility>
#include <vector>

// The element shapes every workbench test needs, in the vocabulary the
// workbench itself authors: one element, one appearance, one capability. The
// model allows a set and several appearances; the workbench draws a rectangle
// and picks one use for it, so these three cover its whole authoring surface
// and a test that needs more builds it through annotation::test directly.
namespace uf::workbench::test
{
    // The appearance name the authoring layer mints, spelled here so a fixture
    // and the code under test agree on it without either guessing.
    inline auto appearance(
        annotation::SourceId sourceId,
        PixelRect templateRect,
        uint32 basisPoints = 9'000
    ) -> std::vector<annotation::Appearance>
    {
        auto appearances = std::vector<annotation::Appearance>{};
        appearances.emplace_back(
            annotation::test::appearance(
                "default",
                sourceId,
                templateRect,
                annotation::test::threshold(basisPoints)
            )
        );
        return appearances;
    }

    // Pixels a page can be recognised by.
    inline auto markElement(
        annotation::ProjectFingerprint fingerprint,
        annotation::ElementId id,
        std::string name,
        annotation::SourceId sourceId,
        PixelRect templateRect,
        PixelRect searchRoi
    ) -> annotation::Element
    {
        return annotation::test::element(
            fingerprint,
            id,
            std::move(name),
            annotation::test::capabilities(annotation::Identify{}),
            searchRoi,
            appearance(sourceId, templateRect)
        );
    }

    // Pixels an action can be delivered to.
    inline auto clickableElement(
        annotation::ProjectFingerprint fingerprint,
        annotation::ElementId id,
        std::string name,
        annotation::SourceId sourceId,
        PixelRect templateRect,
        PixelRect searchRoi,
        std::optional<annotation::TemplateOffset> clickOffset = std::nullopt
    ) -> annotation::Element
    {
        return annotation::test::element(
            fingerprint,
            id,
            std::move(name),
            annotation::test::capabilities(
                std::nullopt,
                annotation::Interact{.clickOffset = clickOffset}
            ),
            searchRoi,
            appearance(sourceId, templateRect)
        );
    }

    // Pixels a task reads text out of. It still carries an appearance here: the
    // workbench seeds one for every rectangle it draws, and an element with no
    // appearance is the separate page-located case.
    inline auto readableElement(
        annotation::ProjectFingerprint fingerprint,
        annotation::ElementId id,
        std::string name,
        annotation::SourceId sourceId,
        PixelRect templateRect,
        PixelRect searchRoi
    ) -> annotation::Element
    {
        return annotation::test::element(
            fingerprint,
            id,
            std::move(name),
            annotation::test::capabilities(
                std::nullopt,
                std::nullopt,
                annotation::Read{}
            ),
            searchRoi,
            appearance(sourceId, templateRect)
        );
    }

    // What a page's reference exercises, for the two cases annotation::test does
    // not already name.
    inline auto reads() -> annotation::ExercisedCapabilities
    {
        return annotation::test::exercised(
            std::nullopt,
            std::nullopt,
            annotation::ExercisedRead{}
        );
    }
}
