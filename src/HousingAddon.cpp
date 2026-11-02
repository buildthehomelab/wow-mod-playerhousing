#include "PlayerHousingMgr.h"

#include "Chat.h"
#include "DatabaseEnv.h"
#include "Item.h"
#include "Opcodes.h"
#include "Player.h"
#include "StringFormat.h"
#include "WorldPacket.h"
#include "WorldSession.h"

#include <algorithm>
#include <cstring>
#include <set>

using namespace Housing;

// The client addon's housing window. Lists go to it as addon whispers with the prefix
// HOUSING, one row a message, so none gets near the client's 255 character limit:
//   begin <kind>, then row <kind> <fields...> for each row, then end <kind>
// Fields are tab separated; text fields have their tabs taken out.

namespace
{
    constexpr size_t ROW_LIMIT = 200;   // characters of list data in one row
    // The most an addon message can be, prefix, tab and text together: the client takes no
    // more than 255 bytes.
    constexpr size_t ADDON_MESSAGE_LIMIT = 255;
    constexpr char const* ADDON_PREFIX = "HOUSING\t";
    constexpr size_t PLACED_ROWS = 250;
    constexpr size_t RECENT_PIECES = 12;
    constexpr uint32 HISTORY_ROWS = 15;
    constexpr uint32 GUESTBOOK_ROWS = 30;

    std::string Clean(std::string text)
    {
        std::replace(text.begin(), text.end(), '\t', ' ');
        std::replace(text.begin(), text.end(), '\n', ' ');
        return text;
    }

    // Runs of item entries: 901100-901110,902001
    std::vector<std::string> Ranges(std::vector<uint32> entries)
    {
        std::sort(entries.begin(), entries.end());
        std::vector<std::string> parts;
        for (size_t i = 0; i < entries.size();)
        {
            size_t j = i;
            while (j + 1 < entries.size() && entries[j + 1] == entries[j] + 1)
                ++j;
            parts.push_back(j > i ? Acore::StringFormat("{}-{}", entries[i], entries[j]) : std::to_string(entries[i]));
            i = j + 1;
        }
        return parts;
    }
}

void PlayerHousingMgr::SendAddon(Player* player, std::string const& text) const
{
    if (!player || !player->GetSession() || player->GetSession()->IsHeadless())
        return;

    // A whisper to themselves, always the plain kind: for a GM in GM mode the core would send
    // the GM variant, which the test client (and, it may be, some addons) doesn't read.
    // Never over the client's limit (rows with free text split themselves before this; this
    // only guards against one that doesn't).
    std::string message = ADDON_PREFIX + text;
    TruncateUtf8(message, ADDON_MESSAGE_LIMIT);
    WorldPacket data;
    ChatHandler::BuildChatPacket(data, CHAT_MSG_WHISPER, LANG_ADDON, player->GetGUID(), player->GetGUID(), message, 0);
    player->SendDirectMessage(&data);
}

void PlayerHousingMgr::SendAddonRows(Player* player, std::string const& kind, std::string const& label, std::vector<std::string> const& parts) const
{
    // Comma lists over several rows when long.
    std::string row;
    for (std::string const& part : parts)
    {
        if (!row.empty() && row.size() + part.size() + 1 > ROW_LIMIT)
        {
            SendAddon(player, "row\t" + kind + "\t" + label + "\t" + row);
            row.clear();
        }
        row += (row.empty() ? "" : ",") + part;
    }
    if (!row.empty())
        SendAddon(player, "row\t" + kind + "\t" + label + "\t" + row);
}

void PlayerHousingMgr::SetAddonClient(Player* player, bool mouse, bool local)
{
    std::lock_guard<std::recursive_mutex> guard(_lock);
    _addonClients.insert(player->GetGUID());
    if (mouse)
        _mouseClients.insert(player->GetGUID());
    else
        _mouseClients.erase(player->GetGUID());
    if (mouse && local)
        _localGhostClients.insert(player->GetGUID());
    else
        _localGhostClients.erase(player->GetGUID());
}

bool PlayerHousingMgr::HasAddon(Player const* player) const
{
    std::lock_guard<std::recursive_mutex> guard(_lock);
    return _addonClients.count(player->GetGUID()) > 0;
}

bool PlayerHousingMgr::HasMouse(Player const* player) const
{
    std::lock_guard<std::recursive_mutex> guard(_lock);
    return _mouseClients.count(player->GetGUID()) > 0;
}

bool PlayerHousingMgr::HasLocalGhosts(Player const* player) const
{
    std::lock_guard<std::recursive_mutex> guard(_lock);
    return _localGhostClients.count(player->GetGUID()) > 0;
}

void PlayerHousingMgr::OpenWindow(Player* player, std::string const& tab) const
{
    if (HasAddon(player))
    {
        SendAddon(player, tab.empty() ? std::string("open") : "open\t" + tab);
        return;
    }
    // Without the addon there's no window: housing is the addon's (Interface/AddOns/PlayerHousing
    // in the module's client-addon folder), or .house commands typed.
    Say(player, "Housing happens in the Player Housing addon's window, and this client doesn't have the addon. "
        "Copy the PlayerHousing folder into Interface/AddOns, or type .house help for the commands.");
}

bool PlayerHousingMgr::SendAddonData(Player* player, std::string const& kind, std::string const& argument, std::string& reason)
{
    ObjectGuid::LowType self = HomeOf(player);  // the account's island

    if (kind == "collection")
    {
        // Old furnishing items in the bags or the bank join the counts first.
        SweepHousingItems(player, true);
        SendAddon(player, "begin\tcollection");
        SendAddon(player, Acore::StringFormat("row\tcollection\tsettings\t{}\t{}\t{}\t{}\t{}", _freeMode ? 1 : 0, _catalogEverything ? 1 : 0,
            _maxFurnishings, _maxBuildings, _unlockAll ? 1 : 0));

        std::set<uint32> known = LoadUnlocks(player);
        std::vector<uint32> unlocked;
        {
            std::lock_guard<std::recursive_mutex> guard(_lock);
            for (auto const& [itemEntry, piece] : _pieces)
                if (IsUnlocked(player, piece, &known))
                    unlocked.push_back(itemEntry);
        }
        SendAddonRows(player, "collection", "unlocked", Ranges(unlocked));

        std::set<uint32> fresh = LoadNewUnlocks(player);
        SendAddonRows(player, "collection", "new", Ranges(std::vector<uint32>(fresh.begin(), fresh.end())));

        std::vector<std::string> stored;
        for (auto const& [itemEntry, count] : GetStorage(self))
            stored.push_back(Acore::StringFormat("{}:{}", itemEntry, count));
        SendAddonRows(player, "collection", "storage", stored);

        std::vector<std::string> placed;
        if (QueryResult result = CharacterDatabase.Query(
                "SELECT source_item_entry, COUNT(*) FROM mod_playerhousing_placement WHERE owner_guid={} GROUP BY source_item_entry", self))
        {
            do
            {
                placed.push_back(Acore::StringFormat("{}:{}", (*result)[0].Get<uint32>(), (*result)[1].Get<uint64>()));
            } while (result->NextRow());
        }
        SendAddonRows(player, "collection", "placed", placed);

        // The last pieces placed on the player's island, newest first.
        std::vector<std::string> recent;
        if (QueryResult result = CharacterDatabase.Query(
                "SELECT source_item_entry FROM mod_playerhousing_placement WHERE owner_guid={} AND map_id={} ORDER BY placement_id DESC LIMIT 80",
                self, _layout.mapId))
        {
            std::set<uint32> seen;
            do
            {
                uint32 itemEntry = (*result)[0].Get<uint32>();
                if (itemEntry && seen.insert(itemEntry).second)
                    recent.push_back(std::to_string(itemEntry));
            } while (result->NextRow() && recent.size() < RECENT_PIECES);
        }
        SendAddonRows(player, "collection", "recent", recent);
        SendAddon(player, "end\tcollection");
        return true;
    }

    if (kind == "placed")
    {
        // The island the player is on, nearest first.
        if (!CanDecorate(player))
        {
            reason = "Go home (or to an island where you're a roommate) to list its pieces.";
            return false;
        }
        std::vector<std::pair<Placement, float>> nearby = GetNearbyPlacements(player, 100000.0f);
        SendAddon(player, "begin\tplaced");
        for (size_t i = 0; i < nearby.size() && i < PLACED_ROWS; ++i)
            SendAddon(player, Acore::StringFormat("row\tplaced\t{}\t{}\t{:.1f}", nearby[i].first.id, nearby[i].first.itemEntry, nearby[i].second));
        SendAddon(player, Acore::StringFormat("end\tplaced\t{}", nearby.size()));
        return true;
    }

    if (kind == "layouts")
    {
        SendAddon(player, "begin\tlayouts");
        SendAddon(player, Acore::StringFormat("row\tlayouts\tlimit\t{}\t{}", _maxSavedLayouts, IsLayoutCopyable(self) ? 1 : 0));
        // Each with what setting it out would still need: pieces to buy (and what they cost) and
        // pieces still locked.
        for (SavedLayout const& layout : GetSavedLayouts(self))
        {
            uint32 gettable = 0;
            uint64 cost = 0;
            uint32 locked = 0;
            DescribeShortfall(player, LayoutShortfall(player, layout.id), gettable, cost, locked);
            SendAddon(player, Acore::StringFormat("row\tlayouts\tlayout\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}", layout.id, Clean(layout.name),
                layout.pieces, layout.savedAt, Clean(layout.source), gettable, cost, locked));
        }
        SendAddon(player, "end\tlayouts");
        return true;
    }

    if (kind == "stand")
    {
        // A mannequin on the island: what it wears and its figure.
        uint32 placementId = Acore::StringTo<uint32>(argument).value_or(0);
        std::optional<Placement> placement = placementId ? GetPlacement(player, placementId) : std::nullopt;
        PieceDefinition const* piece = placement ? GetPiece(placement->itemEntry) : nullptr;
        if (!piece || !piece->HasFlag(PIECE_FLAG_STAND) || !CanDecorate(player))
        {
            reason = "That isn't a mannequin on an island you can decorate.";
            return false;
        }
        SendAddon(player, Acore::StringFormat("begin\tstand\t{}", placementId));
        SendAddon(player, Acore::StringFormat("row\tstand\tfigure\t{}", Clean(LookName(placement->look))));
        // Its race (bits 0-3) and gender (bit 8), for the sheet.
        SendAddon(player, Acore::StringFormat("row\tstand\tlook\t{}", placement->look));
        for (auto const& [slot, gear] : placement->gear)
            SendAddon(player, Acore::StringFormat("row\tstand\tworn\t{}\t{}\t{}\t{}", uint32(slot), Clean(StandSlotName(slot)), gear.itemEntry,
                Clean(StandItemName(gear.itemEntry))));
        SendAddon(player, Acore::StringFormat("end\tstand\t{}", placementId));
        return true;
    }

    if (kind == "guests")
    {
        SendAddon(player, "begin\tguests");
        for (VisitEntry const& guest : GetGuests(self))
            SendAddon(player, Acore::StringFormat("row\tguests\tguest\t{}\t{}", Clean(guest.ownerName), guest.roommate ? 1 : 0));
        SendAddon(player, "end\tguests");
        return true;
    }

    if (kind == "visits")
    {
        uint8 list = uint8(std::min<uint32>(Acore::StringTo<uint32>(argument).value_or(0), 5));
        SendAddon(player, Acore::StringFormat("begin\tvisits\t{}", list));
        for (VisitEntry const& entry : GetVisitList(player, list))
            SendAddon(player, Acore::StringFormat("row\tvisits\tisland\t{}\t{}", Clean(entry.ownerName), entry.likes));
        SendAddon(player, Acore::StringFormat("end\tvisits\t{}", list));
        return true;
    }

    if (kind == "island")
    {
        EnsureHouse(self);
        HouseRecord house;
        GetHouseRecord(self, house);
        SendAddon(player, "begin\tisland");
        SendAddon(player, Acore::StringFormat("row\tisland\tsettings\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}", house.privacy, house.weather,
            house.timeOfDay, house.music, HasMusicBox(self) ? 1 : 0, CountLikes(self), CountVisitorsThisWeek(self), house.hasDoor ? 1 : 0,
            CountNewNotes(self)));
        SendAddon(player, "row\tisland\tgreeting\t" + Clean(house.greeting));
        for (uint8 i = 0; i < WeatherCount(); ++i)
            SendAddon(player, Acore::StringFormat("row\tisland\tweather\t{}\t{}", i, WeatherName(i)));
        for (uint8 i = 0; i < TimeOfDayCount(); ++i)
            SendAddon(player, Acore::StringFormat("row\tisland\ttime\t{}\t{}", i, TimeOfDayName(i)));
        for (auto const& [soundId, name] : MusicTracks())
            SendAddon(player, Acore::StringFormat("row\tisland\tmusic\t{}\t{}", soundId, name));
        SendAddon(player, "end\tisland");
        return true;
    }

    if (kind == "history")
    {
        // Undo and redo, newest first: the addon's list under its Undo button.
        SendAddon(player, "begin\thistory");
        for (std::string const& label : JournalLabels(player, false, HISTORY_ROWS))
            SendAddon(player, "row\thistory\tundo\t" + Clean(label));
        for (std::string const& label : JournalLabels(player, true, HISTORY_ROWS))
            SendAddon(player, "row\thistory\tredo\t" + Clean(label));
        SendAddon(player, "end\thistory");
        return true;
    }

    if (kind == "sets")
    {
        SendAddon(player, "begin\tsets");
        SendAddon(player, Acore::StringFormat("row\tsets\tlimit\t{}", GetMaxSavedSets()));
        for (SavedSet const& set : GetSavedSets(self))
            SendAddon(player, Acore::StringFormat("row\tsets\tset\t{}\t{}\t{}\t{}", set.id, Clean(set.name), set.pieces, set.savedAt));
        SendAddon(player, "end\tsets");
        return true;
    }

    if (kind == "guestbook")
    {
        // The owner reading it marks the notes read.
        SendAddon(player, "begin\tguestbook");
        for (GuestbookNote const& note : GetGuestbook(self, GUESTBOOK_ROWS))
        {
            // A long note doesn't fit one message with its author and date: what doesn't fit
            // follows in "more" rows, which the addon adds back on.
            std::string head = Acore::StringFormat("row\tguestbook\tnote\t{}\t{}\t{}\t{}\t", note.id, Clean(note.author), note.when,
                note.fresh ? 1 : 0);
            std::string rest = Clean(note.text);
            do
            {
                size_t room = ADDON_MESSAGE_LIMIT - std::strlen(ADDON_PREFIX) - std::min(head.size(), ADDON_MESSAGE_LIMIT / 2);
                std::string piece = rest;
                TruncateUtf8(piece, room);
                if (piece.empty() && !rest.empty())
                    break;  // can't happen with notes of whole characters; never loop forever
                SendAddon(player, head + piece);
                rest.erase(0, piece.size());
                head = Acore::StringFormat("row\tguestbook\tmore\t{}\t", note.id);
            } while (!rest.empty());
        }
        SendAddon(player, "end\tguestbook");
        MarkGuestbookRead(self);
        return true;
    }

    reason = "Usage: .house data <collection|placed|layouts|guests|visits <list>|island|history|sets|guestbook|stand <id>>";
    return false;
}

void PlayerHousingMgr::MarkAllSeen(Player* player) const
{
    std::set<uint32> fresh = LoadNewUnlocks(player);
    MarkSeen(player, std::vector<uint32>(fresh.begin(), fresh.end()));
}

void PlayerHousingMgr::SendFigurineCreatures(Player* player) const
{
    std::set<uint32> entries;
    {
        std::lock_guard<std::recursive_mutex> guard(_lock);
        for (auto const& [itemEntry, piece] : _pieces)
        {
            if (!piece.HasFlag(PIECE_FLAG_FIGURE))
                continue;
            for (PieceRule const& rule : piece.rules)
                if (rule.type == RULE_KILL && rule.param1)
                    entries.insert(rule.param1);
        }
    }
    // The core's own answer to the client asking about a creature, sent unasked: the client
    // keeps it in its creature cache like any other.
    for (uint32 entry : entries)
    {
        WorldPacket query(CMSG_CREATURE_QUERY, 12);
        query << uint32(entry) << ObjectGuid::Empty;
        player->GetSession()->HandleCreatureQueryOpcode(query);
    }
}
