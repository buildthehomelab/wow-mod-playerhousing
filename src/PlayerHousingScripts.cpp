#include "PlayerHousingMgr.h"

#include "AllGameObjectScript.h"
#include "Chat.h"
#include "CommandScript.h"
#include "Config.h"
#include "Creature.h"
#include "GameObject.h"
#include "GlobalScript.h"
#include "Item.h"
#include "ItemScript.h"
#include "Player.h"
#include "QuestDef.h"
#include "ScriptMgr.h"
#include "ScriptedGossip.h"
#include "ServerScript.h"
#include "SpellInfo.h"
#include "SpellScript.h"
#include "SpellScriptLoader.h"
#include "StringConvert.h"
#include "StringFormat.h"
#include "Tokenize.h"
#include "WorldPacket.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <limits>
#include <string>
#include <vector>

using namespace Acore::ChatCommands;
using namespace Housing;

namespace
{
    void Reply(Player* player, std::string const& reason)
    {
        if (!reason.empty())
            sPlayerHousingMgr->Say(player, reason);
    }

    void EnsureStewardAppearance(Creature* creature)
    {
        uint32 displayId = sPlayerHousingMgr->GetStewardDisplayId();
        if (creature && displayId && (creature->GetDisplayId() != displayId || creature->GetNativeDisplayId() != displayId))
        {
            creature->SetDisplayId(displayId);
            creature->SetNativeDisplayId(displayId);
        }
    }

    std::string Lower(std::string_view text)
    {
        std::string value(text);
        std::transform(value.begin(), value.end(), value.begin(), ::tolower);
        return value;
    }
}

// Krook, the housing steward: in the capital cities and on every island. He gives and takes
// his welcome tour's quests; everything else is the housing window, which he opens.
class npc_playerhousing_steward : public CreatureScript
{
public:
    npc_playerhousing_steward() : CreatureScript("npc_playerhousing_steward") { }

    bool OnGossipHello(Player* player, Creature* creature) override
    {
        EnsureStewardAppearance(creature);
        // Playerbots don't do housing.
        if (player->GetSession()->IsHeadless())
            return true;

        ClearGossipMenuFor(player);
        // No House Key (never had one, or lost it): Krook has one.
        if (!player->HasItemCount(HOUSE_KEY_ITEM, 1, true))
        {
            bool hadOne = sPlayerHousingMgr->HadHouseKey(player);
            AddGossipItemFor(player, GOSSIP_ICON_CHAT, hadOne ? "I've lost my House Key." : "I'd like a house of my own.",
                GOSSIP_SENDER_MAIN, ACTION_KEY);
            SendGossipMenuFor(player, TEXT_NO_KEY, creature->GetGUID());
            return true;
        }
        if (creature->IsQuestGiver())
            player->PrepareQuestMenu(creature->GetGUID());
        SendGossipMenuFor(player, TEXT_HOME, creature->GetGUID());
        sPlayerHousingMgr->OpenWindow(player);
        return true;
    }

    bool OnGossipSelect(Player* player, Creature* creature, uint32 /*sender*/, uint32 action) override
    {
        CloseGossipMenuFor(player);
        if (action == ACTION_KEY && !player->GetSession()->IsHeadless())
        {
            sPlayerHousingMgr->GiveKeyFromKrook(player);
            // Now with the key: his quests (the welcome tour starts here).
            OnGossipHello(player, creature);
        }
        return true;
    }

private:
    static constexpr uint32 ACTION_KEY = 1;

    // Taking "Home Sweet Island" from Krook on the island itself: already there.
    bool OnQuestAccept(Player* player, Creature* /*creature*/, Quest const* quest) override
    {
        if (quest->GetQuestId() == QUEST_TOUR_HOME && sPlayerHousingMgr->IsOnOwnIsland(player))
            PlayerHousingMgr::QuestEvent(player, QUEST_TOUR_HOME);
        return false;
    }
};

namespace
{
    // A click on a piece on an island the player decorates picks it up: it follows the mouse
    // (or the player) until a click sets it down, the one way pieces move. With Ctrl held (the
    // addon says so) it joins the selection instead, or leaves it; a click on a selected piece
    // picks up the whole selection.
    void PickUpFromClick(Player* player, uint32 placementId)
    {
        // Already holding one: that click was the addon's right-click putting it back.
        if (sPlayerHousingMgr->IsCarrying(player))
            return;

        std::string reason;
        if (sPlayerHousingMgr->IsDecorating(player) && sPlayerHousingMgr->IsGroupHold(player))
        {
            sPlayerHousingMgr->ToggleGroupMember(player, placementId, reason);
            return;
        }

        std::vector<uint32> group = sPlayerHousingMgr->GetGroup(player);
        bool wholeGroup = group.size() > 1 && std::find(group.begin(), group.end(), placementId) != group.end();
        if (!wholeGroup)
            sPlayerHousingMgr->SelectPlacement(player, placementId);
        if (!sPlayerHousingMgr->StartGhostMove(player, wholeGroup ? 0 : placementId, reason) && !reason.empty())
            Reply(player, reason);
        sPlayerHousingMgr->SendAddonState(player);
        sPlayerHousingMgr->OpenWindow(player);
    }
}

// A stand's figure. A right-click opens its character sheet for the owner (and roommates):
// gear dragged on and off, race and looks, trading gear, and Move; guests hear what it's wearing.
class npc_playerhousing_mannequin : public CreatureScript
{
public:
    npc_playerhousing_mannequin() : CreatureScript("npc_playerhousing_mannequin") { }

    bool OnGossipHello(Player* player, Creature* creature) override
    {
        uint32 placementId = sPlayerHousingMgr->GetPlacementForObject(player, creature->GetGUID());
        if (!placementId)
            return true;

        if (sPlayerHousingMgr->CanDecorate(player))
        {
            if (!sPlayerHousingMgr->IsCarrying(player))
            {
                sPlayerHousingMgr->SelectPlacement(player, placementId);
                sPlayerHousingMgr->SendAddonState(player);
                sPlayerHousingMgr->SendAddon(player, Acore::StringFormat("dress\t{}", placementId));
            }
        }
        else
            Reply(player, sPlayerHousingMgr->DescribeStand(player, placementId));
        CloseGossipMenuFor(player);
        return true;
    }
};

// A figurine: its owner (and roommates) select it; visitors hear what it is.
class npc_playerhousing_figurine : public CreatureScript
{
public:
    npc_playerhousing_figurine() : CreatureScript("npc_playerhousing_figurine") { }

    bool OnGossipHello(Player* player, Creature* creature) override
    {
        uint32 placementId = sPlayerHousingMgr->GetPlacementForObject(player, creature->GetGUID());
        if (!placementId)
            return true;

        if (sPlayerHousingMgr->CanDecorate(player))
            PickUpFromClick(player, placementId);
        else
            Reply(player, Acore::StringFormat("{}: a trophy of {}'s adventures.", creature->GetName(),
                sPlayerHousingMgr->NameOf(sPlayerHousingMgr->GetIslandOwner(player))));
        CloseGossipMenuFor(player);
        return true;
    }
};

// A mannequin's armor reaches the client as mirror image data, which the client asks for when
// the figure comes into view: the core only answers for real mirror images (spells), so the
// module answers for mannequins.
class mod_playerhousing_serverscript : public ServerScript
{
public:
    mod_playerhousing_serverscript() : ServerScript("mod_playerhousing_serverscript", { SERVERHOOK_CAN_PACKET_RECEIVE }) { }

    bool CanPacketReceive(WorldSession* session, WorldPacket const& packet) override
    {
        if (packet.GetOpcode() != CMSG_GET_MIRRORIMAGE_DATA || packet.size() < sizeof(uint64))
            return true;

        return !sPlayerHousingMgr->SendMannequinLook(session, ObjectGuid(packet.read<uint64>(0)));
    }
};

// The House Key: its spell is caught in spell_playerhousing_key, which opens the housing window.
class item_playerhousing_key : public ItemScript
{
public:
    item_playerhousing_key() : ItemScript("item_playerhousing_key") { }

    bool OnUse(Player* /*player*/, Item* /*item*/, SpellCastTargets const& /*targets*/) override
    {
        return false;
    }
};

// Furnishing items from before pieces were kept in the Collection: still named by the item
// table (the Collection shows their icons), they no longer go in the bags. One still around is
// caught in spell_playerhousing_place when used.
class item_playerhousing_piece : public ItemScript
{
public:
    item_playerhousing_piece() : ItemScript("item_playerhousing_piece") { }

    bool OnUse(Player* /*player*/, Item* /*item*/, SpellCastTargets const& /*targets*/) override
    {
        return false;
    }
};

// An old furnishing (or "Move a Piece") item used: it and any like it go into the Collection
// instead, and nothing is cast.
class spell_playerhousing_place : public SpellScript
{
    PrepareSpellScript(spell_playerhousing_place);

    SpellCastResult CheckCast()
    {
        Player* player = GetCaster() ? GetCaster()->ToPlayer() : nullptr;
        Item* item = GetCastItem();
        bool mover = item && PlayerHousingMgr::IsMoverItem(item->GetEntry());
        if (!player || !item || !sPlayerHousingMgr->IsEnabled() || (!mover && !sPlayerHousingMgr->GetPiece(item->GetEntry())))
            return SPELL_CAST_OK;

        sPlayerHousingMgr->SweepHousingItems(player, false);
        sPlayerHousingMgr->OpenWindow(player, "Collection");
        return SPELL_FAILED_DONT_REPORT;
    }

    void Register() override
    {
        OnCheckCast += SpellCheckCastFn(spell_playerhousing_place::CheckCast);
    }
};

class spell_playerhousing_key : public SpellScript
{
    PrepareSpellScript(spell_playerhousing_key);

    SpellCastResult CheckCast()
    {
        Player* player = GetCaster() ? GetCaster()->ToPlayer() : nullptr;
        Item* item = GetCastItem();
        if (!player || !item || item->GetEntry() != HOUSE_KEY_ITEM)
            return SPELL_CAST_OK;

        if (!sPlayerHousingMgr->IsEnabled())
        {
            Reply(player, "Housing is disabled on this server.");
            return SPELL_FAILED_DONT_REPORT;
        }

        sPlayerHousingMgr->OpenWindow(player);
        return SPELL_FAILED_DONT_REPORT;
    }

    void Register() override
    {
        OnCheckCast += SpellCheckCastFn(spell_playerhousing_key::CheckCast);
    }
};

// Every placed piece. Clicking a plain piece picks it up for its owner (and roommates), to set
// down with the mouse; pieces that work like the real thing (chairs seat, mailboxes open) do
// that, unless the housing window is open (decorating).
class go_playerhousing_piece : public GameObjectScript
{
public:
    go_playerhousing_piece() : GameObjectScript("go_playerhousing_piece") { }

    bool OnGossipHello(Player* player, GameObject* go) override
    {
        uint32 placementId = sPlayerHousingMgr->GetPlacementForObject(player, go->GetGUID());
        if (!placementId)
            return false;

        // Decorating (the window is open): a click picks the piece up, or with Ctrl selects it.
        if (sPlayerHousingMgr->IsDecorating(player) && sPlayerHousingMgr->CanDecorate(player))
        {
            PickUpFromClick(player, placementId);
            return true;
        }

        // A Bank Chest opens its owner's bank straight away; for anyone else it's locked.
        if (std::optional<Placement> placement = sPlayerHousingMgr->GetPlacement(player, placementId))
        {
            PieceDefinition const* piece = sPlayerHousingMgr->GetPiece(placement->itemEntry);
            if (piece && piece->HasFlag(PIECE_FLAG_CHEST))
            {
                std::string reason;
                if (!sPlayerHousingMgr->IsOnOwnIsland(player))
                    Reply(player, "The chest is locked: it holds its owner's bank.");
                else if (!sPlayerHousingMgr->OpenBankAtChest(player, placementId, reason))
                    Reply(player, reason);
                return true;
            }
            if (piece && piece->HasFlag(PIECE_FLAG_MUSIC))
            {
                if (sPlayerHousingMgr->IsOnOwnIsland(player))
                    sPlayerHousingMgr->OpenWindow(player, "Island");
                else
                {
                    HouseRecord house;
                    char const* track = sPlayerHousingMgr->GetHouseRecord(sPlayerHousingMgr->GetIslandOwner(player), house) && house.music
                        ? PlayerHousingMgr::MusicName(house.music) : nullptr;
                    Reply(player, track ? Acore::StringFormat("The music box is playing {}.", track) : std::string("The music box is quiet."));
                }
                return true;
            }
        }

        // A plain piece (nothing to sit on or open): picked up, decorating from now on. (The
        // pieces that work like the real thing are picked up only while the window is open.)
        if (sPlayerHousingMgr->CanDecorate(player) && go->GetGoType() == GAMEOBJECT_TYPE_GOOBER)
        {
            std::string ignored;
            sPlayerHousingMgr->SetDecorating(player, true, ignored);
            PickUpFromClick(player, placementId);
            return true;
        }

        return false;
    }
};

class mod_playerhousing_worldscript : public WorldScript
{
public:
    mod_playerhousing_worldscript() : WorldScript("mod_playerhousing_worldscript") { }

    void OnStartup() override
    {
        sPlayerHousingMgr->OnStartup();
    }
};

class mod_playerhousing_commandscript : public CommandScript
{
public:
    mod_playerhousing_commandscript() : CommandScript("mod_playerhousing_commandscript") { }

    ChatCommandTable GetCommands() const override
    {
        static ChatCommandTable rootTable =
        {
            { "house", HandleHouseCommand, SEC_PLAYER, Console::No },
            { "krook", HandleHouseCommand, SEC_PLAYER, Console::No }
        };
        return rootTable;
    }

    static void SendUsage(ChatHandler* handler, bool gm)
    {
        handler->SendSysMessage("Housing commands (.house alone opens the housing window, which has all of them):");
        handler->SendSysMessage(".house home | leave | unstuck | krook | visit <name>");
        handler->SendSysMessage(".house decorate [on|off] | undo [steps] | redo | packup | highlight <id|0>");
        handler->SendSysMessage(".house shift <forward> <left> <up> <degrees> [id] | select next|previous");
        handler->SendSysMessage(".house pickup [id] [inside] | rotate <degrees> [id] | face [id] | here [id] | move [id]");
        handler->SendSysMessage(".house nudge <forward|back|left|right|up|down> [yards] [id] | select <id|nearest> | list");
        handler->SendSysMessage(".house size <bigger|smaller|normal|percent> [id] | tilt <forward|back|left|right|straight> [degrees] [id]");
        handler->SendSysMessage(".house ghost <item> | ghost move [id] | ghost adjust <forward> <left> <up> <degrees> | ghost at <x> <y> <z>");
        handler->SendSysMessage(".house ghost place [another] | ghost cancel");
        handler->SendSysMessage(".house another [id] | grid <off|yards> | roommate <name> | unroommate <name> | like | visitors");
        handler->SendSysMessage(".house layout <save <name> | load <name> | delete <name> | send <name> <player> | rename <name> <new name>");
        handler->SendSysMessage(".house layout <overwrite <name> | missing <name> | copyable on|off | copy | list>");
        handler->SendSysMessage(".house get <item> [count] | get all (test servers) | invite <name|target|party> | uninvite <name>");
        handler->SendSysMessage(".house stand <dress <item> | undress <slot|all> | look <race> <male|female> | trade> [id]");
        handler->SendSysMessage(".house weather <name> | time <name> | music <sound id|off> | privacy <private|friends|public> | greeting <text|clear>");
        handler->SendSysMessage(".house group [add|remove <id> | clear] | match <height|turn|line|space> | row <count> [yards] [right|left|forward|back]");
        handler->SendSysMessage(".house set [save <name> | place <name> | delete <name> | list] | undo [steps] | goto <id>");
        handler->SendSysMessage(".house sign <note> (while visiting) | guestbook [delete <id>] | door [here|reset]");
        handler->SendSysMessage(".house report <what's wrong> (while visiting)");
        if (gm)
        {
            handler->SendSysMessage("GM: .house key | unlock|relock <item|name|all> [player] | unlocks [player] | add (steward)");
            handler->SendSysMessage("GM: .house reports [all] | close <id> | inspect <player> | hide|unhide <player> | cleargreeting <player> | gmpackup <player>");
            handler->SendSysMessage("GM: .house phototour <start|next|stop|item> (the addon's /housing phototour takes the pictures)");
        }
    }

    static Player* GmTarget(ChatHandler* handler, Player* self, std::vector<std::string_view> const& tokens, size_t index)
    {
        if (tokens.size() > index)
            if (Player* named = ObjectAccessor::FindPlayerByName(std::string(tokens[index]), true))
                return named;
        if (Player* selected = handler->getSelectedPlayer())
            return selected;
        return self;
    }

    static bool HandleHouseCommand(ChatHandler* handler, char const* args)
    {
        Player* player = handler ? handler->GetPlayer() : nullptr;
        if (!player)
        {
            if (handler)
            {
                handler->SendSysMessage("This command can only be used in-game.");
                handler->SetSentErrorMessage(true);
            }
            return false;
        }

        // Off, the module's tables needn't exist, and a query to a missing table stops the server.
        if (!sPlayerHousingMgr->IsEnabled())
        {
            handler->SendSysMessage("Housing is off on this server.");
            handler->SetSentErrorMessage(true);
            return false;
        }

        bool gm = player->GetSession()->GetSecurity() >= SEC_GAMEMASTER;
        std::vector<std::string_view> tokens = Acore::Tokenize(args ? args : "", ' ', false);
        if (tokens.empty())
        {
            sPlayerHousingMgr->OpenWindow(player);
            return true;
        }

        auto number = [&](size_t index) -> uint32
        {
            if (tokens.size() <= index)
                return 0;
            return Acore::StringTo<uint32>(tokens[index]).value_or(0);
        };
        auto decimal = [&](size_t index, float fallback) -> float
        {
            if (tokens.size() <= index)
                return fallback;
            // nan and inf read as numbers too; they aren't turns or distances.
            std::optional<float> value = Acore::StringTo<float>(tokens[index]);
            return value && std::isfinite(*value) ? *value : fallback;
        };
        auto restFrom = [&](size_t index) -> std::string
        {
            std::string text;
            for (size_t i = index; i < tokens.size(); ++i)
                text += (text.empty() ? "" : " ") + std::string(tokens[i]);
            return text;
        };

        std::string sub = Lower(tokens[0]);
        std::string reason;
        PlayerHousingMgr* mgr = sPlayerHousingMgr;

        // The addon's quiet reads (state, lists, marking seen, saying it's there) are cheap and
        // don't count; edit mode's moves and the mouse's points have windows of their own;
        // everything else counts.
        bool holdMessage = sub == "group" && tokens.size() > 1 && Lower(tokens[1]) == "hold";
        bool ghostAdjust = sub == "ghost" && tokens.size() > 1 && Lower(tokens[1]) == "adjust";
        bool ghostAt = sub == "ghost" && tokens.size() > 1 && Lower(tokens[1]) == "at";
        if (ghostAt)
        {
            if (!gm && mgr->PointFlood(player))
                return true;
        }
        else if (sub == "shift" || ghostAdjust)
        {
            if (!gm && mgr->ShiftFlood(player))
                return true;
        }
        else if (sub != "state" && sub != "data" && sub != "seen" && sub != "addon" && !holdMessage && !gm && mgr->CommandFlood(player))
        {
            Reply(player, "Too many housing commands at once: give it a moment.");
            return true;
        }

        if (sub == "state")
        {
            // Quiet: for the client addon.
            mgr->SendAddonState(player);
            return true;
        }
        else if (sub == "home" || sub == "go")
            mgr->RequestGoHome(player, reason);
        else if (sub == "leave")
            mgr->LeaveHouse(player, reason);
        else if (sub == "krook")
            mgr->CallSteward(player, reason);
        else if (sub == "unstuck")
            mgr->Unstuck(player, reason);
        else if (sub == "key")
        {
            // Krook hands out keys (beside every capital's innkeeper); a GM can make one.
            if (player->GetSession()->GetSecurity() >= SEC_GAMEMASTER)
                mgr->GiveHouseKey(player, reason);
            else
                reason = "Krook, beside the innkeeper in every capital city, has House Keys: ask him for yours.";
        }
        else if (sub == "decorate")
        {
            std::string mode = tokens.size() > 1 ? Lower(tokens[1]) : "";
            bool on = mode == "on" ? true : (mode == "off" ? false : !mgr->IsDecorating(player));
            mgr->SetDecorating(player, on, reason);
        }
        else if (sub == "addon")
        {
            // Quiet: the addon says it's there, and whether PlayerHousing.dll is there (mouse)
            // and moves the ghost itself (local). Older addons sent a House Key setting first.
            bool mouse = false;
            bool local = false;
            for (size_t i = 1; i < tokens.size(); ++i)
            {
                std::string word = Lower(tokens[i]);
                mouse = mouse || word == "mouse";
                local = local || word == "local";
            }
            mgr->SetAddonClient(player, mouse, local);
            mgr->SendAddonState(player);
            mgr->SendFigurineCreatures(player);
            return true;
        }
        else if (sub == "data")
        {
            // Quiet: lists for the addon's window.
            if (tokens.size() < 2 || !mgr->SendAddonData(player, Lower(tokens[1]), tokens.size() > 2 ? std::string(tokens[2]) : "", reason))
                Reply(player, reason.empty() ? "Usage: .house data <collection|placed|layouts|guests|visits <list>|island>" : reason);
            return true;
        }
        else if (sub == "seen")
        {
            mgr->MarkAllSeen(player);
            return true;
        }
        else if (sub == "get")
        {
            uint32 item = number(1);
            uint32 count = std::clamp<uint32>(tokens.size() > 2 ? number(2) : 1, 1, 20);
            if (tokens.size() > 1 && Lower(tokens[1]) == "all")
                mgr->GetOneOfEverything(player, reason);
            else if (!item)
                reason = "Usage: .house get <item entry> [count] | get all";
            else
                mgr->GetCopies(player, item, count, reason);
        }
        else if (sub == "take" || sub == "storage")
        {
            // House Storage became the Collection's counts: nothing goes to the bags any more.
            reason = "Pieces stay in your Collection now: place them from the housing window.";
            mgr->SweepHousingItems(player, true);
        }
        else if (sub == "stand")
        {
            // A mannequin (the selected piece, or the id last): dress <item entry>, undress
            // <slot|all>, figure, look, trade (its gear for the character's).
            std::string what = tokens.size() > 1 ? Lower(tokens[1]) : "";
            if (what == "dress")
                mgr->PutOnStandByEntry(player, mgr->ResolvePlacementArgument(player, number(3)), number(2), reason);
            else if (what == "undress")
            {
                bool all = tokens.size() > 2 && Lower(tokens[2]) == "all";
                mgr->TakeOffStand(player, mgr->ResolvePlacementArgument(player, number(3)), all ? -1 : int32(number(2)), reason);
            }
            else if (what == "figure")
                mgr->ChangeStandFigure(player, mgr->ResolvePlacementArgument(player, number(2)), reason);
            else if (what == "look")
            {
                // A race (its number) and male or female, with a random look.
                bool female = tokens.size() > 3 && Lower(tokens[3]).rfind("f", 0) == 0;
                mgr->SetStandLook(player, mgr->ResolvePlacementArgument(player, number(4)), uint8(number(2)), female ? GENDER_FEMALE : GENDER_MALE, reason);
            }
            else if (what == "trade")
                mgr->TradeStandGear(player, mgr->ResolvePlacementArgument(player, number(2)), reason);
            else
                reason = "Usage: .house stand <dress <item entry> | undress <slot|all> | figure | look <race> <male|female> | trade> [id]";
            mgr->SendAddonState(player);
        }
        else if (sub == "weather" || sub == "time")
        {
            // By number, or by name: .house weather light rain
            bool weather = sub == "weather";
            uint8 count = weather ? PlayerHousingMgr::WeatherCount() : PlayerHousingMgr::TimeOfDayCount();
            std::string wanted = Lower(restFrom(1));
            int32 choice = -1;
            for (uint8 i = 0; i < count; ++i)
                if (wanted == std::to_string(i) || wanted == Lower(weather ? PlayerHousingMgr::WeatherName(i) : PlayerHousingMgr::TimeOfDayName(i)))
                    choice = i;
            if (choice < 0)
            {
                std::string names;
                for (uint8 i = 0; i < count; ++i)
                    names += (names.empty() ? "" : ", ") + std::string(weather ? PlayerHousingMgr::WeatherName(i) : PlayerHousingMgr::TimeOfDayName(i));
                reason = Acore::StringFormat("Usage: .house {} <{}>", sub, names);
            }
            else if (weather)
                mgr->SetWeather(player, uint8(choice), reason);
            else
                mgr->SetTimeOfDay(player, uint8(choice), reason);
        }
        else if (sub == "music")
        {
            std::string wanted = tokens.size() > 1 ? Lower(tokens[1]) : "";
            mgr->SetMusic(player, wanted == "off" || wanted == "none" ? 0 : number(1), reason);
        }
        else if (sub == "edit")
        {
            std::string mode = tokens.size() > 1 ? Lower(tokens[1]) : "";
            bool on = mode == "on" ? true : (mode == "off" ? false : !mgr->IsInEditMode(player));
            mgr->SetEditMode(player, on, reason);
        }
        else if (sub == "shift")
        {
            // Edit mode: forward, left and up in yards, a turn in degrees, then the piece.
            // Quiet when it works; the addon shows the result.
            if (mgr->Shift(player, number(5), decimal(1, 0.0f), decimal(2, 0.0f), decimal(3, 0.0f), decimal(4, 0.0f), reason))
                reason.clear();
        }
        else if (sub == "undo")
            mgr->UndoSteps(player, std::max<uint32>(1, number(1)), reason);
        else if (sub == "redo")
            mgr->Redo(player, reason);
        else if (sub == "packup")
            mgr->PackUpEverything(player, reason);
        else if (sub == "pickup")
        {
            bool inside = tokens.size() > 1 && Lower(tokens.back()) == "inside";
            mgr->PickUp(player, number(1), inside, reason);
        }
        else if (sub == "rotate" || sub == "turn")
            mgr->Rotate(player, number(2), decimal(1, 45.0f), reason);
        else if (sub == "move")
            mgr->StartGhostMove(player, number(1), reason);
        else if (sub == "face")
            mgr->FaceMe(player, number(1), reason);
        else if (sub == "here")
            mgr->MoveHere(player, number(1), reason);
        else if (sub == "nudge" || sub == "up" || sub == "down")
        {
            std::string direction = sub == "nudge" ? (tokens.size() > 1 ? Lower(tokens[1]) : "") : sub;
            size_t distanceIndex = sub == "nudge" ? 2 : 1;
            bool vertical = direction == "up" || direction == "down";
            float distance = decimal(distanceIndex, vertical ? 0.1f : 0.25f);
            uint32 id = number(distanceIndex + 1);
            float forward = 0.0f, left = 0.0f, up = 0.0f;
            if (direction == "forward") forward = distance;
            else if (direction == "back") forward = -distance;
            else if (direction == "left") left = distance;
            else if (direction == "right") left = -distance;
            else if (direction == "up") up = distance;
            else if (direction == "down") up = -distance;
            else
                reason = "Nudge which way? forward, back, left, right, up or down.";

            if (reason.empty())
                mgr->Nudge(player, id, forward, left, up, reason);
        }
        else if (sub == "size" || sub == "resize")
        {
            std::string how = tokens.size() > 1 ? Lower(tokens[1]) : "";
            uint32 id = number(2);
            if (how == "bigger" || how == "up")
                mgr->Resize(player, id, 10.0f, true, reason);
            else if (how == "smaller" || how == "down")
                mgr->Resize(player, id, -10.0f, true, reason);
            else if (how == "normal" || how == "reset")
                mgr->Resize(player, id, 100.0f, false, reason);
            else if (float percent = decimal(1, 0.0f); percent > 0.0f)
                mgr->Resize(player, id, percent, false, reason);
            else
                reason = Acore::StringFormat("Usage: .house size <bigger|smaller|normal|percent> [id]. Sizes go from {:.0f}% to {:.0f}%.",
                    mgr->GetMinSize() * 100.0f, mgr->GetMaxSize() * 100.0f);
        }
        else if (sub == "tilt")
        {
            std::string direction = tokens.size() > 1 ? Lower(tokens[1]) : "";
            float degrees = decimal(2, 5.0f);
            uint32 id = number(3);
            if (direction == "forward")
                mgr->Tilt(player, id, degrees, 0.0f, false, reason);
            else if (direction == "back")
                mgr->Tilt(player, id, -degrees, 0.0f, false, reason);
            else if (direction == "right")
                mgr->Tilt(player, id, 0.0f, degrees, false, reason);
            else if (direction == "left")
                mgr->Tilt(player, id, 0.0f, -degrees, false, reason);
            else if (direction == "straight" || direction == "level")
                mgr->Tilt(player, number(2), 0.0f, 0.0f, true, reason);
            else
                reason = "Usage: .house tilt <forward|back|left|right|straight> [degrees] [id]. Forward tips its front down; left and right are its own.";
        }
        else if (sub == "report")
            mgr->ReportIsland(player, restFrom(1), reason);
        else if (gm && sub == "reports")
        {
            bool all = tokens.size() > 1 && Lower(tokens[1]) == "all";
            auto reports = mgr->GetReports(all, 20);
            if (reports.empty())
                reason = all ? "No reports yet." : "No open reports.";
            for (IslandReport const& report : reports)
                handler->PSendSysMessage("#{} {}: {}'s island, reported by {}: {}{}", report.id, report.when, mgr->NameOf(report.ownerGuid),
                    mgr->NameOf(report.reporterGuid), report.reason, report.closed ? " (closed)" : "");
        }
        else if (gm && sub == "close")
            mgr->CloseReport(player, number(1), reason);
        else if (gm && (sub == "inspect" || sub == "hide" || sub == "unhide" || sub == "cleargreeting" || sub == "gmpackup"))
        {
            ObjectGuid::LowType target = 0;
            std::string name;
            if (tokens.size() < 2 || !mgr->ResolvePlayerGuid(std::string(tokens[1]), target, name))
                reason = Acore::StringFormat("Usage: .house {} <player>", sub);
            else if ((target = mgr->HomeOfCharacter(target)) == 0)
                reason = "No island.";  // never: a character always has an account
            else if (sub == "inspect")
                mgr->GmInspect(player, target, reason);
            else if (sub == "hide" || sub == "unhide")
                mgr->GmSetHidden(player, target, sub == "hide", reason);
            else if (sub == "cleargreeting")
                mgr->GmClearGreeting(player, target, reason);
            else
                mgr->GmPackUp(player, target, reason);
        }
        else if (sub == "like")
            mgr->ToggleLike(player, reason);
        else if (sub == "visitors")
        {
            ObjectGuid::LowType self = mgr->HomeOf(player);  // the account's island
            auto log = mgr->GetVisitorLog(self, 20);
            reason = Acore::StringFormat("{} visitors this week, {} likes.{}", mgr->CountVisitorsThisWeek(self), mgr->CountLikes(self),
                log.empty() ? " Nobody has visited yet." : "");
            for (auto const& [name, when] : log)
                handler->PSendSysMessage("{}, {}", name, when);
        }
        else if (sub == "roommate" || sub == "unroommate")
        {
            ObjectGuid::LowType guestGuid = 0;
            std::string name;
            if (tokens.size() < 2)
                reason = Acore::StringFormat("Usage: .house {} <name>", sub);
            else if (!mgr->ResolvePlayerGuid(std::string(tokens[1]), guestGuid, name))
                reason = "No character with that name.";
            else
                mgr->SetRoommate(player, guestGuid, sub == "roommate", reason);
        }
        else if (sub == "layout" || sub == "layouts")
        {
            std::string what = tokens.size() > 1 ? Lower(tokens[1]) : "";
            ObjectGuid::LowType self = mgr->HomeOf(player);  // the account's island
            auto find = [&](size_t index) -> uint32
            {
                std::string name = tokens.size() > index ? std::string(tokens[index]) : "";
                if (what == "send" && tokens.size() > 3)
                    name = std::string(tokens[2]);
                else
                    name = restFrom(index);
                std::optional<SavedLayout> layout = mgr->FindSavedLayout(self, name);
                if (!layout)
                    reason = name.empty() ? "Which layout? .house layout list shows them." : "You have no layout called " + name + ".";
                return layout ? layout->id : 0;
            };
            if (what.empty())
            {
                mgr->OpenWindow(player, "Layouts");
                return true;
            }
            else if (what == "save")
                mgr->SaveLayout(player, 0, restFrom(2), reason);
            else if (what == "overwrite")
            {
                if (uint32 id = find(2))
                    mgr->SaveLayout(player, id, "", reason);
            }
            else if (what == "rename")
            {
                // By number: the new name may have spaces, so the layout comes first, as a number.
                std::optional<SavedLayout> layout = tokens.size() > 3 ? mgr->FindSavedLayout(self, std::string(tokens[2])) : std::nullopt;
                if (!layout)
                    reason = "Usage: .house layout rename <layout number> <new name>";
                else
                    mgr->RenameLayout(player, layout->id, restFrom(3), reason);
            }
            else if (what == "missing")
            {
                if (uint32 id = find(2))
                    mgr->GetMissingForLayout(player, id, reason);
            }
            else if (what == "copyable")
            {
                std::string mode = tokens.size() > 2 ? Lower(tokens[2]) : "";
                if (mode == "on" || mode == "off")
                    mgr->SetLayoutCopyable(player, mode == "on", reason);
                else
                    reason = mgr->IsLayoutCopyable(self) ? "Visitors may save a copy of your island's layout. .house layout copyable off stops that."
                                                         : "Visitors can't copy your island's layout. .house layout copyable on lets them.";
            }
            else if (what == "copy")
                mgr->CopyIslandLayout(player, reason);
            else if (what == "load" || what == "switch" || what == "set")
            {
                if (uint32 id = find(2))
                    mgr->SwitchLayout(player, id, reason);
            }
            else if (what == "delete")
            {
                if (uint32 id = find(2))
                    mgr->DeleteLayout(player, id, reason);
            }
            else if (what == "send")
            {
                if (tokens.size() < 4)
                    reason = "Usage: .house layout send <layout> <player>";
                else if (uint32 id = find(2))
                    mgr->SendLayout(player, id, std::string(tokens[3]), reason);
            }
            else if (what == "list")
            {
                std::vector<SavedLayout> layouts = mgr->GetSavedLayouts(self);
                if (layouts.empty())
                    reason = "No saved layouts yet: .house layout save <name>.";
                for (SavedLayout const& layout : layouts)
                    handler->PSendSysMessage("#{} {} ({} pieces, {})", layout.id, layout.name, layout.pieces, layout.savedAt);
            }
            else
                reason = "Usage: .house layout <save | load | delete | send | rename | overwrite | missing | copyable | copy | list>";
        }
        else if (sub == "another" || sub == "copy")
            mgr->PlaceAnother(player, number(1), reason);
        else if (sub == "ghost")
        {
            // A piece following the player until it's set down (the addon's keys drive it).
            std::string what = tokens.size() > 1 ? Lower(tokens[1]) : "";
            // x y z [facing x y z] from tokens[index] on: the ghost goes there. False, with why,
            // when it can't (quiet when it's only the mouse wandering off the island).
            auto pointAt = [&](size_t index, std::string& why) -> bool
            {
                float nan = std::numeric_limits<float>::quiet_NaN();
                float x = decimal(index, nan);
                float y = decimal(index + 1, nan);
                float z = decimal(index + 2, nan);
                if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z))
                {
                    why = "Usage: .house ghost at <x> <y> <z> [<facing x> <facing y> <facing z>]";
                    return false;
                }
                float facing[3] = { decimal(index + 3, nan), decimal(index + 4, nan), decimal(index + 5, nan) };
                if (mgr->GhostAt(player, x, y, z, tokens.size() > index + 5 ? facing : nullptr, why))
                    return true;
                if (what == "at")
                    why.clear();  // the addon shows why; chat stays quiet
                return false;
            };
            if (what == "move")
                mgr->StartGhostMove(player, number(2), reason);
            else if (what == "adjust")
            {
                // Quiet when it works: the ghost itself shows it.
                if (mgr->AdjustGhost(player, decimal(2, 0.0f), decimal(3, 0.0f), decimal(4, 0.0f), decimal(5, 0.0f), reason))
                    reason.clear();
            }
            else if (what == "at")
            {
                // Quiet: where the mouse points, a few times a second (the addon, with
                // PlayerHousing.dll); x y z, then which way the surface there faces.
                if (!pointAt(2, reason))
                    Reply(player, reason);
                return true;
            }
            else if (what == "place")
            {
                // A click: set down where the mouse points (the point comes with it, so a spot
                // that can't be used isn't swapped for an older one).
                bool another = tokens.size() > 2 && Lower(tokens[2]) == "another";
                size_t at = another ? 3 : 2;
                if (tokens.size() > at && Lower(tokens[at]) == "at" && !pointAt(at + 1, reason))
                {
                    Reply(player, reason);
                    return true;
                }
                mgr->PlaceGhost(player, another, reason);
            }
            else if (what == "cancel")
                mgr->CancelGhost(player);
            else if (what == "new" && tokens.size() > 2)
                mgr->StartGhostNew(player, number(2), 0, reason);
            else if (uint32 item = number(1))
                mgr->StartGhostNew(player, item, 0, reason);
            else
                reason = "Usage: .house ghost <item> | move [id] | adjust <forward> <left> <up> <degrees> | at <x> <y> <z> | place [another] [at <x> <y> <z>] | cancel";
        }
        else if (sub == "group")
        {
            std::string what = tokens.size() > 1 ? Lower(tokens[1]) : "";
            if (what == "hold")
            {
                // Quiet: the addon says Ctrl went down or up.
                mgr->SetGroupHold(player, tokens.size() > 2 && Lower(tokens[2]) == "on");
                return true;
            }
            if (what == "add" || what == "remove")
                mgr->SetGroupMember(player, number(2), what == "add", reason);
            else if (what == "clear")
            {
                mgr->ClearGroup(player);
                reason = "One piece selected again.";
            }
            else
            {
                size_t count = mgr->GetGroup(player).size();
                reason = count > 1 ? Acore::StringFormat("{} pieces selected. Ctrl-right-click a piece to add it or take it out.", count)
                                   : "One piece selected. Ctrl-right-click others to move them together.";
            }
        }
        else if (sub == "match")
            mgr->MatchGroup(player, tokens.size() > 1 ? Lower(tokens[1]) : "", reason);
        else if (sub == "row")
        {
            // .house row <count> [yards] [right|left|forward|back] [id]
            uint32 count = number(1);
            float spacing = 0.0f;
            std::string direction = "right";
            bool sawDirection = false;
            uint32 id = 0;
            for (size_t i = 2; i < tokens.size(); ++i)
            {
                std::string word = Lower(tokens[i]);
                if (word == "right" || word == "left" || word == "forward" || word == "back")
                {
                    direction = word;
                    sawDirection = true;
                }
                else if (!sawDirection && spacing == 0.0f)
                    spacing = decimal(i, 0.0f);
                else
                    id = number(i);
            }
            if (!count)
                reason = "Usage: .house row <count> [yards apart] [right|left|forward|back] [id]";
            else
                mgr->PlaceRow(player, id, count, spacing, direction, reason);
        }
        else if (sub == "set" || sub == "sets")
        {
            std::string what = tokens.size() > 1 ? Lower(tokens[1]) : "list";
            ObjectGuid::LowType self = mgr->HomeOf(player);  // the account's island
            std::optional<SavedSet> set = tokens.size() > 2 ? mgr->FindSavedSet(self, restFrom(2)) : std::nullopt;
            if (what == "save")
                mgr->SaveSet(player, restFrom(2), reason);
            else if ((what == "place" || what == "delete") && !set)
                reason = tokens.size() > 2 ? "You have no set called " + restFrom(2) + "." : "Which set? .house set list shows them.";
            else if (what == "place")
                mgr->StartGhostSet(player, set->id, reason);
            else if (what == "delete")
                mgr->DeleteSet(player, set->id, reason);
            else
            {
                std::vector<SavedSet> sets = mgr->GetSavedSets(self);
                if (sets.empty())
                    reason = "No saved sets yet: select pieces (Ctrl-click), then .house set save <name>.";
                for (SavedSet const& saved : sets)
                    handler->PSendSysMessage("#{} {} ({} pieces, {})", saved.id, saved.name, saved.pieces, saved.savedAt);
            }
        }
        else if (sub == "goto")
            mgr->GoTo(player, number(1), reason);
        else if (sub == "sign")
            mgr->SignGuestbook(player, restFrom(1), reason);
        else if (sub == "guestbook")
        {
            ObjectGuid::LowType self = mgr->HomeOf(player);  // the account's island
            if (tokens.size() > 2 && Lower(tokens[1]) == "delete")
                mgr->DeleteNote(player, number(2), reason);
            else
            {
                std::vector<GuestbookNote> notes = mgr->GetGuestbook(self, 10);
                if (notes.empty())
                    reason = "Your guestbook is empty: visitors sign it with .house sign <note>.";
                for (GuestbookNote const& note : notes)
                    handler->PSendSysMessage("#{} {}, {}{}: {}", note.id, note.author, note.when, note.fresh ? " (new)" : "", note.text);
                mgr->MarkGuestbookRead(self);
            }
        }
        else if (sub == "door")
        {
            std::string what = tokens.size() > 1 ? Lower(tokens[1]) : "here";
            mgr->SetDoor(player, what == "reset" || what == "off" || what == "landing", reason);
        }
        else if (gm && sub == "phototour")
            mgr->PhotoTour(player, tokens.size() > 1 ? Lower(tokens[1]) : "start", reason);
        else if (sub == "grid")
        {
            std::string size = tokens.size() > 1 ? Lower(tokens[1]) : "";
            if (size == "off" || size == "0")
                mgr->SetGridSize(player, 0.0f, reason);
            else if (float yards = decimal(1, 0.0f); yards > 0.0f)
                mgr->SetGridSize(player, yards, reason);
            else if (float current = mgr->GetGridSize(player->GetGUID().GetCounter()); current > 0.0f)
                reason = Acore::StringFormat("The grid is {} yards. Usage: .house grid <off|yards> (0.25 to 4).", PlayerHousingMgr::FormatYards(current));
            else
                reason = "The grid is off. Usage: .house grid <off|yards> (0.25 to 4).";
            mgr->SendAddonState(player);  // the edit mode banner shows it
        }
        else if (sub == "highlight")
        {
            // Quiet: a click on a piece in the Placed list rings it (0: no ring).
            mgr->HighlightPlacement(player, number(1));
            return true;
        }
        else if (sub == "select" && tokens.size() > 1 && (Lower(tokens[1]) == "next" || Lower(tokens[1]) == "previous" || Lower(tokens[1]) == "prev"))
        {
            // Quiet too: Tab in edit mode.
            mgr->SelectNext(player, Lower(tokens[1]) != "next", reason);
        }
        else if (sub == "select")
        {
            uint32 id = number(1);
            if (!id)
            {
                auto nearby = mgr->GetNearbyPlacements(player, 25.0f);
                id = nearby.empty() ? 0 : nearby.front().first.id;
            }
            if (id && mgr->GetPlacement(player, id))
            {
                // Quiet: the addon shows it.
                mgr->SelectPlacement(player, id);
                mgr->SendAddonState(player);
            }
            else
                reason = "No piece with that number nearby.";
        }
        else if (sub == "list")
        {
            auto nearby = mgr->GetNearbyPlacements(player, 40.0f);
            if (nearby.empty())
                reason = "No pieces within 40 yards.";
            for (auto const& [placement, distance] : nearby)
                handler->PSendSysMessage("#{} {} ({:.0f} yd)", placement.id, mgr->GetPiece(placement.itemEntry)->name, distance);
        }
        else if (sub == "collection")
        {
            mgr->OpenWindow(player, "Collection");
            return true;
        }
        else if (sub == "visit")
        {
            if (tokens.size() > 1)
                mgr->VisitHouseByName(player, std::string(tokens[1]), reason);
            else
            {
                mgr->OpenWindow(player, "Visit");
                return true;
            }
        }
        else if (sub == "invite")
        {
            std::string who = tokens.size() > 1 ? std::string(tokens[1]) : "target";
            if (Lower(who) == "target")
                mgr->InviteTarget(player, reason);
            else if (Lower(who) == "party")
                mgr->InviteParty(player, reason);
            else
                mgr->InviteGuestByName(player, who, reason);
        }
        else if (sub == "uninvite")
        {
            if (tokens.size() > 1)
                mgr->RemoveGuestByName(player, std::string(tokens[1]), reason);
            else
                reason = "Usage: .house uninvite <name>";
        }
        else if (sub == "privacy")
        {
            std::string mode = tokens.size() > 1 ? Lower(tokens[1]) : "";
            if (mode == "private")
                mgr->SetPrivacy(player, PRIVACY_PRIVATE, reason);
            else if (mode == "friends" || mode == "guild")
                mgr->SetPrivacy(player, PRIVACY_FRIENDS, reason);
            else if (mode == "public")
                mgr->SetPrivacy(player, PRIVACY_PUBLIC, reason);
            else
                mgr->CyclePrivacy(player, reason);
        }
        else if (sub == "greeting")
        {
            std::string text = restFrom(1);
            mgr->SetGreeting(player, Lower(text) == "clear" ? "" : text, reason);
        }
        else if (gm && (sub == "unlock" || sub == "relock"))
        {
            if (tokens.size() < 2)
                reason = "Usage: .house unlock|relock <item|name|all> [player]";
            else
                mgr->GmUnlock(GmTarget(handler, player, tokens, 2), std::string(tokens[1]), sub == "unlock", reason);
        }
        else if (gm && sub == "unlocks")
        {
            Player* target = GmTarget(handler, player, tokens, 1);
            uint32 unlocked = 0;
            uint32 total = 0;
            mgr->CollectionCounts(target, -1, unlocked, total);
            reason = Acore::StringFormat("{} has {} of {} pieces unlocked.", target->GetName(), unlocked, total);
        }
        else if (gm && sub == "add")
        {
            float angle = player->GetOrientation();
            float x = player->GetPositionX() + std::cos(angle) * 2.5f;
            float y = player->GetPositionY() + std::sin(angle) * 2.5f;
            if (Creature* steward = player->SummonCreature(mgr->GetStewardEntry(), x, y, player->GetPositionZ(), angle + float(M_PI), TEMPSUMMON_TIMED_DESPAWN, 10 * MINUTE * IN_MILLISECONDS))
            {
                EnsureStewardAppearance(steward);
                reason = "Krook will wait here for ten minutes.";
            }
            else
                reason = "Could not summon Krook.";
        }
        else
        {
            SendUsage(handler, gm);
            return true;
        }

        Reply(player, reason);
        return true;
    }
};

class mod_playerhousing_playerscript : public PlayerScript
{
public:
    mod_playerhousing_playerscript() : PlayerScript("mod_playerhousing_playerscript") { }

    void OnPlayerLogin(Player* player) override { sPlayerHousingMgr->OnPlayerLogin(player); }
    void OnPlayerBeforeLogout(Player* player) override { sPlayerHousingMgr->OnPlayerLogout(player); }
    void OnPlayerUpdate(Player* player, uint32 diffMs) override { sPlayerHousingMgr->OnPlayerUpdate(player, diffMs); }
    void OnPlayerMapChanged(Player* player) override { sPlayerHousingMgr->OnPlayerMapChanged(player); }
    void OnPlayerDelete(ObjectGuid guid, uint32 /*accountId*/) override { sPlayerHousingMgr->OnPlayerDelete(guid); }
    void OnPlayerDeleteFromDB(CharacterDatabaseTransaction /*trans*/, uint32 guid) override { sPlayerHousingMgr->OnPlayerDeleteFromDB(guid); }

    // Progress that unlocks pieces for the Collection.
    void OnPlayerAchievementComplete(Player* player, AchievementEntry const* achievement) override
    {
        sPlayerHousingMgr->EvaluateUnlocks(player, RULE_ACHIEVEMENT, achievement->ID, true);
    }

    void OnPlayerReputationRankChange(Player* player, uint32 factionId, ReputationRank newRank, ReputationRank /*oldRank*/, bool increased) override
    {
        if (increased)
            sPlayerHousingMgr->EvaluateUnlocks(player, RULE_REPUTATION, factionId, true, uint32(newRank));
    }

    void OnPlayerCompleteQuest(Player* player, Quest const* quest) override
    {
        if (quest)
            sPlayerHousingMgr->EvaluateUnlocks(player, RULE_QUEST, quest->GetQuestId(), true);
    }

    void OnPlayerCreatureKill(Player* killer, Creature* killed) override
    {
        if (killed)
            sPlayerHousingMgr->OnCreatureKilled(killer, killed->GetEntry());
    }

    void OnPlayerCreatureKilledByPet(Player* owner, Creature* killed) override
    {
        if (killed)
            sPlayerHousingMgr->OnCreatureKilled(owner, killed->GetEntry());
    }

    void OnPlayerUpdateZone(Player* player, uint32 newZone, uint32 /*newArea*/) override
    {
        sPlayerHousingMgr->EvaluateUnlocks(player, RULE_EXPLORE, newZone, true);
    }

    void OnPlayerUpdateArea(Player* player, uint32 /*oldArea*/, uint32 newArea) override
    {
        sPlayerHousingMgr->EvaluateUnlocks(player, RULE_EXPLORE, newArea, true);
    }

    void OnPlayerLevelChanged(Player* player, uint8 /*oldLevel*/) override
    {
        sPlayerHousingMgr->EvaluateUnlocks(player, RULE_LEVEL, 0, true, player->GetLevel());
    }

    void OnPlayerUpdateSkill(Player* player, uint32 skillId, uint32 /*value*/, uint32 /*max*/, uint32 /*step*/, uint32 newValue) override
    {
        sPlayerHousingMgr->EvaluateUnlocks(player, RULE_SKILL, skillId, true, newValue);
    }
};

class mod_playerhousing_globalscript : public GlobalScript
{
public:
    mod_playerhousing_globalscript() : GlobalScript("mod_playerhousing_globalscript", { GLOBALHOOK_ON_BEFORE_WORLDOBJECT_SET_PHASEMASK }) { }

    void OnBeforeWorldObjectSetPhaseMask(WorldObject const* /*worldObject*/, uint32& oldPhaseMask, uint32& newPhaseMask, bool& useCombinedPhases, bool& /*update*/) override
    {
        sPlayerHousingMgr->OnBeforeSetPhaseMask(oldPhaseMask, newPhaseMask, useCombinedPhases);
    }
};

// GM Island's guild hall as an object on the regular map (world SQL), for the cleared layout:
// its client patch takes the hall out of the map tile. On any other layout the map tile still
// has its hall, so this copy is put in a phase nobody is in rather than drawn twice. Read
// straight from the config: the island's cells can load before the module reads it.
class mod_playerhousing_gameobjectscript : public AllGameObjectScript
{
public:
    mod_playerhousing_gameobjectscript() : AllGameObjectScript("mod_playerhousing_gameobjectscript") { }

    void OnGameObjectAddWorld(GameObject* go) override
    {
        if (go->GetEntry() != GUILD_HALL_GO)
            return;
        std::string layout = sConfigMgr->GetOption<std::string>("PlayerHousing.Layout", "cleared");
        std::transform(layout.begin(), layout.end(), layout.begin(), [](unsigned char c) { return std::tolower(c); });
        if (layout != "cleared")
            go->SetPhaseMask(0, true);
    }
};

void AddHomeScreenScripts();

void Addmod_playerhousingScripts()
{
    AddHomeScreenScripts();
    new npc_playerhousing_steward();
    new npc_playerhousing_mannequin();
    new mod_playerhousing_serverscript();
    new item_playerhousing_key();
    new item_playerhousing_piece();
    RegisterSpellScript(spell_playerhousing_place);
    RegisterSpellScript(spell_playerhousing_key);
    new go_playerhousing_piece();
    new npc_playerhousing_figurine();
    new mod_playerhousing_worldscript();
    new mod_playerhousing_commandscript();
    new mod_playerhousing_playerscript();
    new mod_playerhousing_globalscript();
    new mod_playerhousing_gameobjectscript();
}
