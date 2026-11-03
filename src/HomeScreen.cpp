// The whole file needs a core with home screens; mod-playerhousing.cmake finds out.
#ifdef PLAYERHOUSING_CORE_HAS_HOME_SCREEN

#include "PlayerHousingMgr.h"

#include "AllCommandScript.h"
#include "CharacterCache.h"
#include "Chat.h"
#include "CommandScript.h"
#include "Config.h"
#include "DatabaseEnv.h"
#include "DBCStores.h"
#include "Map.h"
#include "MapMgr.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Opcodes.h"
#include "PetDefines.h"
#include "Player.h"
#include "PlayerScript.h"
#include "ScriptMgr.h"
#include "StringConvert.h"
#include "StringFormat.h"
#include "TemporarySummon.h"
#include "Tokenize.h"
#include "World.h"
#include "WorldPacket.h"
#include "WorldScript.h"
#include "WorldSession.h"

#include <cmath>
#include <mutex>
#include <string_view>
#include <unordered_map>

using namespace Acore::ChatCommands;

// The home screen: after logging in, an account with a viewer lands in the world at its plot
// instead of on character select, and picks a character from there. The viewer is a character
// row (characters.home_viewer, a core column) nobody plays: invisible, rooted, in a private phase
// of its own, and left out of the player hooks by the core. The core's character list and the
// character select screen do the rest: see WorldSession::HandleCharEnum.
//
// This works with PlayerHousing.Enable off, which leaves the rest of the module asleep and its
// world SQL unneeded. The addon (HomeScreen) sends the .home commands over the addon channel and
// reads the character list as addon whispers with the prefix HOMESCREEN, tab separated.
//
// The account's favourites stand at the plot as figures: the same kind of creature a mannequin is,
// shown only to its viewer, dressed from characters.equipmentCache the way the character list is.
// They come when the viewer logs in and go when it's gone. Their one row of world SQL is in data/sql.

namespace
{
    constexpr char const* ADDON_PREFIX = "HOMESCREEN\t";
    constexpr uint32 INVISIBLE_DISPLAY = 11686;    // the invisible stalker's model
    constexpr uint32 FIGURE_ENTRY = 900204;        // data/sql/db-world
    constexpr uint32 SWEEP_MS = 1000;              // how often to look for figures whose viewer has gone
    constexpr float PET_FURTHER = 0.9f;            // where a pet stands from its owner, in yards, as in
    constexpr float PET_ASIDE = 0.6f;              // the character select camp: behind them, to the outside
    constexpr float MAX_SPOT_YARDS = 40.0f;        // a figure further off than this is a typing mistake

    struct Plot
    {
        uint32 map;
        float x, y, z, o;
    };

    // One plot for everybody for now. The default is Fenris Isle, in Silverpine Forest.
    Plot ConfiguredPlot()
    {
        return Plot{ sConfigMgr->GetOption<uint32>("PlayerHousing.Home.Map", 0),
            sConfigMgr->GetOption<float>("PlayerHousing.Home.X", 736.94f),
            sConfigMgr->GetOption<float>("PlayerHousing.Home.Y", 727.85f),
            sConfigMgr->GetOption<float>("PlayerHousing.Home.Z", 36.55f),
            sConfigMgr->GetOption<float>("PlayerHousing.Home.O", 0.0f) };
    }

    std::string PlotUpdate(Plot const& plot)
    {
        return Acore::StringFormat("map = {}, position_x = {}, position_y = {}, position_z = {}, orientation = {}, "
            "instance_id = 0, instance_mode_mask = 0, transguid = 0, trans_x = 0, trans_y = 0, trans_z = 0, trans_o = 0",
            plot.map, plot.x, plot.y, plot.z, plot.o);
    }

    void SendToAddon(Player* player, std::string const& text)
    {
        // A whisper to themselves, the way mod-playerhousing's window hears from the server.
        WorldPacket data;
        ChatHandler::BuildChatPacket(data, CHAT_MSG_WHISPER, LANG_ADDON, player->GetGUID(), player->GetGUID(), ADDON_PREFIX + text, 0);
        player->SendDirectMessage(&data);
    }

    struct Spot
    {
        float forward, right;   // yards from where the viewer stands, by the way it faces
    };

    // "forward right, forward right, ...": one spot per figure. Empty if any of it can't be read.
    std::vector<Spot> ParseSpots(std::string_view text)
    {
        std::vector<Spot> spots;
        for (std::string_view spot : Acore::Tokenize(text, ',', false))
        {
            std::vector<std::string_view> numbers = Acore::Tokenize(spot, ' ', false);
            Optional<float> forward = numbers.size() == 2 ? Acore::StringTo<float>(numbers[0]) : std::nullopt;
            Optional<float> right = numbers.size() == 2 ? Acore::StringTo<float>(numbers[1]) : std::nullopt;
            if (!forward || !right || !std::isfinite(*forward) || !std::isfinite(*right)
                || std::abs(*forward) > MAX_SPOT_YARDS || std::abs(*right) > MAX_SPOT_YARDS)
                return {};
            spots.push_back(Spot{ *forward, *right });
        }
        return spots;
    }

    // Set by .home spots until the server restarts, for trying a layout out in the game. Written on
    // the world thread only, while no map thread is running.
    std::string TriedSpots;

    std::string SpotsText()
    {
        return !TriedSpots.empty() ? TriedSpots
            : sConfigMgr->GetOption<std::string>("PlayerHousing.Home.FigureSpots", "3 -1, 4.2 -3.5, 4.2 1.5, 5.4 -6, 5.4 4");
    }

    Position SpotPosition(Plot const& plot, Spot const& spot, Map* map, uint32 phaseMask)
    {
        float const x = plot.x + spot.forward * std::cos(plot.o) + spot.right * std::sin(plot.o);
        float const y = plot.y + spot.forward * std::sin(plot.o) - spot.right * std::cos(plot.o);
        float const ground = map->GetHeight(phaseMask, x, y, plot.z + 5.0f, true, 40.0f);
        // Everyone faces the viewer, which is where the camera looks from.
        return Position(x, y, ground > INVALID_HEIGHT ? ground : plot.z, std::atan2(plot.y - y, plot.x - x));
    }

    // Who stands where, for the addon and for taking them away again. Behind a lock because a session's
    // query callbacks run on its map's thread as well as the world thread (WorldSession::Update).
    struct FigureRow
    {
        ObjectGuid guid;
        std::string name;   // whose figure, or whose pet
    };
    struct ViewerFigures
    {
        uint32 mapId{0};
        std::vector<FigureRow> rows;
    };
    std::unordered_map<ObjectGuid::LowType, ViewerFigures> Figures;
    std::mutex FiguresLock;

    // On the world thread, or on the thread of the map the figures are on.
    void RemoveFigures(ObjectGuid::LowType viewerGuid)
    {
        ViewerFigures figures;
        {
            std::lock_guard<std::mutex> guard(FiguresLock);
            auto itr = Figures.find(viewerGuid);
            if (itr == Figures.end())
                return;
            figures = std::move(itr->second);
            Figures.erase(itr);
        }
        Map* map = sMapMgr->FindBaseMap(figures.mapId);
        for (FigureRow const& row : figures.rows)
            sPlayerHousingMgr->RemoveFigure(map, row.guid);
    }

    // Built by hand instead of with Map::SummonCreature, which would put the figure in the world
    // before there's anything to dress: the first thing its viewer gets is the finished figure, and
    // only the viewer ever gets it.
    TempSummon* NewFigure(Player* viewer, Position const& at, uint32 displayId)
    {
        Map* map = viewer->GetMap();
        TempSummon* figure = new TempSummon(nullptr, viewer->GetGUID());
        if (!figure->Create(map->GenerateLowGuid<HighGuid::Unit>(), map, viewer->GetPhaseMask(), FIGURE_ENTRY, 0,
            at.GetPositionX(), at.GetPositionY(), at.GetPositionZ(), at.GetOrientation()))
        {
            delete figure;
            return nullptr;
        }
        figure->SetHomePosition(at);
        figure->InitStats(0);
        figure->SetVisibleBySummonerOnly(true);
        figure->SetDisplayId(displayId);
        figure->SetNativeDisplayId(displayId);
        figure->SetReactState(REACT_PASSIVE);
        return figure;
    }

    bool PlaceFigure(Player* viewer, TempSummon* figure, std::string const& name)
    {
        ObjectGuid const guid = figure->GetGUID();
        if (!viewer->GetMap()->AddToMap(figure->ToCreature()))
        {
            delete figure;
            sPlayerHousingMgr->RemoveFigure(nullptr, guid);
            return false;
        }
        figure->InitSummon();
        figure->SetControlled(true, UNIT_STATE_ROOT);
        if (sConfigMgr->GetOption<bool>("PlayerHousing.Home.FigureFreeze", false))
            figure->AddAura(Housing::SPELL_FREEZE_ANIM, figure);

        std::lock_guard<std::mutex> guard(FiguresLock);
        ViewerFigures& figures = Figures[viewer->GetGUID().GetCounter()];
        figures.mapId = viewer->GetMapId();
        figures.rows.push_back(FigureRow{ guid, name });
        return true;
    }

    // One row of the query in AskForFigures:
    // 0 name, 1 race, 2 class, 3 gender, 4 skin, 5 face, 6 hairStyle, 7 hairColor, 8 facialStyle,
    // 9 playerFlags, 10 equipmentCache, 11 pet model
    void SpawnFigure(Player* viewer, Field* fields, Plot const& plot, Spot const& spot)
    {
        std::string const name = fields[0].Get<std::string>();
        uint8 const race = fields[1].Get<uint8>();
        uint8 const playerClass = fields[2].Get<uint8>();
        uint8 const gender = fields[3].Get<uint8>();
        uint32 const playerFlags = fields[9].Get<uint32>();

        ChrRacesEntry const* raceEntry = sChrRacesStore.LookupEntry(race);
        if (!raceEntry)
            return;

        PlayerHousingMgr::MannequinLook look;
        look.displayId = gender == GENDER_FEMALE ? raceEntry->model_f : raceEntry->model_m;
        look.race = race;
        look.gender = gender;
        look.classId = playerClass;
        look.skin = fields[4].Get<uint8>();
        look.face = fields[5].Get<uint8>();
        look.hair = fields[6].Get<uint8>();
        look.hairColor = fields[7].Get<uint8>();
        look.facial = fields[8].Get<uint8>();

        // The entries the character was last seen wearing, transmog included: a transmogged slot's
        // value is the look's item, and a hidden one's isn't an item at all, so it shows nothing.
        // Player::BuildEnumData reads the same field for the character list.
        std::string const equipmentCache = fields[10].Get<std::string>();
        std::vector<std::string_view> equipment = Acore::Tokenize(equipmentCache, ' ', false);
        auto worn = [&equipment](uint8 slot) -> ItemTemplate const*
        {
            size_t const index = size_t(slot) * 2;
            Optional<uint32> entry = index < equipment.size() ? Acore::StringTo<uint32>(equipment[index]) : std::nullopt;
            return entry ? sObjectMgr->GetItemTemplate(*entry) : nullptr;
        };

        for (size_t i = 0; i < look.displays.size(); ++i)
        {
            uint8 const slot = Housing::MIRROR_SLOTS[i];
            bool const hidden = (slot == EQUIPMENT_SLOT_HEAD && (playerFlags & PLAYER_FLAGS_HIDE_HELM))
                || (slot == EQUIPMENT_SLOT_BACK && (playerFlags & PLAYER_FLAGS_HIDE_CLOAK));
            ItemTemplate const* proto = hidden ? nullptr : worn(slot);
            look.displays[i] = proto ? proto->DisplayInfoID : 0;
        }

        Position const at = SpotPosition(plot, spot, viewer->GetMap(), viewer->GetPhaseMask());
        TempSummon* figure = NewFigure(viewer, at, look.displayId);
        if (!figure)
            return;

        figure->SetUnitFlag2(UNIT_FLAG2_MIRROR_IMAGE);
        uint8 hand = 0;
        for (uint8 slot : { EQUIPMENT_SLOT_MAINHAND, EQUIPMENT_SLOT_OFFHAND, EQUIPMENT_SLOT_RANGED })
        {
            ItemTemplate const* proto = worn(slot);
            figure->SetVirtualItem(hand++, proto ? proto->ItemId : 0);
        }
        figure->SetSheath(SHEATH_STATE_UNARMED);
        sPlayerHousingMgr->SetFigureLook(figure->GetGUID(), look);
        if (!PlaceFigure(viewer, figure, name))
            return;

        // The current pet, for the classes the character list shows one for.
        uint32 const petModel = fields[11].Get<uint32>();
        bool const showsPet = !(playerFlags & PLAYER_FLAGS_GHOST) && (playerClass == CLASS_HUNTER || playerClass == CLASS_WARLOCK);
        if (!petModel || !showsPet)
            return;

        Spot const beside{ spot.forward + PET_FURTHER, spot.right + (spot.right < 0.0f ? -PET_ASIDE : PET_ASIDE) };
        if (TempSummon* pet = NewFigure(viewer, SpotPosition(plot, beside, viewer->GetMap(), viewer->GetPhaseMask()), petModel))
            PlaceFigure(viewer, pet, name);
    }

    // What the addon needs to know which character a clicked creature stands for.
    void SendFigures(Player* viewer)
    {
        std::vector<FigureRow> rows;
        {
            std::lock_guard<std::mutex> guard(FiguresLock);
            auto itr = Figures.find(viewer->GetGUID().GetCounter());
            if (itr != Figures.end())
                rows = itr->second.rows;
        }
        SendToAddon(viewer, "figures");
        for (FigureRow const& row : rows)
            SendToAddon(viewer, Acore::StringFormat("figure\t{:016X}\t{}", row.guid.GetRawValue(), row.name));
    }

    // The account's top characters, by the order the character list and its camp use: favourites
    // first. The answer comes in a later update, by which time the viewer is in the world.
    void AskForFigures(WorldSession* session)
    {
        std::string const text = SpotsText();
        size_t const spots = ParseSpots(text).size();
        if (!spots)
        {
            if (!text.empty())
                LOG_ERROR("module", "mod-playerhousing: Can't read PlayerHousing.Home.FigureSpots: \"{}\"", text);
            return;
        }

        session->GetQueryProcessor().AddCallback(CharacterDatabase.AsyncQuery(Acore::StringFormat(
            "SELECT c.name, c.race, c.class, c.gender, c.skin, c.face, c.hairStyle, c.hairColor, c.facialStyle, "
            "c.playerFlags, c.equipmentCache, "
            "(SELECT cp.modelid FROM character_pet AS cp WHERE cp.owner = c.guid AND cp.slot = {} LIMIT 1) "
            "FROM characters AS c WHERE c.account = {} AND c.home_viewer = 0 AND c.deleteInfos_Name IS NULL "
            "ORDER BY c.favourite DESC, COALESCE(c.order, c.guid) LIMIT {}",
            uint32(PET_SAVE_AS_CURRENT), session->GetAccountId(), spots)).WithCallback([session](QueryResult result)
        {
            Player* viewer = session->GetPlayer();
            if (!viewer || !viewer->IsHomeViewer() || !viewer->IsInWorld())
                return;

            RemoveFigures(viewer->GetGUID().GetCounter());
            if (!sObjectMgr->GetCreatureTemplate(FIGURE_ENTRY))
            {
                LOG_ERROR("module", "mod-playerhousing: The home screen's figure (creature {}) isn't in the world database.", FIGURE_ENTRY);
                return;
            }

            std::vector<Spot> const layout = ParseSpots(SpotsText());
            Plot const plot = ConfiguredPlot();
            if (result)
            {
                size_t spot = 0;
                do
                {
                    if (spot < layout.size())
                        SpawnFigure(viewer, result->Fetch(), plot, layout[spot++]);
                } while (result->NextRow());
            }
            SendFigures(viewer);
        }));
    }

    // Only the viewer can ask where to go next; anyone else just gets no answer, so the addon
    // can ask on every character and stay out of the way on the real ones.
    Player* Viewer(ChatHandler* handler)
    {
        WorldSession* session = handler->GetSession();
        Player* player = session ? session->GetPlayer() : nullptr;
        return player && player->IsHomeViewer() ? player : nullptr;
    }

    // Random, so that the account doesn't tell anyone the name and nobody can take it first. Twelve
    // letters, the most a character name has, in consonant and vowel pairs: a name can't have three
    // of a letter in a row. That's 100 to the sixth names, far too many to try.
    std::string RandomViewerName()
    {
        static constexpr char const* CONSONANTS = "bcdfghjklmnpqrstvwxz";
        static constexpr char const* VOWELS = "aeiou";
        std::string name;
        for (uint8 i = 0; i < 6; ++i)
        {
            name += CONSONANTS[urand(0, 19)];
            name += VOWELS[urand(0, 4)];
        }
        name[0] = char(name[0] - 'a' + 'A');
        return name;
    }

    // Empty if ten tries found nothing: they can fail on the realm's reserved and profanity lists,
    // or a shorter name length limit, which no later try gets past.
    std::string FreeViewerName()
    {
        for (uint8 i = 0; i < 10; ++i)
        {
            std::string name = RandomViewerName();
            if (ObjectMgr::CheckPlayerName(name, true) == CHAR_NAME_SUCCESS && !sCharacterCache->GetCharacterGuidByName(name))
                return name;
        }
        return {};
    }

    bool CreateViewer(WorldSession* session, std::string& reason)
    {
        std::string const name = FreeViewerName();
        if (name.empty())
        {
            reason = "No valid name for the viewer: check the realm's character name rules.";
            return false;
        }

        CharacterCreateInfo info(name, RACE_HUMAN, CLASS_WARRIOR, GENDER_MALE);

        // Built the way the core's character creation does it, without its script hook: no module
        // should treat this as a new character.
        std::shared_ptr<Player> viewer(new Player(session), [](Player* ptr)
        {
            if (ptr->HasAtLoginFlag(AT_LOGIN_FIRST))
                ptr->CleanupsBeforeDelete();
            delete ptr;
        });
        viewer->GetMotionMaster()->Initialize();
        if (!viewer->Create(sObjectMgr->GetGenerator<HighGuid::Player>().Generate(), &info))
        {
            reason = "The core couldn't create the viewer.";
            return false;
        }
        viewer->setCinematic(1);
        viewer->SetAtLoginFlag(AT_LOGIN_FIRST);

        CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();
        viewer->SaveToDB(trans, true, false);
        trans->Append("UPDATE characters SET home_viewer = 1, logout_time = 0, {} WHERE guid = {}", PlotUpdate(ConfiguredPlot()), viewer->GetGUID().GetCounter());
        CharacterDatabase.CommitTransaction(trans);

#if __has_include("HomeViewer.h")
        sCharacterCache->AddCharacterCacheEntry(viewer->GetGUID(), session->GetAccountId(), viewer->GetName(), viewer->getGender(), viewer->getRace(), viewer->getClass(), viewer->GetLevel(), true);
#else
        sCharacterCache->AddCharacterCacheEntry(viewer->GetGUID(), session->GetAccountId(), viewer->GetName(), viewer->getGender(), viewer->getRace(), viewer->getClass(), viewer->GetLevel());
#endif
        return true;
    }
}

class mod_home_screen_playerscript : public PlayerScript
{
public:
    mod_home_screen_playerscript() : PlayerScript("mod_home_screen_playerscript", { PLAYERHOOK_ON_HOME_VIEWER_LOGIN }) { }

    void OnPlayerHomeViewerLogin(Player* player) override
    {
        player->SetPhaseMask(PlayerHousingMgr::PhaseOfOwner(player->GetGUID().GetCounter()), false);
        player->m_serverSideVisibility.SetValue(SERVERSIDE_VISIBILITY_GM, SEC_GAMEMASTER);
        player->SetDisplayId(INVISIBLE_DISPLAY);
        player->SetUnitFlag(UNIT_FLAG_NOT_SELECTABLE);
        player->SetPlayerFlag(PLAYER_FLAGS_NO_XP_GAIN);
        // Right-dragging the camera turns the viewer: back to the plot's way, so the view is the same
        // each time.
        player->SetOrientation(ConfiguredPlot().o);
        AskForFigures(player->GetSession());
    }
};

// A viewer only runs .home: everything else would treat it as a character someone plays, from
// bots logging in for it to a module's own commands. GM accounts keep theirs, for testing.
class mod_home_screen_allcommandscript : public AllCommandScript
{
public:
    mod_home_screen_allcommandscript() : AllCommandScript("mod_home_screen_allcommandscript", { ALLCOMMANDHOOK_ON_TRY_EXECUTE_COMMAND }) { }

    bool OnTryExecuteCommand(ChatHandler& handler, std::string_view cmdStr) override
    {
        WorldSession* session = handler.GetSession();
        Player* player = session ? session->GetPlayer() : nullptr;
        if (!player || !player->IsHomeViewer() || session->IsGMAccount())
            return true;

        return cmdStr.substr(0, cmdStr.find(' ')) == "home";
    }
};

class mod_home_screen_worldscript : public WorldScript
{
public:
    mod_home_screen_worldscript() : WorldScript("mod_home_screen_worldscript", { WORLDHOOK_ON_STARTUP, WORLDHOOK_ON_UPDATE }) { }

    // Nobody is online yet, so the plot in the config moves every viewer without a teleport.
    void OnStartup() override
    {
        CharacterDatabase.Execute("UPDATE characters SET {} WHERE home_viewer <> 0", PlotUpdate(ConfiguredPlot()));
    }

    // A viewer has no logout hook, on purpose (player hooks skip it), and this catches every way of
    // leaving: the figures go within a second, and nobody else could see them anyway.
    void OnUpdate(uint32 diff) override
    {
        if (_sweepIn > diff)
        {
            _sweepIn -= diff;
            return;
        }
        _sweepIn = SWEEP_MS;

        std::vector<ObjectGuid::LowType> viewers;
        {
            std::lock_guard<std::mutex> guard(FiguresLock);
            for (auto const& entry : Figures)
                viewers.push_back(entry.first);
        }
        for (ObjectGuid::LowType viewerGuid : viewers)
            if (!ObjectAccessor::FindPlayerByLowGUID(viewerGuid))
                RemoveFigures(viewerGuid);
    }

private:
    uint32 _sweepIn{SWEEP_MS};
};

class mod_home_screen_commandscript : public CommandScript
{
public:
    mod_home_screen_commandscript() : CommandScript("mod_home_screen_commandscript") { }

    ChatCommandTable GetCommands() const override
    {
        static ChatCommandTable homeTable =
        {
            { "list",    HandleList,    SEC_PLAYER,     Console::No },
            { "play",    HandlePlay,    SEC_PLAYER,     Console::No },
            { "glue",    HandleGlue,    SEC_PLAYER,     Console::No },
            { "spots",   HandleSpots,   SEC_GAMEMASTER, Console::No },
            { "enable",  HandleEnable,  SEC_GAMEMASTER, Console::No },
            { "disable", HandleDisable, SEC_GAMEMASTER, Console::No },
        };
        static ChatCommandTable rootTable =
        {
            { "home", homeTable },
        };
        return rootTable;
    }

    // The account's characters for the addon: begin, one row each, end.
    static bool HandleList(ChatHandler* handler)
    {
        if (!Viewer(handler))
            return true;

        WorldSession* session = handler->GetSession();
        session->GetQueryProcessor().AddCallback(CharacterDatabase.AsyncQuery(Acore::StringFormat(
            "SELECT name, race, class, level, zone FROM characters WHERE account = {} AND home_viewer = 0 "
            "AND deleteInfos_Name IS NULL ORDER BY favourite DESC, COALESCE(`order`, guid)", session->GetAccountId())).WithCallback([session](QueryResult result)
        {
            Player* player = session->GetPlayer();
            if (!player || !player->IsHomeViewer())
                return;

            SendToAddon(player, "begin");
            if (result)
            {
                do
                {
                    Field* fields = result->Fetch();
                    SendToAddon(player, Acore::StringFormat("row\t{}\t{}\t{}\t{}\t{}", fields[0].Get<std::string>(),
                        fields[1].Get<uint8>(), fields[2].Get<uint8>(), fields[3].Get<uint8>(), fields[4].Get<uint32>()));
                } while (result->NextRow());
            }
            SendToAddon(player, "end");
            SendToAddon(player, Acore::StringFormat("camera\t{}", sConfigMgr->GetOption<float>("PlayerHousing.Home.CameraDistance", 6.0f)));
            SendFigures(player);
        }));
        return true;
    }

    // Where the next character list sends this session. The addon logs out right after sending
    // this: the session handles the two in order, so nothing has to come back first. A name that
    // isn't one of the account's characters just leads home again.
    static bool HandlePlay(ChatHandler* handler, std::string name)
    {
        Player* viewer = Viewer(handler);
        if (!viewer)
            return true;

        ObjectGuid guid;
        if (normalizePlayerName(name))
            guid = sCharacterCache->GetCharacterGuidByName(name);
        if (guid && guid != viewer->GetGUID()
            && sCharacterCache->GetCharacterAccountIdByGuid(guid) == handler->GetSession()->GetAccountId())
            handler->GetSession()->SetHomeEnter(guid.GetCounter());
        return true;
    }

    static bool HandleGlue(ChatHandler* handler)
    {
        if (Viewer(handler))
            handler->GetSession()->SetHomeStay();
        return true;
    }

    // Moves the figures around in the game, until the server restarts. Prints the line for the config
    // to keep a layout, and with no layout prints the one in use.
    static bool HandleSpots(ChatHandler* handler, Tail text)
    {
        std::string_view const wanted = text;
        if (!wanted.empty())
        {
            if (ParseSpots(wanted).empty())
            {
                handler->SendErrorMessage("Spots are yards forward and right of the viewer, each pair a figure: .home spots 3 -1, 4.2 -3.5", false);
                return false;
            }
            TriedSpots = std::string(wanted);
            if (Viewer(handler))
                AskForFigures(handler->GetSession());
        }
        handler->PSendSysMessage("PlayerHousing.Home.FigureSpots = \"{}\"", SpotsText());
        return true;
    }

    // Turns the home screen on for your own account.
    static bool HandleEnable(ChatHandler* handler)
    {
        WorldSession* session = handler->GetSession();
        QueryResult existing = CharacterDatabase.Query("SELECT guid FROM characters WHERE account = {} AND home_viewer <> 0", session->GetAccountId());
        if (existing)
            CharacterDatabase.Execute("UPDATE characters SET home_viewer = 1 WHERE account = {} AND home_viewer <> 0", session->GetAccountId());
        else
        {
            std::string reason;
            if (!CreateViewer(session, reason))
            {
                handler->SendErrorMessage(reason, false);
                return false;
            }
        }
        handler->SendSysMessage(sWorld->getBoolConfig(CONFIG_CHARACTER_SELECT_HOME_SCREEN)
            ? "Home screen on for this account: log out to see it."
            : "This account has a viewer now, but CharacterSelect.HomeScreen is off for the whole realm.");
        return true;
    }

    static bool HandleDisable(ChatHandler* handler)
    {
        CharacterDatabase.Execute("UPDATE characters SET home_viewer = 2 WHERE account = {} AND home_viewer <> 0", handler->GetSession()->GetAccountId());
        handler->SendSysMessage("Home screen off for this account.");
        return true;
    }
};

void AddHomeScreenScripts()
{
    new mod_home_screen_playerscript();
    new mod_home_screen_allcommandscript();
    new mod_home_screen_worldscript();
    new mod_home_screen_commandscript();
}

#else

void AddHomeScreenScripts()
{
}

#endif
