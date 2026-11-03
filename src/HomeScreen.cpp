// The whole file needs a core with home screens; mod-playerhousing.cmake finds out.
#ifdef PLAYERHOUSING_CORE_HAS_HOME_SCREEN

#include "PlayerHousingMgr.h"

#include "CharacterCache.h"
#include "Chat.h"
#include "CommandScript.h"
#include "Config.h"
#include "DatabaseEnv.h"
#include "ObjectMgr.h"
#include "Opcodes.h"
#include "Player.h"
#include "PlayerScript.h"
#include "ScriptMgr.h"
#include "StringFormat.h"
#include "World.h"
#include "WorldPacket.h"
#include "WorldScript.h"
#include "WorldSession.h"

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

namespace
{
    constexpr char const* ADDON_PREFIX = "HOMESCREEN\t";
    constexpr uint32 INVISIBLE_DISPLAY = 11686;    // the invisible stalker's model

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

    // Only the viewer can ask where to go next; anyone else just gets no answer, so the addon
    // can ask on every character and stay out of the way on the real ones.
    Player* Viewer(ChatHandler* handler)
    {
        WorldSession* session = handler->GetSession();
        Player* player = session ? session->GetPlayer() : nullptr;
        return player && player->IsHomeViewer() ? player : nullptr;
    }

    // Consonant and vowel pairs: a character name can't have three of a letter in a row, and has
    // twelve letters at most, which leaves room for over three million accounts.
    std::string ViewerName(uint32 account)
    {
        static constexpr char const* CONSONANTS = "bcdfghjklmnpqrstvwxz";
        std::string name = "Hv";
        do
        {
            name += CONSONANTS[account % 20];
            name += 'a';
            account /= 20;
        } while (account);
        return name;
    }

    bool CreateViewer(WorldSession* session, std::string& reason)
    {
        std::string const name = ViewerName(session->GetAccountId());
        if (ObjectMgr::CheckPlayerName(name, true) != CHAR_NAME_SUCCESS)
        {
            reason = "The viewer's name " + name + " isn't a valid character name here.";
            return false;
        }
        if (sCharacterCache->GetCharacterGuidByName(name))
        {
            reason = "The viewer's name " + name + " is taken.";
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

        sCharacterCache->AddCharacterCacheEntry(viewer->GetGUID(), session->GetAccountId(), viewer->GetName(), viewer->getGender(), viewer->getRace(), viewer->getClass(), viewer->GetLevel());
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
    }
};

class mod_home_screen_worldscript : public WorldScript
{
public:
    mod_home_screen_worldscript() : WorldScript("mod_home_screen_worldscript", { WORLDHOOK_ON_STARTUP }) { }

    // Nobody is online yet, so the plot in the config moves every viewer without a teleport.
    void OnStartup() override
    {
        CharacterDatabase.Execute("UPDATE characters SET {} WHERE home_viewer <> 0", PlotUpdate(ConfiguredPlot()));
    }
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
            "AND deleteInfos_Name IS NULL ORDER BY COALESCE(`order`, guid)", session->GetAccountId())).WithCallback([session](QueryResult result)
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
    new mod_home_screen_worldscript();
    new mod_home_screen_commandscript();
}

#else

void AddHomeScreenScripts()
{
}

#endif
