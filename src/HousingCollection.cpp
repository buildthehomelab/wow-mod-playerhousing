#include "PlayerHousingMgr.h"

#include "Chat.h"
#include "DatabaseEnv.h"
#include "DBCStores.h"
#include "Group.h"
#include "Log.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "ReputationMgr.h"
#include "StringFormat.h"
#include "WorldSession.h"

#include <algorithm>
#include <cctype>

using namespace Housing;

namespace
{
    char const* const RANK_NAMES[] = { "Hated", "Hostile", "Unfriendly", "Neutral", "Friendly", "Honored", "Revered", "Exalted" };
    // Standing where each rank starts, and how much it takes to fill it.
    int32 const RANK_START[] = { -42000, -6000, -3000, 0, 3000, 9000, 21000, 42000 };
    int32 const RANK_SIZE[] = { 36000, 3000, 3000, 3000, 6000, 12000, 21000, 1000 };

    std::string Thousands(int32 value)
    {
        std::string digits = std::to_string(std::abs(value));
        for (int32 i = int32(digits.size()) - 3; i > 0; i -= 3)
            digits.insert(size_t(i), ",");
        return value < 0 ? "-" + digits : digits;
    }

    std::string ToLower(std::string value)
    {
        std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return char(std::tolower(c)); });
        return value;
    }
}

bool PlayerHousingMgr::IsAreaExplored(Player const* player, uint32 areaId)
{
    AreaTableEntry const* area = sAreaTableStore.LookupEntry(areaId);
    if (!area)
        return false;

    uint32 offset = area->exploreFlag / 32;
    if (offset >= PLAYER_EXPLORED_ZONES_SIZE)
        return false;

    uint32 bit = uint32(1) << (area->exploreFlag % 32);
    return (player->GetUInt32Value(PLAYER_EXPLORED_ZONES_1 + offset) & bit) != 0;
}

bool PlayerHousingMgr::RuleMet(Player const* player, PieceRule const& rule) const
{
    switch (rule.type)
    {
        case RULE_LEVEL:
            return player->GetLevel() >= rule.param1;
        case RULE_ACHIEVEMENT:
            return player->HasAchieved(rule.param1);
        case RULE_REPUTATION:
            return player->GetReputationRank(rule.param1) >= ReputationRank(rule.param2);
        case RULE_QUEST:
            return player->GetQuestRewardStatus(rule.param1);
        case RULE_EXPLORE:
            return player->GetAreaId() == rule.param1 || player->GetZoneId() == rule.param1 || IsAreaExplored(player, rule.param1);
        case RULE_SKILL:
            return player->GetSkillValue(rule.param1) >= rule.param2;
        case RULE_KILL:     // only when it happens, see OnCreatureKilled
        case RULE_NEVER:
        default:
            return false;
    }
}

template <typename Triggered>
bool PlayerHousingMgr::AnyGroupMet(Player const* player, PieceDefinition const& piece, Triggered triggered) const
{
    std::map<uint8, bool> groups;
    for (PieceRule const& rule : piece.rules)
    {
        bool met = triggered(rule) || RuleMet(player, rule);
        auto itr = groups.find(rule.group);
        if (itr == groups.end())
            groups[rule.group] = met;
        else
            itr->second = itr->second && met;
    }

    return std::any_of(groups.begin(), groups.end(), [](auto const& group) { return group.second; });
}

std::set<uint32> PlayerHousingMgr::LoadUnlocks(Player const* player) const
{
    std::set<uint32> unlocked;
    if (QueryResult result = CharacterDatabase.Query(
            "SELECT item_entry FROM mod_playerhousing_collection WHERE (account_id={} AND guid=0) OR guid={}",
            player->GetSession()->GetAccountId(), player->GetGUID().GetCounter()))
    {
        do
        {
            unlocked.insert((*result)[0].Get<uint32>());
        } while (result->NextRow());
    }
    return unlocked;
}

std::set<uint32> PlayerHousingMgr::LoadNewUnlocks(Player const* player) const
{
    std::set<uint32> fresh;
    if (_unlockAll)
        return fresh;

    if (QueryResult result = CharacterDatabase.Query(
            "SELECT item_entry FROM mod_playerhousing_collection WHERE ((account_id={} AND guid=0) OR guid={}) AND seen=0",
            player->GetSession()->GetAccountId(), player->GetGUID().GetCounter()))
    {
        do
        {
            fresh.insert((*result)[0].Get<uint32>());
        } while (result->NextRow());
    }
    return fresh;
}

void PlayerHousingMgr::MarkSeen(Player const* player, std::vector<uint32> const& itemEntries) const
{
    if (itemEntries.empty())
        return;

    std::string list;
    for (uint32 entry : itemEntries)
        list += (list.empty() ? "" : ",") + std::to_string(entry);
    CharacterDatabase.Execute(
        "UPDATE mod_playerhousing_collection SET seen=1 WHERE ((account_id={} AND guid=0) OR guid={}) AND item_entry IN ({})",
        player->GetSession()->GetAccountId(), player->GetGUID().GetCounter(), list);
}

bool PlayerHousingMgr::IsUnlocked(Player const* player, PieceDefinition const& piece, std::set<uint32> const* known) const
{
    if (_unlockAll || piece.rules.empty())
        return true;

    if (known)
        return known->count(piece.itemEntry) > 0;

    return LoadUnlocks(player).count(piece.itemEntry) > 0;
}

bool PlayerHousingMgr::Unlock(Player* player, PieceDefinition const& piece, bool announce)
{
    // Playerbots never collect pieces.
    if (player->GetSession()->IsHeadless())
        return false;

    // Unlocking it gives the character who did it one to place; more cost a copy each. Only the
    // first time: a GM unlocking it again, or an alt on the same account, gets no extra one.
    bool already = LoadUnlocks(player).count(piece.itemEntry) > 0;
    uint32 guid = piece.HasFlag(PIECE_FLAG_PER_CHARACTER) ? player->GetGUID().GetCounter() : 0;
    CharacterDatabase.DirectExecute(
        "INSERT IGNORE INTO mod_playerhousing_collection (account_id, guid, item_entry, seen) VALUES ({}, {}, {}, 0)",
        player->GetSession()->GetAccountId(), guid, piece.itemEntry);
    if (!already)
        AddToStorage(HomeOf(player), piece.itemEntry, 1);

    if (announce)
    {
        std::string kind = piece.IsBuilding() ? "building" : "furnishing";
        ChatHandler(player->GetSession()).SendNotification("Housing unlock: " + piece.name);
        Say(player, Acore::StringFormat("New {} unlocked: {}. There's one waiting in your Collection (/housing).", kind, piece.name));
        Tip(player, TIP_FIRST_UNLOCK, "Your Collection grows as you explore, run dungeons and raids, earn reputation and level your professions.");
    }
    return true;
}

void PlayerHousingMgr::EvaluateUnlocks(Player* player, uint8 ruleType, uint32 param, bool announce, uint32 value)
{
    if (!_enabled || !player || _unlockAll || player->GetSession()->IsHeadless())
        return;

    std::vector<PieceDefinition const*> candidates;
    {
        std::lock_guard<std::recursive_mutex> guard(_lock);
        auto itr = _piecesByRule.find({ ruleType, ruleType == RULE_LEVEL ? 0 : param });
        if (itr == _piecesByRule.end())
            return;

        for (uint32 itemEntry : itr->second)
        {
            auto pieceItr = _pieces.find(itemEntry);
            if (pieceItr != _pieces.end())
                candidates.push_back(&pieceItr->second);
        }
    }

    std::set<uint32> known = LoadUnlocks(player);
    for (PieceDefinition const* piece : candidates)
    {
        if (known.count(piece->itemEntry))
            continue;

        // The rule behind this event is met by the event itself: a kill, entering an area, or
        // a new level, rank or skill value the player's own data may not show yet.
        auto triggered = [&](PieceRule const& rule)
        {
            if (rule.type != ruleType)
                return false;
            switch (ruleType)
            {
                case RULE_LEVEL: return value >= rule.param1;
                case RULE_REPUTATION:
                case RULE_SKILL: return rule.param1 == param && value >= rule.param2;
                default: return rule.param1 == param;
            }
        };

        if (AnyGroupMet(player, *piece, triggered))
            Unlock(player, *piece, announce);
    }
}

uint32 PlayerHousingMgr::CreditPastProgress(Player* player)
{
    if (!_enabled || !player || _unlockAll)
        return 0;

    std::vector<PieceDefinition const*> candidates;
    {
        std::lock_guard<std::recursive_mutex> guard(_lock);
        for (auto const& [itemEntry, piece] : _pieces)
            if (!piece.rules.empty())
                candidates.push_back(&piece);
    }

    std::set<uint32> known = LoadUnlocks(player);
    uint32 credited = 0;
    for (PieceDefinition const* piece : candidates)
    {
        if (known.count(piece->itemEntry))
            continue;

        bool met = AnyGroupMet(player, *piece, [](PieceRule const&) { return false; });
        if (met && Unlock(player, *piece, false))
            ++credited;
    }
    return credited;
}

void PlayerHousingMgr::OnCreatureKilled(Player* killer, uint32 creatureEntry)
{
    if (!_enabled || !killer || killer->GetSession()->IsHeadless())
        return;

    {
        std::lock_guard<std::recursive_mutex> guard(_lock);
        if (!_piecesByRule.count({ RULE_KILL, creatureEntry }))
            return;
    }

    // Everyone in the group who was there shares the kill, like an achievement.
    std::vector<Player*> credited{ killer };
    if (Group* group = killer->GetGroup())
    {
        for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
        {
            Player* member = ref->GetSource();
            if (member && member != killer && member->IsInMap(killer) && member->IsWithinDist(killer, 100.0f, false))
                credited.push_back(member);
        }
    }

    for (Player* player : credited)
        EvaluateUnlocks(player, RULE_KILL, creatureEntry, true);
}

std::string PlayerHousingMgr::DescribeProgress(Player const* player, PieceDefinition const& piece) const
{
    std::string text = piece.hint;
    for (PieceRule const& rule : piece.rules)
    {
        if (RuleMet(player, rule))
            continue;

        switch (rule.type)
        {
            case RULE_REPUTATION:
            {
                FactionEntry const* faction = sFactionStore.LookupEntry(rule.param1);
                if (!faction)
                    break;

                ReputationRank rank = player->GetReputationRank(rule.param1);
                int32 standing = player->GetReputationMgr().GetReputation(faction);
                uint8 rankIndex = std::min<uint8>(uint8(rank), 7);
                if (text.empty())
                    text = Acore::StringFormat("Reach {} with {}", RANK_NAMES[std::min<uint32>(rule.param2, 7)], faction->name[0]);
                text += Acore::StringFormat(" (you're {}, {}/{})", RANK_NAMES[rankIndex],
                    Thousands(standing - RANK_START[rankIndex]), Thousands(RANK_SIZE[rankIndex]));
                break;
            }
            case RULE_LEVEL:
                if (text.empty())
                    text = Acore::StringFormat("Reach level {}", rule.param1);
                text += Acore::StringFormat(" (you're level {})", player->GetLevel());
                break;
            case RULE_SKILL:
                if (text.empty())
                    text = Acore::StringFormat("Reach {} skill", rule.param2);
                text += Acore::StringFormat(" (you have {})", player->GetSkillValue(rule.param1));
                break;
            case RULE_ACHIEVEMENT:
                if (text.empty())
                    if (AchievementEntry const* achievement = sAchievementStore.LookupEntry(rule.param1))
                        text = Acore::StringFormat("Earn the achievement {}", achievement->name[0]);
                break;
            default:
                break;
        }
    }

    return text.empty() ? "Not available yet" : text;
}

void PlayerHousingMgr::CollectionCounts(Player const* player, int32 category, uint32& unlocked, uint32& total, std::set<uint32> const* known) const
{
    std::set<uint32> loaded;
    if (!known)
    {
        loaded = LoadUnlocks(player);
        known = &loaded;
    }

    unlocked = 0;
    total = 0;
    std::lock_guard<std::recursive_mutex> guard(_lock);
    for (auto const& [itemEntry, piece] : _pieces)
    {
        if (category >= 0 && piece.category != uint8(category))
            continue;

        ++total;
        if (IsUnlocked(player, piece, known))
            ++unlocked;
    }
}

bool PlayerHousingMgr::GetCopies(Player* player, uint32 itemEntry, uint32 count, std::string& reason)
{
    PieceDefinition const* piece = GetPiece(itemEntry);
    if (!piece)
    {
        reason = "That piece doesn't exist.";
        return false;
    }

    if (!IsUnlocked(player, *piece))
    {
        reason = Acore::StringFormat("{} is still locked: {}.", piece->name, DescribeProgress(player, *piece));
        return false;
    }

    count = std::clamp<uint32>(count, 1, 20);
    uint64 cost = uint64(_freeMode ? 0 : piece->copyCost) * count;
    if (cost && player->GetMoney() < cost)
    {
        reason = count == 1 ? Acore::StringFormat("A {} costs {}.", piece->name, FormatMoney(cost))
                            : Acore::StringFormat("{} of them cost {}.", count, FormatMoney(cost));
        return false;
    }

    if (cost)
        player->ModifyMoney(-int64(cost));
    AddToStorage(HomeOf(player), itemEntry, int32(count));

    // The addon's Collection shows the new count; typed, a line.
    if (HasAddon(player))
        reason.clear();
    else
        reason = Acore::StringFormat("Added {}{} to your Collection{}.", piece->name, count > 1 ? Acore::StringFormat(" x{}", count) : std::string(),
            cost ? " for " + FormatMoney(cost) : std::string());
    return true;
}

bool PlayerHousingMgr::GetOneOfEverything(Player* player, std::string& reason)
{
    if (OnCooldown(player, COOLDOWN_HEAVY, 3000, reason))
        return false;
    if (!_freeMode && !_unlockAll)
    {
        reason = "That's only available on test servers.";
        return false;
    }

    std::set<uint32> known = LoadUnlocks(player);
    std::vector<uint32> entries;
    {
        std::lock_guard<std::recursive_mutex> guard(_lock);
        for (auto const& [itemEntry, piece] : _pieces)
            if (IsUnlocked(player, piece, &known))
                entries.push_back(itemEntry);
    }

    for (uint32 itemEntry : entries)
        AddToStorage(HomeOf(player), itemEntry, 1);
    reason = Acore::StringFormat("Added one of each of {} pieces to your Collection.", entries.size());
    return true;
}

bool PlayerHousingMgr::GmUnlock(Player* target, std::string const& what, bool unlock, std::string& reason)
{
    if (unlock && target->GetSession()->IsHeadless())
    {
        reason = "Playerbots don't do housing.";
        return false;
    }

    // An item entry, "all", an exact name, or else every name containing the text. Pieces
    // everyone has (no unlock rules) are left out: there's nothing to unlock.
    std::vector<PieceDefinition const*> matches;
    std::vector<PieceDefinition const*> exact;
    bool anyMatch = false;
    {
        std::lock_guard<std::recursive_mutex> guard(_lock);
        uint32 entry = uint32(std::strtoul(what.c_str(), nullptr, 10));
        std::string lowerWhat = ToLower(what);
        for (auto const& [itemEntry, piece] : _pieces)
        {
            std::string lowerName = ToLower(piece.name);
            bool match = lowerWhat == "all" || itemEntry == entry || (!entry && lowerName.find(lowerWhat) != std::string::npos);
            if (!match)
                continue;
            anyMatch = true;
            if (piece.rules.empty())
                continue;
            matches.push_back(&piece);
            if (lowerName == lowerWhat)
                exact.push_back(&piece);
        }
    }
    if (!exact.empty())
        matches = exact;

    if (matches.empty())
    {
        reason = anyMatch ? "Everyone has that piece from the start: there's nothing to unlock." : "No piece matches that.";
        return false;
    }

    for (PieceDefinition const* piece : matches)
    {
        if (unlock)
            Unlock(target, *piece, matches.size() == 1);
        else
            CharacterDatabase.DirectExecute(
                "DELETE FROM mod_playerhousing_collection WHERE item_entry={} AND ((account_id={} AND guid=0) OR guid={})",
                piece->itemEntry, target->GetSession()->GetAccountId(), target->GetGUID().GetCounter());
    }

    reason = Acore::StringFormat("{} {} {} for {}.", unlock ? "Unlocked" : "Locked", matches.size(),
        matches.size() == 1 ? matches.front()->name : std::string("pieces"), target->GetName());
    return true;
}
