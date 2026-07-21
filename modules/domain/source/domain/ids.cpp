#include "ids.hpp"

#include <core/text/utf8.hpp>

#include <string>
#include <utility>

namespace uf
{
    Label::Label(std::string value) noexcept
        : m_value{std::move(value)}
    {
    }

    auto Label::create(std::string value) -> Result<Label>
    {
        if (!isValidUtf8(value))
        {
            return fail(
                AutomationErrorKind::InternalInvariant,
                "label must contain valid UTF-8"
            );
        }

        return Label{std::move(value)};
    }

    auto Label::value() const noexcept -> std::string const& { return m_value; }

    auto TargetGeneration::next() const -> Result<TargetGeneration>
    {
        auto const nextGeneration = m_generation.next();
        if (!nextGeneration)
        {
            return fail(
                AutomationErrorKind::InternalInvariant,
                "target generation " + std::to_string(value()) + " cannot be incremented"
            );
        }

        return TargetGeneration{*nextGeneration};
    }
}
