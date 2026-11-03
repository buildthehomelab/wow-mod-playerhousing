#include "PlayerHousingMgr.h"

#include "CharacterCache.h"
#include "DatabaseEnv.h"
#include "Log.h"
#include "Player.h"
#include "StringFormat.h"
#include "WorldSession.h"
#include "WorldSessionMgr.h"

// A core with home screens keeps a character row per account that nobody plays. Other cores have none.
#if __has_include("HomeViewer.h")
#include "HomeViewer.h"
#else
#define SQL_NOT_HOME_VIEWER "1"
inline bool IsHomeViewer(Player const*) { return false; }
#endif

#include <vector>

// One island per account. Every character of an account shares it, with its owned pieces,
// layouts, sets, settings, likes and guestbook: they're all kept under the account's home
// character (mod_playerhousing_account), the owner_guid the rest of the module uses. What's a
// player's own stays with each character: undo history, what's selected, the grid, tips, the
// visits they make and the invitations they get.

using namespace Housing;

ObjectGuid::LowType PlayerHousingMgr::HomeOf(Player const* player) const
{
    if (!player || !player->GetSession())
        return 0;
    ObjectGuid::LowType self = player->GetGUID().GetCounter();
    if (player->GetSession()->IsHeadless())
        return self;  // bots never get housing; this keeps them apart from their owner's account
    return HomeOfAccount(player->GetSession()->GetAccountId(), self);
}

Player* PlayerHousingMgr::FindOwnerOnline(ObjectGuid::LowType home) const
{
    // Whichever of the account's characters is playing.
    uint32 account = sCharacterCache->GetCharacterAccountIdByGuid(ObjectGuid::Create<HighGuid::Player>(home));
    WorldSession* session = account ? sWorldSessionMgr->FindSession(account) : nullptr;
    Player* player = session ? session->GetPlayer() : nullptr;
    return player && player->IsInWorld() && !IsHomeViewer(player) ? player : nullptr;
}

ObjectGuid::LowType PlayerHousingMgr::HomeOfCharacter(ObjectGuid::LowType guid) const
{
    uint32 account = sCharacterCache->GetCharacterAccountIdByGuid(ObjectGuid::Create<HighGuid::Player>(guid));
    return account ? HomeOfAccount(account, guid) : guid;
}

ObjectGuid::LowType PlayerHousingMgr::HomeOfAccount(uint32 account, ObjectGuid::LowType fallback) const
{
    std::lock_guard<std::recursive_mutex> guard(_lock);
    auto itr = _homeByAccount.find(account);
    if (itr != _homeByAccount.end())
        return itr->second;

    ObjectGuid::LowType stored = 0;
    if (QueryResult result = CharacterDatabase.Query("SELECT home_guid FROM mod_playerhousing_account WHERE account_id={}", account))
        stored = (*result)[0].Get<uint32>();

    ObjectGuid::LowType home = 0;
    if (stored && CharacterDatabase.Query("SELECT 1 FROM characters WHERE guid={} AND account={} AND " SQL_NOT_HOME_VIEWER, stored, account))
        home = stored;
    else if (stored)
    {
        // Its home character was deleted (or kept aside by the core's undelete): the island
        // moves, as it is, to the character played last.
        if (QueryResult result = CharacterDatabase.Query(
                "SELECT guid FROM characters WHERE account={} AND guid<>{} AND " SQL_NOT_HOME_VIEWER " ORDER BY logout_time DESC LIMIT 1", account, stored))
            home = (*result)[0].Get<uint32>();
        if (!home)
            home = fallback;
        if (home)
        {
            const_cast<PlayerHousingMgr*>(this)->MoveHousing(stored, home, false);
            LOG_INFO("module", "mod-playerhousing: The island of account {} moved from character {} (gone) to {}.", account, stored, home);
        }
    }
    else
    {
        // First time: the character whose island has the most on it, then the one who owns the
        // most pieces, then the one played last.
        if (QueryResult result = CharacterDatabase.Query(
                "SELECT c.guid FROM characters c "
                "LEFT JOIN (SELECT owner_guid, COUNT(*) n FROM mod_playerhousing_placement GROUP BY owner_guid) p ON p.owner_guid = c.guid "
                "LEFT JOIN (SELECT owner_guid, SUM(count) n FROM mod_playerhousing_storage GROUP BY owner_guid) s ON s.owner_guid = c.guid "
                "WHERE c.account={} AND (p.n > 0 OR s.n > 0) "
                "ORDER BY COALESCE(p.n, 0) DESC, COALESCE(s.n, 0) DESC, c.logout_time DESC LIMIT 1", account))
            home = (*result)[0].Get<uint32>();
        if (!home)
            home = fallback;
    }
    if (!home)
        return 0;
    if (home != stored)
        CharacterDatabase.DirectExecute("REPLACE INTO mod_playerhousing_account (account_id, home_guid) VALUES ({}, {})", account, home);

    _homeByAccount[account] = home;
    return home;
}

void PlayerHousingMgr::MoveHousing(ObjectGuid::LowType from, ObjectGuid::LowType to, bool packUp)
{
    if (!from || !to || from == to)
        return;

    std::lock_guard<std::recursive_mutex> guard(_lock);

    // Pieces it owns join the other's counts, one kind at a time (an INSERT ... SELECT from the
    // same table with ON DUPLICATE KEY UPDATE fails on MySQL 8, and the counts were lost).
    for (auto const& [itemEntry, count] : GetStorage(from))
        AddToStorage(to, itemEntry, int32(count));
    CharacterDatabase.DirectExecute("DELETE FROM mod_playerhousing_storage WHERE owner_guid={}", from);

    if (packUp)
    {
        // Two islands become one: the other island's pieces are put away (into the shared
        // counts), and what mannequins wore goes back to whoever dressed them.
        std::map<uint32, Placement> placements;
        if (QueryResult result = CharacterDatabase.Query(
                "SELECT placement_id, source_item_entry, placed_by FROM mod_playerhousing_placement WHERE owner_guid={}", from))
        {
            do
            {
                Placement placement;
                placement.id = (*result)[0].Get<uint32>();
                placement.itemEntry = (*result)[1].Get<uint32>();
                placement.placedBy = (*result)[2].Get<uint32>();
                placements[placement.id] = placement;
            } while (result->NextRow());
        }
        LoadGear(from, placements);
        for (auto const& [id, placement] : placements)
        {
            for (auto const& [slot, gear] : placement.gear)
                ReturnGear(nullptr, from, placement.placedBy ? placement.placedBy : from, id, slot, gear);
            if (_pieces.count(placement.itemEntry))
                AddToStorage(placement.placedBy ? HomeOfCharacter(placement.placedBy) : to, placement.itemEntry, 1);
        }
        CharacterDatabase.DirectExecute("DELETE FROM mod_playerhousing_placement WHERE owner_guid={}", from);
        CharacterDatabase.DirectExecute("DELETE FROM mod_playerhousing_house WHERE owner_guid={}", from);
    }
    else
    {
        // The island itself moves over, as it is (its home character was deleted).
        CharacterDatabase.DirectExecute("DELETE FROM mod_playerhousing_house WHERE owner_guid={}", to);
        CharacterDatabase.DirectExecute("UPDATE mod_playerhousing_house SET owner_guid={} WHERE owner_guid={}", to, from);
        CharacterDatabase.DirectExecute("DELETE FROM mod_playerhousing_placement WHERE owner_guid={}", to);
        CharacterDatabase.DirectExecute("UPDATE mod_playerhousing_placement SET owner_guid={} WHERE owner_guid={}", to, from);
        CharacterDatabase.DirectExecute("UPDATE mod_playerhousing_placement_gear SET owner_guid={} WHERE owner_guid={}", to, from);
    }
    // Pieces it put on roommates' islands are the account's.
    CharacterDatabase.DirectExecute("UPDATE mod_playerhousing_placement SET placed_by={} WHERE placed_by={}", to, from);

    // Saved layouts and sets keep their names, with new numbers.
    std::vector<uint32> layouts;
    if (QueryResult result = CharacterDatabase.Query("SELECT layout_id FROM mod_playerhousing_saved_layout WHERE owner_guid={} ORDER BY layout_id", from))
        do layouts.push_back((*result)[0].Get<uint32>()); while (result->NextRow());
    for (uint32 layoutId : layouts)
    {
        uint32 newId = NextLayoutId(to);
        CharacterDatabase.DirectExecute("UPDATE mod_playerhousing_saved_piece SET owner_guid={}, layout_id={} WHERE owner_guid={} AND layout_id={}",
            to, newId, from, layoutId);
        CharacterDatabase.DirectExecute("UPDATE mod_playerhousing_saved_layout SET owner_guid={}, layout_id={} WHERE owner_guid={} AND layout_id={}",
            to, newId, from, layoutId);
    }
    std::vector<uint32> sets;
    if (QueryResult result = CharacterDatabase.Query("SELECT set_id FROM mod_playerhousing_set WHERE owner_guid={} ORDER BY set_id", from))
        do sets.push_back((*result)[0].Get<uint32>()); while (result->NextRow());
    for (uint32 setId : sets)
    {
        uint32 newId = 1;
        if (QueryResult result = CharacterDatabase.Query("SELECT COALESCE(MAX(set_id), 0) + 1 FROM mod_playerhousing_set WHERE owner_guid={}", to))
            newId = uint32((*result)[0].Get<uint64>());
        CharacterDatabase.DirectExecute("UPDATE mod_playerhousing_set_piece SET owner_guid={}, set_id={} WHERE owner_guid={} AND set_id={}",
            to, newId, from, setId);
        CharacterDatabase.DirectExecute("UPDATE mod_playerhousing_set SET owner_guid={}, set_id={} WHERE owner_guid={} AND set_id={}",
            to, newId, from, setId);
    }

    // Guests and roommates, likes, notes, visits and reports: the island's.
    CharacterDatabase.DirectExecute(
        "INSERT IGNORE INTO mod_playerhousing_acl (owner_guid, guest_guid, roommate) SELECT {}, guest_guid, roommate FROM mod_playerhousing_acl "
        "WHERE owner_guid={} AND guest_guid<>{}", to, from, to);
    CharacterDatabase.DirectExecute("DELETE FROM mod_playerhousing_acl WHERE owner_guid={}", from);
    CharacterDatabase.DirectExecute("UPDATE IGNORE mod_playerhousing_like SET owner_guid={} WHERE owner_guid={}", to, from);
    CharacterDatabase.DirectExecute("DELETE FROM mod_playerhousing_like WHERE owner_guid={}", from);
    CharacterDatabase.DirectExecute("UPDATE mod_playerhousing_guestbook SET owner_guid={} WHERE owner_guid={}", to, from);
    CharacterDatabase.DirectExecute("UPDATE mod_playerhousing_visit_log SET owner_guid={} WHERE owner_guid={}", to, from);
    CharacterDatabase.DirectExecute("UPDATE mod_playerhousing_report SET owner_guid={} WHERE owner_guid={}", to, from);

    _knownHouses.erase(from);
    _knownHouses.erase(to);
}

void PlayerHousingMgr::MergeAccountHomes()
{
    // Once: islands used to be one per character. Each account keeps the island with the most
    // on it; the others are put away into its counts, their layouts, sets, guests, likes and
    // notes join it.
    if (CharacterDatabase.Query("SELECT 1 FROM mod_playerhousing_meta WHERE meta_key='account_homes'"))
        return;

    std::map<uint32, std::vector<ObjectGuid::LowType>> byAccount;
    if (QueryResult result = CharacterDatabase.Query(
            "SELECT DISTINCT c.account, c.guid FROM characters c JOIN ("
            "SELECT owner_guid AS guid FROM mod_playerhousing_house UNION SELECT owner_guid FROM mod_playerhousing_placement "
            "UNION SELECT owner_guid FROM mod_playerhousing_storage UNION SELECT owner_guid FROM mod_playerhousing_saved_layout "
            "UNION SELECT owner_guid FROM mod_playerhousing_set UNION SELECT owner_guid FROM mod_playerhousing_acl "
            "UNION SELECT owner_guid FROM mod_playerhousing_guestbook) h ON h.guid = c.guid"))
    {
        do
            byAccount[(*result)[0].Get<uint32>()].push_back((*result)[1].Get<uint32>());
        while (result->NextRow());
    }

    uint32 merged = 0;
    for (auto const& [account, characters] : byAccount)
    {
        ObjectGuid::LowType home = HomeOfAccount(account, characters.front());
        for (ObjectGuid::LowType character : characters)
        {
            if (character == home)
                continue;
            MoveHousing(character, home, true);
            ++merged;
        }
    }
    CharacterDatabase.DirectExecute("REPLACE INTO mod_playerhousing_meta (meta_key, meta_value) VALUES ('account_homes', 1)");
    LOG_INFO("server.loading", "mod-playerhousing: One island per account: {} accounts, {} other characters' islands put away into their account's.",
        byAccount.size(), merged);
}

bool PlayerHousingMgr::RehomeAccountOf(ObjectGuid::LowType deleted)
{
    // The account's home character is gone for good: the island goes to another character of
    // the account, as it is. False when it wasn't a home, or there's no one left (then the
    // account's housing goes with it).
    QueryResult result = CharacterDatabase.Query("SELECT account_id FROM mod_playerhousing_account WHERE home_guid={}", deleted);
    if (!result)
        return false;
    uint32 account = (*result)[0].Get<uint32>();

    ObjectGuid::LowType next = 0;
    if (QueryResult other = CharacterDatabase.Query(
            "SELECT guid FROM characters WHERE account={} AND guid<>{} AND " SQL_NOT_HOME_VIEWER " ORDER BY logout_time DESC LIMIT 1", account, deleted))
        next = (*other)[0].Get<uint32>();

    std::lock_guard<std::recursive_mutex> guard(_lock);
    _homeByAccount.erase(account);
    if (!next)
    {
        CharacterDatabase.DirectExecute("DELETE FROM mod_playerhousing_account WHERE account_id={}", account);
        return false;
    }

    MoveHousing(deleted, next, false);
    CharacterDatabase.DirectExecute("REPLACE INTO mod_playerhousing_account (account_id, home_guid) VALUES ({}, {})", account, next);
    _homeByAccount[account] = next;
    LOG_INFO("module", "mod-playerhousing: The island of account {} moved from deleted character {} to {}.", account, deleted, next);
    return true;
}
