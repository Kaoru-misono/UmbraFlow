#include "ocr.hpp"

#include <core/types/integer.hpp>

#include <domain/space.hpp>

#include <json/value.hpp>

#include <string>
#include <utility>
#include <vector>

namespace uf::cli
{
    namespace
    {
        [[nodiscard]] auto ofCount(uint32 value) -> json::Value
        {
            return json::Value::ofNumber(static_cast<double>(value));
        }

        // x, y, width, height rather than the two corners PixelRect also
        // carries: a consumer of this document draws or crops with it, and the
        // right and bottom edges are the same four numbers restated.
        [[nodiscard]] auto rectValue(PixelRect const& bounds) -> json::Value
        {
            auto members = std::vector<json::Member>{};
            members.emplace_back("x", ofCount(bounds.x()));
            members.emplace_back("y", ofCount(bounds.y()));
            members.emplace_back("width", ofCount(bounds.width()));
            members.emplace_back("height", ofCount(bounds.height()));
            return json::Value::ofObject(std::move(members));
        }
    }

    auto formatImageText(ImageText const& text) -> std::string
    {
        auto lines = std::vector<json::Value>{};
        lines.reserve(text.lines.size());
        for (auto const& line : text.lines)
        {
            auto members = std::vector<json::Member>{};
            members.emplace_back("text", json::Value::ofString(line.text));
            members.emplace_back("rect", rectValue(line.bounds));
            members.emplace_back("confidenceBp", ofCount(line.confidenceBp));
            lines.emplace_back(json::Value::ofObject(std::move(members)));
        }

        auto extent = std::vector<json::Member>{};
        extent.emplace_back("width", ofCount(text.width));
        extent.emplace_back("height", ofCount(text.height));

        auto document = std::vector<json::Member>{};
        document.emplace_back("image", json::Value::ofObject(std::move(extent)));
        document.emplace_back("lines", json::Value::ofArray(std::move(lines)));

        return json::canonicalBytes(json::Value::ofObject(std::move(document)));
    }
}
