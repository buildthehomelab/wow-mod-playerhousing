#include "PlayerHousingMgr.h"

#include "CharacterCache.h"
#include "Chat.h"
#include "DatabaseEnv.h"
#include "Group.h"
#include "Guild.h"
#include "GuildMgr.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "StringFormat.h"
#include "WorldSession.h"

#include <algorithm>

using namespace Housing;

namespace
{
    constexpr uint32 SOCIAL_FLAG_FRIEND = 0x01;
    constexpr size_t MAX_GREETING = 200;
    constexpr size_t MAX_NOTE = 200;
    constexpr uint32 NOTES_KEPT = 100;  // per island; the oldest go

    // Printable, no chat codes, trimmed, not too long.
    std::string CleanNote(std::string const& text)
    {
        std::string clean;
        for (char c : text)
            if (static_cast<unsigned char>(c) >= 32 && c != '|' && c != 127)
                clean += c;
        size_t begin = clean.find_first_not_of(' ');
        if (begin == std::string::npos)
            return "";
        clean = clean.substr(begin, clean.find_last_not_of(' ') - begin + 1);
        PlayerHousingMgr::TruncateUtf8(clean, MAX_NOTE);
        return clean;
    }

    char const* PrivacyDescription(uint8 privacy)
    {
        switch (privacy)
        {
            case PRIVACY_PUBLIC: return "anyone can visit";
            case PRIVACY_FRIENDS: return "your friends, your guild and your guests can visit";
            default: return "only you and your guests can visit";
        }
    }
}

char const* PlayerHousingMgr::PrivacyName(uint8 privacy)
{
    switch (privacy)
    {
        case PRIVACY_PUBLIC: return "Public";
        case PRIVACY_FRIENDS: return "Friends & guild";
        default: return "Private";
    }
}

bool PlayerHousingMgr::GetHouseRecord(ObjectGuid::LowType ownerGuid, HouseRecord& outRecord) const
{
    // A deleted character's island is closed (even while a GM could still restore them).
    if (!sCharacterCache->GetCharacterCacheByGuid(ObjectGuid::Create<HighGuid::Player>(ownerGuid)))
        return false;

    QueryResult result = CharacterDatabase.Query(
        "SELECT owner_guid, is_private, flags, greeting, weather, time_of_day, music, door_set, door_x, door_y, door_z, door_o "
        "FROM mod_playerhousing_house WHERE owner_guid={}", ownerGuid);
    if (!result)
        return false;

    Field* fields = result->Fetch();
    outRecord.ownerGuid = fields[0].Get<uint32>();
    outRecord.privacy = std::min<uint8>(fields[1].Get<uint8>(), PRIVACY_FRIENDS);
    outRecord.flags = fields[2].Get<uint32>();
    outRecord.greeting = fields[3].Get<std::string>();
    outRecord.weather = fields[4].Get<uint8>();
    outRecord.timeOfDay = fields[5].Get<uint8>();
    outRecord.music = fields[6].Get<uint32>();
    outRecord.hasDoor = fields[7].Get<uint8>() != 0;
    outRecord.door.Relocate(fields[8].Get<float>(), fields[9].Get<float>(), fields[10].Get<float>(), fields[11].Get<float>());
    return true;
}

bool PlayerHousingMgr::EnsureHouse(ObjectGuid::LowType ownerGuid) const
{
    // Written once a login; trips home and menus after that skip the write.
    {
        std::lock_guard<std::recursive_mutex> guard(_lock);
        if (_knownHouses.count(ownerGuid))
            return true;
    }

    // Synchronous, so a read right after sees the row.
    CharacterDatabase.DirectExecute(
        "INSERT IGNORE INTO mod_playerhousing_house (owner_guid, style_id, stage, is_private) VALUES ({}, 1, 0, {})",
        ownerGuid, uint32(_defaultPrivacy));
    std::lock_guard<std::recursive_mutex> guard(_lock);
    _knownHouses.insert(ownerGuid);
    return true;
}

bool PlayerHousingMgr::CanVisit(Player const* visitor, HouseRecord const& house, std::string& reason, VisitContext const* context) const
{
    ObjectGuid::LowType visitorGuid = visitor->GetGUID().GetCounter();
    if (HomeOf(visitor) == house.ownerGuid)
        return true;

    if (_gmVisitBypass && visitor->IsGameMaster())
        return true;

    bool hidden = house.flags & HOUSE_FLAG_HIDDEN;
    if (house.privacy == PRIVACY_PUBLIC && !hidden)
        return true;

    if (context ? context->guestOf.count(house.ownerGuid) > 0
                : bool(CharacterDatabase.Query("SELECT 1 FROM mod_playerhousing_acl WHERE owner_guid={} AND guest_guid={}", house.ownerGuid, visitorGuid)))
        return true;

    if (hidden)
    {
        reason = "That island is closed to visitors for now.";
        return false;
    }

    if (house.privacy == PRIVACY_FRIENDS)
    {
        // Friends and guild of any of the owner's characters (the island is the account's).
        if (context ? context->friendOf.count(house.ownerGuid) > 0
                    : bool(CharacterDatabase.Query(
                          "SELECT 1 FROM character_social s JOIN characters c ON c.guid = s.guid JOIN mod_playerhousing_account a ON a.account_id = c.account "
                          "WHERE a.home_guid={} AND s.friend={} AND (s.flags & {}) LIMIT 1", house.ownerGuid, visitorGuid, SOCIAL_FLAG_FRIEND)))
            return true;

        if (uint32 guildId = visitor->GetGuildId())
            if (CharacterDatabase.Query(
                    "SELECT 1 FROM guild_member gm JOIN characters c ON c.guid = gm.guid JOIN mod_playerhousing_account a ON a.account_id = c.account "
                    "WHERE a.home_guid={} AND gm.guildid={} LIMIT 1", house.ownerGuid, guildId))
                return true;

        reason = "That island is open to its owner's friends and guild only.";
        return false;
    }

    reason = "That island is private, and you're not on its guest list.";
    return false;
}

bool PlayerHousingMgr::SetPrivacy(Player* player, uint8 privacy, std::string& reason)
{
    if (!player || privacy > PRIVACY_FRIENDS)
    {
        reason = "Unknown privacy setting.";
        return false;
    }

    ObjectGuid::LowType owner = HomeOf(player);
    EnsureHouse(owner);
    CharacterDatabase.DirectExecute("UPDATE mod_playerhousing_house SET is_private={} WHERE owner_guid={}", uint32(privacy), owner);
    reason = Acore::StringFormat("Your island is now {}: {}.", PrivacyName(privacy), PrivacyDescription(privacy));
    if (privacy != PRIVACY_PRIVATE)
        QuestEvent(player, QUEST_TOUR_OPEN);
    return true;
}

bool PlayerHousingMgr::CyclePrivacy(Player* player, std::string& reason)
{
    HouseRecord house;
    uint8 current = _defaultPrivacy;
    if (GetHouseRecord(HomeOf(player), house))
        current = house.privacy;

    uint8 next = current == PRIVACY_PRIVATE ? PRIVACY_FRIENDS : (current == PRIVACY_FRIENDS ? PRIVACY_PUBLIC : PRIVACY_PRIVATE);
    return SetPrivacy(player, next, reason);
}

bool PlayerHousingMgr::InviteGuest(Player* player, ObjectGuid::LowType guestGuid, std::string& reason)
{
    ObjectGuid::LowType owner = HomeOf(player);
    if (HomeOfCharacter(guestGuid) == owner)
    {
        reason = "You can always visit your own island (all your characters share it).";
        return false;
    }

    EnsureHouse(owner);
    CharacterDatabase.DirectExecute("INSERT IGNORE INTO mod_playerhousing_acl (owner_guid, guest_guid) VALUES ({}, {})", owner, guestGuid);

    std::string guestName = NameOf(guestGuid);
    if (Player* guest = ObjectAccessor::FindPlayerByLowGUID(guestGuid); guest && MayNotify(player, guestGuid, NOTICE_INVITE))
        Say(guest, Acore::StringFormat("{} invited you to their island. The housing window's Visit tab, Invited.", player->GetName()));

    reason = Acore::StringFormat("Invited {}. They can visit any time.", guestName);
    QuestEvent(player, QUEST_TOUR_OPEN);
    return true;
}

bool PlayerHousingMgr::InviteGuestByName(Player* player, std::string const& name, std::string& reason)
{
    ObjectGuid::LowType guestGuid = 0;
    std::string normalized;
    if (!ResolvePlayerGuid(name, guestGuid, normalized))
    {
        reason = "No character with that name.";
        return false;
    }

    return InviteGuest(player, guestGuid, reason);
}

bool PlayerHousingMgr::InviteTarget(Player* player, std::string& reason)
{
    Player* target = player->GetSelectedPlayer();
    if (!target || target == player)
    {
        reason = "Target the player you want to invite first.";
        return false;
    }

    return InviteGuest(player, target->GetGUID().GetCounter(), reason);
}

bool PlayerHousingMgr::InviteParty(Player* player, std::string& reason)
{
    Group* group = player->GetGroup();
    if (!group)
    {
        reason = "You're not in a party.";
        return false;
    }

    uint32 invited = 0;
    for (Group::MemberSlot const& slot : group->GetMemberSlots())
    {
        if (slot.guid == player->GetGUID())
            continue;

        std::string ignored;
        if (InviteGuest(player, slot.guid.GetCounter(), ignored))
            ++invited;
    }

    reason = Acore::StringFormat("Invited {} party {}.", invited, invited == 1 ? "member" : "members");
    return invited > 0;
}

bool PlayerHousingMgr::RemoveGuest(Player* player, ObjectGuid::LowType guestGuid, std::string& reason)
{
    ObjectGuid::LowType self = HomeOf(player);  // the account's island
    CharacterDatabase.DirectExecute("DELETE FROM mod_playerhousing_acl WHERE owner_guid={} AND guest_guid={}", self, guestGuid);
    {
        std::lock_guard<std::recursive_mutex> guard(_lock);
        auto sessionItr = _sessionsByOwner.find(self);
        if (sessionItr != _sessionsByOwner.end())
            sessionItr->second.roommates.erase(guestGuid);
    }
    reason = Acore::StringFormat("Removed {} from your guest list.", NameOf(guestGuid));
    return true;
}

bool PlayerHousingMgr::IsRoommate(ObjectGuid::LowType ownerGuid, ObjectGuid::LowType guid) const
{
    return bool(CharacterDatabase.Query("SELECT 1 FROM mod_playerhousing_acl WHERE owner_guid={} AND guest_guid={} AND roommate=1", ownerGuid, guid));
}

bool PlayerHousingMgr::SetRoommate(Player* owner, ObjectGuid::LowType guestGuid, bool roommate, std::string& reason)
{
    ObjectGuid::LowType self = HomeOf(owner);  // the account's island
    if (HomeOfCharacter(guestGuid) == self)
    {
        reason = "That's you.";
        return false;
    }

    // A roommate is a guest who may decorate: making one puts them on the guest list too.
    if (roommate)
        CharacterDatabase.DirectExecute(
            "INSERT INTO mod_playerhousing_acl (owner_guid, guest_guid, roommate) VALUES ({}, {}, 1) ON DUPLICATE KEY UPDATE roommate=1", self, guestGuid);
    else
        CharacterDatabase.DirectExecute("UPDATE mod_playerhousing_acl SET roommate=0 WHERE owner_guid={} AND guest_guid={}", self, guestGuid);

    {
        std::lock_guard<std::recursive_mutex> guard(_lock);
        auto sessionItr = _sessionsByOwner.find(self);
        if (sessionItr != _sessionsByOwner.end())
        {
            if (roommate)
                sessionItr->second.roommates.insert(guestGuid);
            else
                sessionItr->second.roommates.erase(guestGuid);
        }
    }

    std::string name = NameOf(guestGuid);
    if (Player* guest = ObjectAccessor::FindPlayerByLowGUID(guestGuid); guest && MayNotify(owner, guestGuid, NOTICE_ROOMMATE))
        Say(guest, roommate ? Acore::StringFormat("{} made you a roommate: you can decorate their island (Edit in the housing window, there).", owner->GetName())
                            : Acore::StringFormat("You're no longer a roommate on {}'s island (still a guest).", owner->GetName()));
    reason = roommate ? Acore::StringFormat("{} is now a roommate: they can place their own pieces and change yours. Their pieces stay theirs.", name)
                      : Acore::StringFormat("{} is a guest again, no longer decorating.", name);
    return true;
}

bool PlayerHousingMgr::RemoveGuestByName(Player* player, std::string const& name, std::string& reason)
{
    ObjectGuid::LowType guestGuid = 0;
    std::string normalized;
    if (!ResolvePlayerGuid(name, guestGuid, normalized))
    {
        reason = "No character with that name.";
        return false;
    }

    return RemoveGuest(player, guestGuid, reason);
}

std::vector<VisitEntry> PlayerHousingMgr::GetGuests(ObjectGuid::LowType ownerGuid) const
{
    std::vector<VisitEntry> guests;
    if (QueryResult result = CharacterDatabase.Query("SELECT guest_guid, roommate FROM mod_playerhousing_acl WHERE owner_guid={}", ownerGuid))
    {
        do
        {
            VisitEntry entry;
            entry.ownerGuid = (*result)[0].Get<uint32>();
            entry.ownerName = NameOf(entry.ownerGuid);
            entry.roommate = (*result)[1].Get<uint8>() != 0;
            guests.push_back(entry);
        } while (result->NextRow());
    }

    std::sort(guests.begin(), guests.end(), [](VisitEntry const& left, VisitEntry const& right) { return left.ownerName < right.ownerName; });
    return guests;
}

std::vector<VisitEntry> PlayerHousingMgr::GetVisitList(Player const* player, uint8 list) const
{
    ObjectGuid::LowType self = player->GetGUID().GetCounter();
    ObjectGuid::LowType home = HomeOf(player);  // left out of every list
    // Each candidate with its island's privacy and flags, all in one query, so deciding who
    // may enter costs no query of its own for public islands.
    std::vector<HouseRecord> candidates;
    std::map<ObjectGuid::LowType, uint32> likes;

    auto collect = [&](QueryResult result)
    {
        if (!result)
            return;
        do
        {
            HouseRecord house;
            house.ownerGuid = (*result)[0].Get<uint32>();
            house.privacy = std::min<uint8>((*result)[1].Get<uint8>(), PRIVACY_FRIENDS);
            house.flags = (*result)[2].Get<uint32>();
            if (result->GetFieldCount() > 3)
                likes[house.ownerGuid] = uint32((*result)[3].Get<uint64>());
            candidates.push_back(house);
        } while (result->NextRow());
    };

    switch (list)
    {
        case 0:  // party
            if (Group const* group = player->GetGroup())
            {
                std::string members;
                for (Group::MemberSlot const& slot : group->GetMemberSlots())
                    if (slot.guid != player->GetGUID())
                        members += (members.empty() ? "" : ",") + std::to_string(slot.guid.GetCounter());
                if (!members.empty())
                    collect(CharacterDatabase.Query(
                        "SELECT DISTINCT h.owner_guid, h.is_private, h.flags FROM characters c JOIN mod_playerhousing_account a ON a.account_id = c.account "
                        "JOIN mod_playerhousing_house h ON h.owner_guid = a.home_guid WHERE c.guid IN ({}) AND h.owner_guid<>{}", members, home));
            }
            break;
        case 1:  // guild
            if (uint32 guildId = player->GetGuildId())
                collect(CharacterDatabase.Query(
                    "SELECT DISTINCT h.owner_guid, h.is_private, h.flags FROM guild_member gm JOIN characters c ON c.guid = gm.guid "
                    "JOIN mod_playerhousing_account a ON a.account_id = c.account JOIN mod_playerhousing_house h ON h.owner_guid = a.home_guid "
                    "WHERE gm.guildid={} AND h.owner_guid<>{} LIMIT 100", guildId, home));
            break;
        case 2:  // friends
            collect(CharacterDatabase.Query(
                "SELECT DISTINCT h.owner_guid, h.is_private, h.flags FROM character_social s JOIN characters c ON c.guid = s.friend "
                "JOIN mod_playerhousing_account a ON a.account_id = c.account JOIN mod_playerhousing_house h ON h.owner_guid = a.home_guid "
                "WHERE s.guid={} AND (s.flags & {}) AND h.owner_guid<>{} LIMIT 100", self, SOCIAL_FLAG_FRIEND, home));
            break;
        case 3:  // invited
            collect(CharacterDatabase.Query(
                "SELECT h.owner_guid, h.is_private, h.flags FROM mod_playerhousing_acl a JOIN mod_playerhousing_house h ON h.owner_guid = a.owner_guid "
                "WHERE a.guest_guid={} LIMIT 100", self));
            break;
        case 5:  // most liked, most first
            collect(CharacterDatabase.Query(
                "SELECT h.owner_guid, h.is_private, h.flags, COUNT(*) FROM mod_playerhousing_like l JOIN mod_playerhousing_house h ON h.owner_guid = l.owner_guid "
                "WHERE l.owner_guid<>{} GROUP BY h.owner_guid, h.is_private, h.flags ORDER BY COUNT(*) DESC, MIN(l.liked_at) LIMIT 100", home));
            break;
        default:  // public, hidden ones left out
            collect(CharacterDatabase.Query(
                "SELECT owner_guid, is_private, flags FROM mod_playerhousing_house WHERE is_private={} AND (flags & {}) = 0 AND owner_guid<>{} "
                "ORDER BY updated_at DESC LIMIT 100", uint32(PRIVACY_PUBLIC), uint32(HOUSE_FLAG_HIDDEN), home));
            break;
    }

    // Who has this player on a guest list or a friends list, once for the whole list rather
    // than a query or two per island.
    VisitContext context;
    if (QueryResult result = CharacterDatabase.Query("SELECT owner_guid FROM mod_playerhousing_acl WHERE guest_guid={}", self))
    {
        do
        {
            context.guestOf.insert((*result)[0].Get<uint32>());
        } while (result->NextRow());
    }
    // (Islands of accounts where any character has this one as a friend.)
    if (QueryResult result = CharacterDatabase.Query(
            "SELECT DISTINCT a.home_guid FROM character_social s JOIN characters c ON c.guid = s.guid "
            "JOIN mod_playerhousing_account a ON a.account_id = c.account WHERE s.friend={} AND (s.flags & {})", self, SOCIAL_FLAG_FRIEND))
    {
        do
        {
            context.friendOf.insert((*result)[0].Get<uint32>());
        } while (result->NextRow());
    }

    // Only islands this player may enter, so every entry works when clicked.
    std::vector<VisitEntry> entries;
    std::set<ObjectGuid::LowType> seen;
    for (HouseRecord const& house : candidates)
    {
        ObjectGuid::LowType owner = house.ownerGuid;
        if (!seen.insert(owner).second)
            continue;

        CharacterCacheEntry const* character = sCharacterCache->GetCharacterCacheByGuid(ObjectGuid::Create<HighGuid::Player>(owner));
        std::string ignored;
        if (!character || !CanVisit(player, house, ignored, &context))
            continue;

        VisitEntry entry;
        entry.ownerGuid = owner;
        entry.ownerName = character->Name;
        if (list == 5)
            entry.likes = likes[owner];
        entries.push_back(entry);
        if (list == 5 && entries.size() >= 18)
            break;
    }

    if (list != 5)
        std::sort(entries.begin(), entries.end(), [](VisitEntry const& left, VisitEntry const& right) { return left.ownerName < right.ownerName; });
    return entries;
}

uint32 PlayerHousingMgr::CountLikes(ObjectGuid::LowType ownerGuid) const
{
    if (QueryResult result = CharacterDatabase.Query("SELECT COUNT(*) FROM mod_playerhousing_like WHERE owner_guid={}", ownerGuid))
        return uint32((*result)[0].Get<uint64>());
    return 0;
}

bool PlayerHousingMgr::LikesIsland(Player const* player, ObjectGuid::LowType ownerGuid) const
{
    return bool(CharacterDatabase.Query("SELECT 1 FROM mod_playerhousing_like WHERE owner_guid={} AND liker_account={}",
        ownerGuid, player->GetSession()->GetAccountId()));
}

bool PlayerHousingMgr::ToggleLike(Player* player, std::string& reason)
{
    ObjectGuid::LowType owner = GetIslandOwner(player);
    if (!owner)
    {
        reason = "Visit an island to like it.";
        return false;
    }
    if (owner == HomeOf(player))
    {
        reason = "You can't like your own island (everyone else can).";
        return false;
    }

    if (OnCooldown(player, COOLDOWN_LIKE, 10000, reason))
        return false;

    // One like per account, so alts don't count twice.
    uint32 account = player->GetSession()->GetAccountId();
    std::string ownerName = NameOf(owner);
    if (LikesIsland(player, owner))
    {
        CharacterDatabase.DirectExecute("DELETE FROM mod_playerhousing_like WHERE owner_guid={} AND liker_account={}", owner, account);
        reason = Acore::StringFormat("You took back your like of {}'s island.", ownerName);
        return true;
    }

    CharacterDatabase.DirectExecute("INSERT IGNORE INTO mod_playerhousing_like (owner_guid, liker_account, liker_guid) VALUES ({}, {}, {})",
        owner, account, player->GetGUID().GetCounter());
    if (Player* ownerPlayer = FindOwnerOnline(owner); ownerPlayer && MayNotify(player, owner, NOTICE_LIKE))
        Say(ownerPlayer, Acore::StringFormat("{} likes your island.", player->GetName()));
    reason = Acore::StringFormat("You like {}'s island ({} {}).", ownerName, CountLikes(owner), CountLikes(owner) == 1 ? "like" : "likes");
    return true;
}

void PlayerHousingMgr::LogVisit(ObjectGuid::LowType ownerGuid, Player* visitor) const
{
    if (!visitor || visitor->GetSession()->IsHeadless() || HomeOf(visitor) == ownerGuid)
        return;

    // The last 50 arrivals are kept.
    CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();
    trans->Append("INSERT INTO mod_playerhousing_visit_log (owner_guid, visitor_guid) VALUES ({}, {})", ownerGuid, visitor->GetGUID().GetCounter());
    trans->Append("DELETE FROM mod_playerhousing_visit_log WHERE owner_guid={} AND id NOT IN "
                  "(SELECT id FROM (SELECT id FROM mod_playerhousing_visit_log WHERE owner_guid={} ORDER BY id DESC LIMIT 50) newest)",
        ownerGuid, ownerGuid);
    CharacterDatabase.CommitTransaction(trans);
}

std::vector<std::pair<std::string, std::string>> PlayerHousingMgr::GetVisitorLog(ObjectGuid::LowType ownerGuid, uint32 limit) const
{
    std::vector<std::pair<std::string, std::string>> log;
    if (QueryResult result = CharacterDatabase.Query(
            "SELECT visitor_guid, DATE_FORMAT(visited_at, '%Y-%m-%d %H:%i') FROM mod_playerhousing_visit_log WHERE owner_guid={} ORDER BY id DESC LIMIT {}",
            ownerGuid, limit))
    {
        do
        {
            log.emplace_back(NameOf((*result)[0].Get<uint32>()), (*result)[1].Get<std::string>());
        } while (result->NextRow());
    }
    return log;
}

uint32 PlayerHousingMgr::CountVisitorsThisWeek(ObjectGuid::LowType ownerGuid) const
{
    if (QueryResult result = CharacterDatabase.Query(
            "SELECT COUNT(DISTINCT visitor_guid) FROM mod_playerhousing_visit_log WHERE owner_guid={} AND visited_at > NOW() - INTERVAL 7 DAY", ownerGuid))
        return uint32((*result)[0].Get<uint64>());
    return 0;
}

bool PlayerHousingMgr::SetGreeting(Player* player, std::string const& greeting, std::string& reason)
{
    std::string text = greeting;
    // No color or link codes from players.
    text.erase(std::remove(text.begin(), text.end(), '|'), text.end());
    text.erase(0, text.find_first_not_of(" \t"));
    text.erase(text.find_last_not_of(" \t") + 1);
    TruncateUtf8(text, MAX_GREETING);

    ObjectGuid::LowType owner = HomeOf(player);  // the account's island
    EnsureHouse(owner);
    CharacterDatabase.EscapeString(text);
    CharacterDatabase.DirectExecute("UPDATE mod_playerhousing_house SET greeting='{}' WHERE owner_guid={}", text, owner);
    reason = text.empty() ? "Greeting cleared." : "Visitors will now be greeted with your message.";
    return true;
}

// ---------------------------------------------------------------------------------------------
// The guestbook

bool PlayerHousingMgr::SignGuestbook(Player* visitor, std::string const& text, std::string& reason)
{
    ObjectGuid::LowType owner = GetIslandOwner(visitor);
    ObjectGuid::LowType self = HomeOf(visitor);  // the visitor's account's island
    if (!owner || owner == self)
    {
        reason = owner ? "It's your own guestbook: visitors write in it." : "Visit an island to sign its guestbook.";
        return false;
    }

    std::string note = CleanNote(text);
    if (note.empty())
    {
        reason = "Write something to leave in the guestbook.";
        return false;
    }

    // One note per island a day, and five an hour in all, per account.
    uint32 account = visitor->GetSession()->GetAccountId();
    if (QueryResult result = CharacterDatabase.Query(
            "SELECT COUNT(*) FROM mod_playerhousing_guestbook WHERE author_account={} AND written_at > NOW() - INTERVAL 1 HOUR", account))
        if ((*result)[0].Get<uint64>() >= 5)
        {
            reason = "You've written a lot of notes this hour. Try again later.";
            return false;
        }
    if (CharacterDatabase.Query("SELECT 1 FROM mod_playerhousing_guestbook WHERE owner_guid={} AND author_account={} "
            "AND written_at > NOW() - INTERVAL 1 DAY", owner, account))
    {
        reason = "You've already signed this guestbook today.";
        return false;
    }

    std::string escaped = note;
    CharacterDatabase.EscapeString(escaped);
    CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();
    trans->Append("INSERT INTO mod_playerhousing_guestbook (owner_guid, author_guid, author_account, text) VALUES ({}, {}, {}, '{}')",
        owner, self, account, escaped);
    trans->Append("DELETE FROM mod_playerhousing_guestbook WHERE owner_guid={} AND id NOT IN "
                  "(SELECT id FROM (SELECT id FROM mod_playerhousing_guestbook WHERE owner_guid={} ORDER BY id DESC LIMIT {}) newest)",
        owner, owner, NOTES_KEPT);
    CharacterDatabase.CommitTransaction(trans);

    std::string ownerName = NameOf(owner);
    if (Player* ownerPlayer = FindOwnerOnline(owner))
        if (ownerPlayer != visitor && MayNotify(visitor, owner, NOTICE_GUESTBOOK))
            Say(ownerPlayer, Acore::StringFormat("{} signed your guestbook.", visitor->GetName()));
    reason = Acore::StringFormat("You signed {}'s guestbook.", ownerName);
    return true;
}

std::vector<GuestbookNote> PlayerHousingMgr::GetGuestbook(ObjectGuid::LowType ownerGuid, uint32 limit) const
{
    std::vector<GuestbookNote> notes;
    if (QueryResult result = CharacterDatabase.Query(
            "SELECT id, author_guid, DATE_FORMAT(written_at, '%Y-%m-%d %H:%i'), text, seen FROM mod_playerhousing_guestbook "
            "WHERE owner_guid={} ORDER BY id DESC LIMIT {}", ownerGuid, limit))
    {
        do
        {
            Field* fields = result->Fetch();
            notes.push_back(GuestbookNote{ fields[0].Get<uint32>(), NameOf(fields[1].Get<uint32>()), fields[2].Get<std::string>(),
                fields[3].Get<std::string>(), fields[4].Get<uint8>() == 0 });
        } while (result->NextRow());
    }
    return notes;
}

uint32 PlayerHousingMgr::CountNewNotes(ObjectGuid::LowType ownerGuid) const
{
    if (QueryResult result = CharacterDatabase.Query("SELECT COUNT(*) FROM mod_playerhousing_guestbook WHERE owner_guid={} AND seen=0", ownerGuid))
        return uint32((*result)[0].Get<uint64>());
    return 0;
}

void PlayerHousingMgr::MarkGuestbookRead(ObjectGuid::LowType ownerGuid) const
{
    CharacterDatabase.Execute("UPDATE mod_playerhousing_guestbook SET seen=1 WHERE owner_guid={} AND seen=0", ownerGuid);
}

bool PlayerHousingMgr::DeleteNote(Player* owner, uint32 noteId, std::string& reason)
{
    ObjectGuid::LowType self = HomeOf(owner);  // the account's island
    if (!CharacterDatabase.Query("SELECT 1 FROM mod_playerhousing_guestbook WHERE owner_guid={} AND id={}", self, noteId))
    {
        reason = "That note is gone.";
        return false;
    }
    CharacterDatabase.DirectExecute("DELETE FROM mod_playerhousing_guestbook WHERE owner_guid={} AND id={}", self, noteId);
    reason = "Threw the note out.";
    return true;
}

// ---------------------------------------------------------------------------------------------
// The door: where visitors arrive

bool PlayerHousingMgr::SetDoor(Player* player, bool reset, std::string& reason)
{
    ObjectGuid::LowType self = HomeOf(player);  // the account's island
    EnsureHouse(self);
    if (reset)
    {
        CharacterDatabase.DirectExecute("UPDATE mod_playerhousing_house SET door_set=0 WHERE owner_guid={}", self);
        reason = "Visitors arrive at the landing spot again.";
        return true;
    }

    if (!IsOnOwnIsland(player) || !IsSpotOnIsland(player->GetPositionX(), player->GetPositionY(), player->GetPositionZ()))
    {
        reason = "Stand where visitors should arrive, on your own island.";
        return false;
    }
    CharacterDatabase.DirectExecute("UPDATE mod_playerhousing_house SET door_set=1, door_x={}, door_y={}, door_z={}, door_o={} WHERE owner_guid={}",
        player->GetPositionX(), player->GetPositionY(), player->GetPositionZ(), player->GetOrientation(), self);
    reason = "Visitors will arrive here, facing the way you face now.";
    return true;
}
