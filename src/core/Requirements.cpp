#include "core/Requirements.h"

#include <string>
#include <vector>

namespace eluvian
{

Json ItemRequirements(std::string_view answer)
{
    std::vector<std::string_view> pieces;
    std::size_t from = 0;
    while (from <= answer.size())
    {
        const auto semicolon = answer.find(';', from);
        if (semicolon == std::string_view::npos)
        {
            pieces.push_back(answer.substr(from));
            break;
        }
        pieces.push_back(answer.substr(from, semicolon - from));
        from = semicolon + 1;
    }
    // As DAExternalCommands.GetItemRequirements reads them: a pair while
    // two pieces are left, "1" met and anything else not.
    Json requirements = Json::array();
    for (std::size_t i = 0; i + 1 < pieces.size(); i += 2)
    {
        requirements.push_back(Json{{"met", pieces[i] == "1"}, {"text", std::string(pieces[i + 1])}});
    }
    return requirements;
}

} // namespace eluvian
