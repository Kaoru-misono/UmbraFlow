#include "name-resolution.hpp"

#include <domain/error.hpp>

#include <format>
#include <string>
#include <string_view>
#include <vector>

namespace uf::cli
{
    namespace
    {
        // Builds a comma-separated, quoted list of names for an error message.
        // Names is a forward range of ResourceName-yielding elements.
        template <typename Names>
        [[nodiscard]]
        auto describeNames(Names const& names) -> std::string
        {
            auto joined = std::string{};
            for (auto const& name : names)
            {
                if (!joined.empty())
                {
                    joined += ", ";
                }
                joined += '"';
                joined += name;
                joined += '"';
            }
            if (joined.empty())
            {
                return "(none)";
            }
            return joined;
        }
    }

    auto resolvePageName(
        annotation::RecognitionCatalog const& catalog,
        std::string_view name
    ) -> Result<annotation::PageId>
    {
        auto available = std::vector<std::string>{};
        for (auto const& page : catalog.pages())
        {
            auto const pageName = page.name();
            if (pageName.value() == name)
            {
                return page.id();
            }
            available.emplace_back(pageName.value());
        }
        return fail(
            AutomationErrorKind::InvalidResource,
            std::format(
                "unknown page name \"{}\"; available pages: {}",
                name,
                describeNames(available)
            )
        );
    }

    auto resolveActionName(
        annotation::RecognitionCatalog const& catalog,
        std::string_view name
    ) -> Result<annotation::RecognizerId>
    {
        auto available = std::vector<std::string>{};
        for (auto const& recognizer : catalog.recognizers())
        {
            if (recognizer.annotationType() != annotation::AnnotationType::ActionTarget)
            {
                continue;
            }
            auto const recognizerName = recognizer.name();
            if (recognizerName.value() == name)
            {
                return recognizer.id();
            }
            available.emplace_back(recognizerName.value());
        }
        return fail(
            AutomationErrorKind::InvalidResource,
            std::format(
                "unknown action name \"{}\"; available action targets: {}",
                name,
                describeNames(available)
            )
        );
    }
}
