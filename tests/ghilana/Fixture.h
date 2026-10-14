// A game install made in a test, laid out as the GOG install is
// (dev/GHILANA.md, "The game on disk"), small: the core's archives with data
// tables, creature templates and a texture at two qualities, a patch of a
// table, its talk tables in two languages, the campaign's talk table, an
// add-in whose archive is enciphered, another not enabled, a DAOLib plugin's
// override folder adding to a table and overriding the texture, and a
// Documents folder with AddIns.xml and a loose override. Its copies are in
// the game's order (DAOLib's dev/FILES.md): Documents' override first, then
// the plugin's, the core's patch, its textures, its data.
#pragma once

#include "Build.h"

#include "ghilana/Ghilana.h"

#include <dao/core/GdaHash.h>

#include <memory>

namespace ghtest
{

inline std::vector<std::uint32_t> Hashes(const std::vector<std::string> &names)
{
    std::vector<std::uint32_t> out;
    out.reserve(names.size());
    for (const auto &name : names)
    {
        out.push_back(dao::core::GdaColumnHash(name));
    }
    return out;
}

// A creature template: its appearance, tag, name's string id, conversation,
// and an inventory list of one item.
inline Bytes Creature(std::uint32_t appearance, std::string_view tag, std::uint32_t nameStrref,
                      std::string_view conversation)
{
    Gff3 out("UTC");
    const std::uint32_t t = out.ExoString(tag);
    const std::uint32_t n = out.LocString(nameStrref);
    const std::uint32_t c = out.ResRef(conversation);
    const std::uint32_t r = out.ResRef("gen_im_wep_axe");
    // As the game's: the appearance in a struct of its own.
    out.Struct(
        0xFFFFFFFF,
        {{"Appearance Data", 14, 1}, {"Tag", 10, t}, {"LocName", 12, n}, {"Conversation", 11, c}, {"ItemList", 15, 0}});
    out.Struct(0, {{"Appearance_Type", 4, appearance}, {"Gender", 0, 1}});
    const std::uint32_t inventory = out.Struct(0, {{"InventoryRes", 11, r}});
    out.List({inventory});
    return out.Done();
}

struct Install
{
    TempFolder root;
    std::filesystem::path game = root.path / L"game";
    std::filesystem::path documents = root.path / L"documents";

    Install()
    {
        const std::vector<std::string> sheetColumns{"ID", "Label", "Worksheet"};
        const std::vector<std::string> abiColumns{"ID", "Label", "Cost"};
        root.Write(
            L"game/packages/core/data/2da.erf",
            Erf({{"m2da_base.gda", Gda(sheetColumns,
                                       {{1, std::string("Abilities"), std::string("ABI_")},
                                        {7, std::string("Portraits"), std::string("portraits")}},
                                       Hashes(sheetColumns))},
                 {"2da_base.gda",
                  Gda(sheetColumns, {{0, std::string("Gender"), std::string("gender")}}, Hashes(sheetColumns))},
                 {"abi_base.gda", Gda(abiColumns, {{1, std::string("Flurry"), 2.0F}, {2, std::string("Dual"), 3.0F}},
                                      Hashes(abiColumns))}}));
        root.Write(L"game/packages/core/data/designercreatures.erf",
                   Erf({{"ogre.utc", Creature(80, "ogre", 100, "ogre_talk")},
                        {"hurlock.utc", Creature(81, "hurlock", 101, "")},
                        {"ogre_talk.dlg", FromText("not read")}}));
        root.Write(L"game/packages/core/data/talktables/core_en-us.tlk",
                   Tlk({{7, "End turn"}, {100, "Ogre"}, {101, "Hurlock"}}));
        root.Write(L"game/packages/core/data/talktables/core_fr-fr.tlk", Tlk({{100, "Ogre FR"}}));
        root.Write(L"game/packages/core/textures/high/texturepack.erf",
                   Erf({{"ogre_d.dds", Dds(8, 8, {255, 0, 0, 255}, {0, 255, 0, 255})}}));
        root.Write(L"game/packages/core/textures/medium/texturepack.erf",
                   Erf({{"ogre_d.dds", Dds(2, 2, {9, 9, 9, 255}, {9, 9, 9, 255})}}));
        root.Write(L"game/packages/core/patch/patch001.erf",
                   Erf({{"abi_base.gda",
                         Gda(abiColumns, {{1, std::string("Flurry patched"), 2.0F}, {2, std::string("Dual"), 3.0F}},
                             Hashes(abiColumns))}}));
        root.Write(L"game/Addins/dao_prc_off/core/override/ogre.utc", Creature(99, "off", 100, ""));
        // Two files of the abilities in one folder giving one ID: the game keeps
        // the first by name, whatever order they were written in.
        root.Write(L"game/packages/core/override/abi_ovr_b.gda",
                   Gda(abiColumns, {{7, std::string("Over B"), 1.0F}}, Hashes(abiColumns)));
        root.Write(L"game/packages/core/override/abi_ovr_a.gda",
                   Gda(abiColumns, {{7, std::string("Over A"), 1.0F}}, Hashes(abiColumns)));
        root.Write(L"game/Addins/dao_prc_off/core/override/off_only.uti", FromText("only here"));
        root.Write(L"game/packages/core/data/sound.fsb", FromText("audio"));
        root.Write(L"game/modules/single player/data/talktables/singleplayer_en-us.tlk", Tlk({{5000, "Hello"}}));
        root.Write(L"game/Addins/dao_prc_cp_1/core/data/2da.erf",
                   Erf({{"abi_cp1.gda", FromText("ciphered bytes")}}, true, 2U << 4));
        root.Write(L"game/Addins/dao_prc_cp_1/manifest.xml", FromText("<Manifest/>"));
        root.Write(
            L"game/bin_ship/daolib/plugins/mymod/override/abi_mymod.gda",
            Gda(abiColumns, {{2, std::string("Dual Mod"), 4.0F}, {3, std::string("New"), 1.0F}}, Hashes(abiColumns)));
        root.Write(L"game/bin_ship/daolib/plugins/mymod/override/ogre_d.dds",
                   Dds(4, 4, {0, 0, 255, 128}, {0, 0, 0, 255}));
        root.Write(
            L"documents/Settings/AddIns.xml",
            FromText("<?xml version=\"1.0\"?>\n<AddInsList>\n"
                     "<AddInItem UID=\"DAO_PRC_CP_1\" Name=\"PRC_CP1_Cailan\" ExtendedModuleUID=\"Single Player\" "
                     "Priority=\"200\" Enabled=\"1\" State=\"2\" BioWare=\"1\">\n"
                     "<Title DefaultText=\"Return to Ostagar\"/></AddInItem>\n"
                     "<AddInItem UID=\"DAO_PRC_GONE\" Name=\"Gone\" Priority=\"5\" Enabled=\"0\"></AddInItem>\n"
                     "<AddInItem UID=\"DAO_PRC_OFF\" Name=\"Off\" Priority=\"7\" Enabled=\"0\"></AddInItem>\n"
                     "</AddInsList>\n"));
        root.Write(L"documents/packages/core/override/hurlock.utc", Creature(82, "hurlock_mod", 101, ""));
        // A package a campaign loads ahead of the core, as Awakening's .cif
        // names core_ep1 (#129); the main campaign's names none.
        root.Write(L"game/packages/core_ep1/data/designercreatures.erf",
                   Erf({{"hurlock.utc", Creature(83, "hurlock_ep1", 101, "")},
                        {"golem.utc", Creature(84, "golem", 101, "")}}));
        root.Write(L"game/modules/single player/singleplayer.cif", Cif({}));
        root.Write(L"game/Addins/dao_prc_ep_1/module/dao_prc_ep_1.cif", Cif({"core_ep1"}));
    }

    // A campaign's .cif whose CampaignCIFPackagesList names these packages
    // (DAOLib's tests/test_install.cpp writes it the same way).
    static Bytes Cif(const std::vector<std::string> &packages)
    {
        constexpr std::uint32_t kPackagesList = 17124;
        constexpr std::uint16_t kString = 14;
        constexpr std::uint16_t kList = 0x8000;
        Gff4 cif("CIF ", "V0.1", {{"CIF ", 4, {{kPackagesList, kString, kList, 0}}}});
        const std::uint32_t top = cif.Reserve(4);
        const std::uint32_t list = cif.Reserve(4 + (4 * packages.size()));
        cif.Set32(list, static_cast<std::uint32_t>(packages.size()));
        for (std::size_t i = 0; i < packages.size(); ++i)
        {
            cif.Set32(list + 4 + static_cast<std::uint32_t>(4 * i), cif.String(packages[i]));
        }
        cif.Set32(top, list);
        return cif.bytes;
    }

    eluvian::ghilana::Paths Paths(std::string campaign = "Single Player") const
    {
        eluvian::ghilana::Paths paths;
        paths.game = game;
        paths.documents = documents;
        paths.campaign = std::move(campaign);
        return paths;
    }
};

// The install, ghilana over it, and its tools on a bench.
struct Served
{
    Install install;
    eluvian::ghilana::Ghilana ghilana;
    eluvian::Bench bench;

    explicit Served(const std::string &campaign = "Single Player")
        : ghilana{install.Paths(campaign), {}, install.root.path / L"cache", "test"}
    {
        eluvian::ghilana::RegisterTools(bench, ghilana);
    }

    eluvian::Json Call(std::string_view tool, const eluvian::Json &args = eluvian::Json::object())
    {
        return bench.Invoke(tool, args).data;
    }
};

} // namespace ghtest
