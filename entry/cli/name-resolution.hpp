#pragma once

#include <core/error/result.hpp>

#include <annotation/catalog.hpp>

#include <string_view>

namespace uf::cli
{
    // Resolves a page recognizer NAME to its PageId against the loaded catalog.
    // An unknown name fails InvalidResource with a message listing every page
    // name the catalog defines, so a caller can correct the invocation.
    [[nodiscard]]
    auto resolvePageName(
        annotation::RecognitionCatalog const& catalog,
        std::string_view name
    ) -> Result<annotation::PageId>;

    // Resolves an action target NAME to its RecognizerId. Only recognizers whose
    // annotation type is ActionTarget are eligible; an unknown or non-action name
    // fails InvalidResource with a message listing every action target name.
    [[nodiscard]]
    auto resolveActionName(
        annotation::RecognitionCatalog const& catalog,
        std::string_view name
    ) -> Result<annotation::RecognizerId>;
}
