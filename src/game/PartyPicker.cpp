// The party picker's steps through DAOLib's dao::partypicker (DAOLib #259,
// e3ec1e1, lib-agent-3, 2026-10-24): who it offers and in what state, a
// follower chosen or let go as a click on them does, Remove All and
// Accept, each refused where the picker refuses the click. core's party
// asks these, unlisted, where it swept the cursor across the picker's
// scene until the name under it was the follower's (#25, #182).
#include "game/Shared.h"

#include "dao/PartyPicker.h"

namespace eluvian::game
{

namespace
{

namespace picker = dao::partypicker;

Json Taken(picker::Refusal refusal)
{
    if (refusal == picker::Refusal::None)
    {
        return Json{{"taken", true}};
    }
    return Json{{"taken", false},
                {"refusal", Label(refusal, {{picker::Refusal::NotShown, "not shown"},
                                            {picker::Refusal::ReadOnly, "read only"},
                                            {picker::Refusal::NotOffered, "not offered"},
                                            {picker::Refusal::Locked, "locked"},
                                            {picker::Refusal::AlreadySo, "already so"},
                                            {picker::Refusal::PartyFull, "party full"}})}};
}

ToolSpec Step(std::string name, std::string description, Json properties, const std::vector<std::string> &required)
{
    ToolSpec spec = Spec(std::move(name), std::move(description), std::move(properties), required);
    spec.listed = false;
    return spec;
}

const Json kIdProperty = {{"type", "string"}, {"description", "The follower, by id, from party.entries."}};

} // namespace

void AddPartyPicker(Bench &bench)
{
    bench.Tools().Add(
        Read("party.entries",
             "The creatures the party picker shows, in its order: each one's `id`, `name` and `tag`, its "
             "`state` (chosen, available, unavailable, suspended, lockedActive), its `slot` among the four "
             "chosen (-1 for none) and whether it is the `hero`. Empty until the picker is done showing.",
             Json::object(), {}, false),
        [](const Json &) -> ToolResult {
            Json entries = Json::array();
            for (const picker::Entry &entry : picker::Entries())
            {
                Json json = Brief(entry.creature);
                json["state"] = Label(entry.state, {{picker::State::Chosen, "chosen"},
                                                    {picker::State::Available, "available"},
                                                    {picker::State::Unavailable, "unavailable"},
                                                    {picker::State::Suspended, "suspended"},
                                                    {picker::State::LockedActive, "lockedActive"}});
                json["slot"] = entry.slot;
                json["hero"] = entry.hero;
                entries.push_back(std::move(json));
            }
            return Json{{"entries", std::move(entries)}};
        });

    bench.Tools().Add(Step("party.choose", "Chooses a follower as a click on them does: `taken`, or the `refusal`.",
                           Json{{"id", kIdProperty}}, {"id"}),
                      [](const Json &raw) -> ToolResult {
                          const Args args(raw, "party.choose");
                          return Taken(picker::Choose(args.Id("id", "a follower, from party.entries")));
                      });

    bench.Tools().Add(Step("party.release",
                           "Lets a chosen follower go as a click on them does: `taken`, or the `refusal`.",
                           Json{{"id", kIdProperty}}, {"id"}),
                      [](const Json &raw) -> ToolResult {
                          const Args args(raw, "party.release");
                          return Taken(picker::Release(args.Id("id", "a follower, from party.entries")));
                      });

    bench.Tools().Add(Step("party.release_all",
                           "Lets every chosen follower go but the hero and those the story keeps, as Remove All does.",
                           Json::object(), {}),
                      [](const Json &) -> ToolResult { return Taken(picker::ReleaseAll()); });

    bench.Tools().Add(Step("party.accept",
                           "Accepts the party as the picker's button does and closes it; the party changes a frame "
                           "or more after.",
                           Json::object(), {}),
                      [](const Json &) -> ToolResult { return Taken(picker::Accept()); });
}

} // namespace eluvian::game
