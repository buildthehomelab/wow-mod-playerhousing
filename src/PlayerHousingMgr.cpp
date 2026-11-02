#include "PlayerHousingMgr.h"

#include "CharacterCache.h"
#include "Chat.h"
#include "Config.h"
#include "Creature.h"
#include "DatabaseEnv.h"
#include "DBCStores.h"
#include "GameObject.h"
#include "Group.h"
#include "Item.h"
#include "Log.h"
#include "Map.h"
#include "MapMgr.h"
#include "ObjectMgr.h"
#include "GameTime.h"
#include "Player.h"
#include "QuestDef.h"
#include "SharedDefines.h"
#include "StringFormat.h"
#include "WorldPacket.h"
#include "WorldSession.h"

#include <algorithm>
#include <cctype>
#include <cmath>

using namespace Housing;

namespace
{
    constexpr uint32 LEGACY_HOUSING_MAPS[] = { 309, 531, 534, 568, 658 };  // earlier instanced houses
    constexpr uint32 STEWARD_DISPLAY_ID = 25384;                           // Wolvar orphan
    // House phases carry this bit and never bit 0 (PHASEMASK_NORMAL), so nothing in the normal
    // world shares a bit with them.
    constexpr uint32 HOUSING_PHASE_FLAG = 0x80000000;
    // Players are brought back to the beach this far inside the edge of their private copy.
    constexpr float EDGE_MARGIN = 12.0f;
    // Footprint of GM Island's guild hall, for things placed inside it before it was removed.
    constexpr float HALL_MIN_X = 16205.0f;
    constexpr float HALL_MAX_X = 16274.0f;
    constexpr float HALL_MIN_Y = 16262.0f;
    constexpr float HALL_MAX_Y = 16335.0f;

    std::string ToLower(std::string value)
    {
        std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return std::tolower(c); });
        return value;
    }
}

PlayerHousingMgr* PlayerHousingMgr::instance()
{
    static PlayerHousingMgr instance;
    return &instance;
}

std::string PlayerHousingMgr::FormatMoney(uint64 copper)
{
    uint64 gold = copper / 10000;
    uint64 silver = (copper % 10000) / 100;
    uint64 bronze = copper % 100;
    if (gold)
        return silver ? Acore::StringFormat("{}g {}s", gold, silver) : Acore::StringFormat("{}g", gold);
    if (silver)
        return bronze ? Acore::StringFormat("{}s {}c", silver, bronze) : Acore::StringFormat("{}s", silver);
    return Acore::StringFormat("{}c", bronze);
}

char const* PlayerHousingMgr::CategoryName(uint8 category)
{
    switch (category)
    {
        case CATEGORY_STARTER: return "Starter";
        case CATEGORY_BUILDINGS: return "Buildings";
        case CATEGORY_EXPLORATION: return "Exploration";
        case CATEGORY_DUNGEONS: return "Dungeons";
        case CATEGORY_RAIDS: return "Raids";
        case CATEGORY_REPUTATION: return "Reputation";
        case CATEGORY_PROFESSIONS: return "Professions";
        case CATEGORY_HOLIDAYS: return "Holidays";
        case CATEGORY_CAPSTONES: return "Capstones";
        case CATEGORY_FIGURINES: return "Figurines";
        case CATEGORY_CATALOG: return "Catalog";
        default: return "Other";
    }
}

void PlayerHousingMgr::LoadConfig()
{
    _enabled = sConfigMgr->GetOption<bool>("PlayerHousing.Enable", true);
    _freeMode = sConfigMgr->GetOption<bool>("PlayerHousing.FreeMode", false);
    _unlockAll = sConfigMgr->GetOption<bool>("PlayerHousing.UnlockAll", false);
    _gmVisitBypass = sConfigMgr->GetOption<bool>("PlayerHousing.GmBypassPrivate", false);

    std::string privacy = ToLower(sConfigMgr->GetOption<std::string>("PlayerHousing.DefaultPrivacy", "private"));
    _defaultPrivacy = privacy == "public" ? PRIVACY_PUBLIC : (privacy == "friends" ? PRIVACY_FRIENDS : PRIVACY_PRIVATE);

    _stewardEntry = sConfigMgr->GetOption<uint32>("PlayerHousing.StewardEntry", 900200);
    _stewardDisplayId = sConfigMgr->GetOption<uint32>("PlayerHousing.StewardDisplayId", STEWARD_DISPLAY_ID);
    if (!sCreatureDisplayInfoStore.LookupEntry(_stewardDisplayId))
    {
        LOG_WARN("module", "mod-playerhousing: Steward display id {} is invalid. Falling back to {}.", _stewardDisplayId, STEWARD_DISPLAY_ID);
        _stewardDisplayId = STEWARD_DISPLAY_ID;
    }

    _maxFurnishings = std::clamp<uint32>(sConfigMgr->GetOption<uint32>("PlayerHousing.MaxFurnishings", 200), 1, 5000);
    _maxBuildings = std::clamp<uint32>(sConfigMgr->GetOption<uint32>("PlayerHousing.MaxBuildings", 10), 0, 200);
    _keyDelaySeconds = std::min<uint32>(sConfigMgr->GetOption<uint32>("PlayerHousing.HouseKey.DelaySeconds", 10), 60);
    _layoutCode = ToLower(sConfigMgr->GetOption<std::string>("PlayerHousing.Layout", "cleared"));
    // It goes into a query: letters, digits and underscores only.
    if (_layoutCode.empty() || _layoutCode.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789_") != std::string::npos)
    {
        LOG_WARN("module", "mod-playerhousing: PlayerHousing.Layout \"{}\" isn't a layout name; using cleared.", _layoutCode);
        _layoutCode = "cleared";
    }

    _sizeMin = std::clamp(sConfigMgr->GetOption<float>("PlayerHousing.Size.Min", 0.5f), 0.1f, 1.0f);
    _sizeMax = std::clamp(sConfigMgr->GetOption<float>("PlayerHousing.Size.Max", 2.0f), 1.0f, 10.0f);
    _tiltMax = std::clamp(sConfigMgr->GetOption<float>("PlayerHousing.Tilt.Max", 180.0f), 0.0f, 180.0f);
    _maxSavedLayouts = std::min<uint32>(sConfigMgr->GetOption<uint32>("PlayerHousing.SavedLayouts", 5), 20);
    std::string catalog = ToLower(sConfigMgr->GetOption<std::string>("PlayerHousing.Catalog", "curated"));
    _catalogEverything = catalog == "everything";
    if (catalog != "everything" && catalog != "curated")
        LOG_WARN("module", "mod-playerhousing: PlayerHousing.Catalog = \"{}\" is neither curated nor everything; using curated.", catalog);
    _ghosts = sConfigMgr->GetOption<bool>("PlayerHousing.Ghosts", true);
}

bool PlayerHousingMgr::LoadDefinitions()
{
    std::lock_guard<std::recursive_mutex> guard(_lock);

    _pieces.clear();
    _piecesByRule.clear();

    QueryResult layoutResult = WorldDatabase.Query(
        "SELECT layout, map_id, landing_x, landing_y, landing_z, landing_o, steward_offset_x, steward_offset_y, center_x, center_y, radius "
        "FROM mod_playerhousing_layout ORDER BY layout = '{}' DESC, layout", _layoutCode);
    if (!layoutResult)
    {
        LOG_ERROR("module", "mod-playerhousing: No island layouts found. Did you apply the db_world SQL?");
        return false;
    }

    Field* layoutFields = layoutResult->Fetch();
    _layout.code = layoutFields[0].Get<std::string>();
    _layout.mapId = layoutFields[1].Get<uint32>();
    _layout.landing.Relocate(layoutFields[2].Get<float>(), layoutFields[3].Get<float>(), layoutFields[4].Get<float>(), layoutFields[5].Get<float>());
    _layout.stewardOffsetX = layoutFields[6].Get<float>();
    _layout.stewardOffsetY = layoutFields[7].Get<float>();
    _layout.centerX = layoutFields[8].Get<float>();
    _layout.centerY = layoutFields[9].Get<float>();
    _layout.radius = std::max(20.0f, layoutFields[10].Get<float>());
    if (_layout.code != _layoutCode)
        LOG_WARN("module", "mod-playerhousing: Layout '{}' not found, using '{}'.", _layoutCode, _layout.code);

    MapEntry const* mapEntry = sMapStore.LookupEntry(_layout.mapId);
    if (!mapEntry || mapEntry->Instanceable())
    {
        LOG_ERROR("module", "mod-playerhousing: Layout map {} is missing or instanced; islands need an open-world map.", _layout.mapId);
        return false;
    }

    QueryResult pieceResult = WorldDatabase.Query(
        "SELECT item_entry, kind, category, name, go_entry, edit_go_entry, scale, footprint, height, flags, copy_cost, sort_order, hint, legacy_catalog_id, "
        "outline_min_x, outline_min_y, outline_max_x, outline_max_y, creature_entry FROM mod_playerhousing_piece");
    if (!pieceResult)
    {
        LOG_ERROR("module", "mod-playerhousing: No pieces found. Did you apply the db_world SQL?");
        return false;
    }

    do
    {
        Field* fields = pieceResult->Fetch();
        PieceDefinition piece;
        piece.itemEntry = fields[0].Get<uint32>();
        piece.kind = fields[1].Get<uint8>();
        piece.category = std::min<uint8>(fields[2].Get<uint8>(), CATEGORY_COUNT - 1);
        piece.name = fields[3].Get<std::string>();
        piece.goEntry = fields[4].Get<uint32>();
        piece.editGoEntry = fields[5].Get<uint32>();
        piece.scale = std::max(0.05f, fields[6].Get<float>());
        piece.footprint = std::max(0.1f, fields[7].Get<float>());
        piece.height = std::max(0.0f, fields[8].Get<float>());
        piece.flags = fields[9].Get<uint32>();
        piece.copyCost = fields[10].Get<uint32>();
        piece.sortOrder = fields[11].Get<uint32>();
        piece.hint = fields[12].Get<std::string>();
        piece.legacyCatalogId = fields[13].Get<uint32>();
        piece.outlineMinX = fields[14].Get<float>();
        piece.outlineMinY = fields[15].Get<float>();
        piece.outlineMaxX = fields[16].Get<float>();
        piece.outlineMaxY = fields[17].Get<float>();
        piece.creatureEntry = fields[18].Get<uint32>();

        // The catalog of every object is there only when the server asks for it.
        if (piece.category == CATEGORY_CATALOG && !_catalogEverything)
            continue;

        // Stands and figurines are creatures, not objects: they have no gameobject.
        bool needsObject = !piece.IsCreature();
        bool figureOk = !piece.HasFlag(PIECE_FLAG_FIGURE) || sObjectMgr->GetCreatureTemplate(piece.creatureEntry);
        if (!sObjectMgr->GetItemTemplate(piece.itemEntry) || (needsObject && !sObjectMgr->GetGameObjectTemplate(piece.goEntry)) ||
            (piece.editGoEntry && !sObjectMgr->GetGameObjectTemplate(piece.editGoEntry)) || !figureOk)
        {
            LOG_WARN("module", "mod-playerhousing: Piece {} ({}) is missing its item or object template; skipped.", piece.itemEntry, piece.name);
            continue;
        }

        _pieces[piece.itemEntry] = piece;
    } while (pieceResult->NextRow());

    if (QueryResult ruleResult = WorldDatabase.Query(
            "SELECT item_entry, rule_type, param1, param2, rule_group FROM mod_playerhousing_piece_rule ORDER BY item_entry, rule_group, rule_index"))
    {
        do
        {
            Field* fields = ruleResult->Fetch();
            auto pieceItr = _pieces.find(fields[0].Get<uint32>());
            if (pieceItr == _pieces.end())
                continue;

            PieceRule rule;
            rule.type = fields[1].Get<uint8>();
            rule.param1 = fields[2].Get<uint32>();
            rule.param2 = fields[3].Get<uint32>();
            rule.group = fields[4].Get<uint8>();
            pieceItr->second.rules.push_back(rule);

            // Events look pieces up by what changed: level and skill changes by type alone.
            uint32 key = (rule.type == RULE_LEVEL) ? 0 : rule.param1;
            _piecesByRule[{ rule.type, key }].push_back(pieceItr->first);
        } while (ruleResult->NextRow());
    }

    LOG_INFO("server.loading", "mod-playerhousing: Loaded layout '{}' and {} pieces.", _layout.code, _pieces.size());
    return !_pieces.empty();
}

void PlayerHousingMgr::ConvertLegacyData()
{
    // Placements from the old catalog point at the piece that replaced their catalog entry.
    for (auto const& [itemEntry, piece] : _pieces)
    {
        if (!piece.legacyCatalogId)
            continue;

        CharacterDatabase.DirectExecute(
            "UPDATE mod_playerhousing_placement SET source_item_entry={} WHERE source_item_entry=0 AND catalog_id={}",
            itemEntry, piece.legacyCatalogId);
    }

    // Old catalog unlocks become Collection unlocks plus one copy in the Collection.
    if (QueryResult unlockResult = CharacterDatabase.Query(
            "SELECT u.owner_guid, u.catalog_id, c.account FROM mod_playerhousing_unlock u JOIN characters c ON c.guid = u.owner_guid"))
    {
        uint32 converted = 0;
        do
        {
            Field* fields = unlockResult->Fetch();
            uint32 owner = fields[0].Get<uint32>();
            uint32 catalogId = fields[1].Get<uint32>();
            uint32 account = fields[2].Get<uint32>();
            for (auto const& [itemEntry, piece] : _pieces)
            {
                if (piece.legacyCatalogId != catalogId)
                    continue;

                CharacterDatabase.DirectExecute(
                    "INSERT IGNORE INTO mod_playerhousing_collection (account_id, guid, item_entry) VALUES ({}, 0, {})", account, itemEntry);
                AddToStorage(owner, itemEntry, 1);
                ++converted;
            }
        } while (unlockResult->NextRow());

        CharacterDatabase.DirectExecute("DELETE FROM mod_playerhousing_unlock");
        LOG_INFO("server.loading", "mod-playerhousing: Converted {} old catalog unlocks into Collection unlocks.", converted);
    }

    // With the guild hall removed, whatever stood inside it goes back to its owner's storage,
    // once: later placements on the same ground are the owner's new home.
    if (_layout.code == "cleared")
    {
        QueryResult done = CharacterDatabase.Query("SELECT 1 FROM mod_playerhousing_meta WHERE meta_key='hall_cleared'");
        if (!done)
        {
            if (QueryResult hallResult = CharacterDatabase.Query(
                    "SELECT owner_guid, placement_id, source_item_entry FROM mod_playerhousing_placement "
                    "WHERE map_id={} AND pos_x BETWEEN {} AND {} AND pos_y BETWEEN {} AND {}",
                    _layout.mapId, HALL_MIN_X, HALL_MAX_X, HALL_MIN_Y, HALL_MAX_Y))
            {
                do
                {
                    Field* fields = hallResult->Fetch();
                    uint32 owner = fields[0].Get<uint32>();
                    uint32 itemEntry = fields[2].Get<uint32>();
                    if (_pieces.count(itemEntry))
                        AddToStorage(owner, itemEntry, 1);
                    DeletePlacement(owner, fields[1].Get<uint32>());
                    CharacterDatabase.DirectExecute("UPDATE mod_playerhousing_house SET flags = flags | {} WHERE owner_guid={}", uint32(HOUSE_FLAG_HALL_NOTICE), owner);
                } while (hallResult->NextRow());
            }
            CharacterDatabase.DirectExecute("REPLACE INTO mod_playerhousing_meta (meta_key, meta_value) VALUES ('hall_cleared', 1)");
        }
    }
    else
        CharacterDatabase.DirectExecute("DELETE FROM mod_playerhousing_meta WHERE meta_key='hall_cleared'");
}

void PlayerHousingMgr::OnStartup()
{
    LoadConfig();

    if (!_enabled)
    {
        LOG_INFO("server.loading", "mod-playerhousing: Disabled by config.");
        return;
    }

    if (!LoadDefinitions())
    {
        LOG_ERROR("module", "mod-playerhousing: Definitions failed to load. Module will remain inactive until SQL is applied.");
        _enabled = false;
        return;
    }

    ConvertLegacyData();
    MergeAccountHomes();
    PurgeLeftovers();
    _ready = true;
}

bool PlayerHousingMgr::IsHousingPhase(uint32 phaseMask)
{
    return (phaseMask & HOUSING_PHASE_FLAG) && !(phaseMask & PHASEMASK_NORMAL);
}

namespace
{
    uint32 GetHousePhase(ObjectGuid::LowType ownerGuid)
    {
        return HOUSING_PHASE_FLAG | ((ownerGuid & 0x3FFFFFFF) << 1);
    }
}

void PlayerHousingMgr::OnBeforeSetPhaseMask(uint32 oldPhaseMask, uint32 newPhaseMask, bool& useCombinedPhases) const
{
    // A house phase is an ID, not a set of bits: objects in one only see the exact same value,
    // which gives every owner a private copy of the island.
    if (IsHousingPhase(newPhaseMask))
        useCombinedPhases = false;
    else if (IsHousingPhase(oldPhaseMask))
        useCombinedPhases = true;
}

Map* PlayerHousingMgr::GetHousingMap() const
{
    return sMapMgr->FindBaseNonInstanceMap(_layout.mapId);
}

bool PlayerHousingMgr::IsOnIslandGround(float x, float y) const
{
    float dx = x - _layout.centerX;
    float dy = y - _layout.centerY;
    return dx * dx + dy * dy <= _layout.radius * _layout.radius;
}

bool PlayerHousingMgr::IsInHousingArea(WorldObject const* object) const
{
    return object && object->GetMapId() == _layout.mapId && IsOnIslandGround(object->GetPositionX(), object->GetPositionY());
}

ObjectGuid::LowType PlayerHousingMgr::GetIslandOwner(Player const* player) const
{
    if (!_enabled || !player || !IsInHousingArea(player))
        return 0;

    std::lock_guard<std::recursive_mutex> guard(_lock);
    auto itr = _playerOwnerByGuid.find(player->GetGUID());
    return itr != _playerOwnerByGuid.end() && _sessionsByOwner.count(itr->second) ? itr->second : 0;
}

bool PlayerHousingMgr::IsOnOwnIsland(Player const* player) const
{
    return player && GetIslandOwner(player) == HomeOf(player);
}

bool PlayerHousingMgr::CanDecorate(Player const* player) const
{
    ObjectGuid::LowType owner = player ? GetIslandOwner(player) : 0;
    if (!owner)
        return false;
    if (owner == HomeOf(player))
        return true;

    std::lock_guard<std::recursive_mutex> guard(_lock);
    auto itr = _sessionsByOwner.find(owner);
    return itr != _sessionsByOwner.end() && itr->second.roommates.count(player->GetGUID().GetCounter());
}

bool PlayerHousingMgr::IsDecorating(Player const* player) const
{
    if (!CanDecorate(player))
        return false;

    std::lock_guard<std::recursive_mutex> guard(_lock);
    auto itr = _sessionsByOwner.find(GetIslandOwner(player));
    return itr != _sessionsByOwner.end() && itr->second.decorating;
}

uint32 PlayerHousingMgr::GetCharacterFlags(ObjectGuid::LowType guid, uint32* tips) const
{
    QueryResult result = CharacterDatabase.Query("SELECT flags, tips FROM mod_playerhousing_character WHERE guid={}", guid);
    if (!result)
    {
        if (tips)
            *tips = 0;
        return 0;
    }

    if (tips)
        *tips = (*result)[1].Get<uint32>();
    return (*result)[0].Get<uint32>();
}

void PlayerHousingMgr::SetCharacterFlag(ObjectGuid::LowType guid, uint32 flag, bool tip) const
{
    if (tip)
        CharacterDatabase.DirectExecute(
            "INSERT INTO mod_playerhousing_character (guid, flags, tips) VALUES ({}, 0, {}) ON DUPLICATE KEY UPDATE tips = tips | {}", guid, flag, flag);
    else
        CharacterDatabase.DirectExecute(
            "INSERT INTO mod_playerhousing_character (guid, flags, tips) VALUES ({}, {}, 0) ON DUPLICATE KEY UPDATE flags = flags | {}", guid, flag, flag);
}

void PlayerHousingMgr::ClearCharacterFlag(ObjectGuid::LowType guid, uint32 flag) const
{
    CharacterDatabase.DirectExecute("UPDATE mod_playerhousing_character SET flags = flags & ~{} WHERE guid={}", flag, guid);
}

void PlayerHousingMgr::TruncateUtf8(std::string& text, size_t maxBytes)
{
    if (text.size() <= maxBytes)
        return;
    size_t cut = maxBytes;
    while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0) == 0x80)
        --cut;
    text.resize(cut);
}

bool PlayerHousingMgr::OnCooldown(Player* player, uint8 kind, uint32 ms, std::string& reason)
{
    uint64 now = GameTime::GetGameTimeMS().count();
    std::lock_guard<std::recursive_mutex> guard(_lock);
    auto& readyAt = _cooldowns[player->GetGUID()];
    if (readyAt[kind] > now)
    {
        reason = Acore::StringFormat("One moment: try again in {} {}.", (readyAt[kind] - now + 999) / 1000,
            (readyAt[kind] - now + 999) / 1000 == 1 ? "second" : "seconds");
        return true;
    }
    readyAt[kind] = now + ms;
    return false;
}

bool PlayerHousingMgr::MayNotify(Player const* sender, ObjectGuid::LowType target, uint8 kind)
{
    // One message of a kind a minute from one player to another; the action itself still
    // happens.
    uint64 now = GameTime::GetGameTimeMS().count();
    std::lock_guard<std::recursive_mutex> guard(_lock);
    uint64& last = _notified[sender->GetGUID().GetCounter()][uint64(target) << 8 | kind];
    if (last && now - last < MINUTE * IN_MILLISECONDS)
        return false;
    last = now;
    return true;
}

bool PlayerHousingMgr::CommandFlood(Player* player)
{
    // Up to 15 commands in any 3 seconds.
    uint64 now = GameTime::GetGameTimeMS().count();
    std::lock_guard<std::recursive_mutex> guard(_lock);
    CommandWindow& window = _commandWindows[player->GetGUID()];
    if (now - window.start > 3000)
    {
        window.start = now;
        window.count = 0;
    }
    return ++window.count > 15;
}

bool PlayerHousingMgr::ShiftFlood(Player* player)
{
    // The addon sends about seven a second while a key is held.
    uint64 now = GameTime::GetGameTimeMS().count();
    std::lock_guard<std::recursive_mutex> guard(_lock);
    CommandWindow& window = _shiftWindows[player->GetGUID()];
    if (now - window.start > 1000)
    {
        window.start = now;
        window.count = 0;
    }
    return ++window.count > 12;
}

bool PlayerHousingMgr::PointFlood(Player* player)
{
    // The addon sends where the mouse points about ten times a second while it moves.
    uint64 now = GameTime::GetGameTimeMS().count();
    std::lock_guard<std::recursive_mutex> guard(_lock);
    CommandWindow& window = _pointWindows[player->GetGUID()];
    if (now - window.start > 1000)
    {
        window.start = now;
        window.count = 0;
    }
    return ++window.count > 25;
}

std::string PlayerHousingMgr::FormatYards(float yards)
{
    // 0.25, 0.5, 1, 2
    std::string text = Acore::StringFormat("{:.2f}", yards);
    while (text.back() == '0')
        text.pop_back();
    if (text.back() == '.')
        text.pop_back();
    return text;
}

void PlayerHousingMgr::QuestEvent(Player* player, uint32 questId)
{
    if (player && player->GetQuestStatus(questId) == QUEST_STATUS_INCOMPLETE)
        player->AreaExploredOrEventHappens(questId);
}

float PlayerHousingMgr::GetGridSize(ObjectGuid::LowType guid) const
{
    {
        std::lock_guard<std::recursive_mutex> guard(_lock);
        auto itr = _gridSizes.find(guid);
        if (itr != _gridSizes.end())
            return itr->second;
    }
    float yards = 0.0f;
    if (QueryResult result = CharacterDatabase.Query("SELECT grid FROM mod_playerhousing_character WHERE guid={}", guid))
        yards = float(result->Fetch()[0].Get<uint8>()) / 4.0f;
    std::lock_guard<std::recursive_mutex> guard(_lock);
    _gridSizes[guid] = yards;
    return yards;
}

void PlayerHousingMgr::SetGridSize(Player* player, float yards, std::string& reason) const
{
    // Quarter yards, up to 4 yards.
    uint32 quarters = yards <= 0.0f ? 0 : std::clamp<uint32>(uint32(std::lround(yards * 4.0f)), 1, 16);
    CharacterDatabase.DirectExecute(
        "INSERT INTO mod_playerhousing_character (guid, flags, tips, grid) VALUES ({}, 0, 0, {}) ON DUPLICATE KEY UPDATE grid = {}",
        player->GetGUID().GetCounter(), quarters, quarters);
    {
        std::lock_guard<std::recursive_mutex> guard(_lock);
        _gridSizes[player->GetGUID().GetCounter()] = float(quarters) / 4.0f;
    }
    if (!quarters)
        reason = "Grid off: pieces go exactly where you click.";
    else
        reason = Acore::StringFormat("Grid on: pieces land on a {} yard grid, new ones face straight or diagonal, and nudges move one square.",
            FormatYards(float(quarters) / 4.0f));
}

void PlayerHousingMgr::Say(Player* player, std::string const& text) const
{
    if (player && player->GetSession())
        ChatHandler(player->GetSession()).PSendSysMessage("Housing: {}", text);
}

void PlayerHousingMgr::Tip(Player* player, uint32 tip, std::string const& text)
{
    if (!player)
        return;

    uint32 tips = 0;
    GetCharacterFlags(player->GetGUID().GetCounter(), &tips);
    if (tips & tip)
        return;

    SetCharacterFlag(player->GetGUID().GetCounter(), tip, true);
    ChatHandler(player->GetSession()).PSendSysMessage("|cff33ff99Tip:|r {}", text);
}

std::string PlayerHousingMgr::NameOf(ObjectGuid::LowType guid) const
{
    std::string name;
    if (sCharacterCache->GetCharacterNameByGuid(ObjectGuid::Create<HighGuid::Player>(guid), name))
        return name;
    return "someone";
}

bool PlayerHousingMgr::ResolvePlayerGuid(std::string const& playerName, ObjectGuid::LowType& guidLow, std::string& normalizedName) const
{
    normalizedName = playerName;
    normalizedName.erase(0, normalizedName.find_first_not_of(" \t"));
    normalizedName.erase(normalizedName.find_last_not_of(" \t") + 1);
    if (!normalizePlayerName(normalizedName))
        return false;

    ObjectGuid guid = sCharacterCache->GetCharacterGuidByName(normalizedName);
    if (!guid)
        return false;

    guidLow = guid.GetCounter();
    return true;
}

void PlayerHousingMgr::GiveKeyFromKrook(Player* player)
{
    ObjectGuid::LowType guid = player->GetGUID().GetCounter();
    std::string reason;
    if (!GiveHouseKey(player, reason))
    {
        Say(player, reason);
        return;
    }

    std::vector<uint32> gifts;
    {
        std::lock_guard<std::recursive_mutex> guard(_lock);
        for (auto const& [itemEntry, piece] : _pieces)
            if (piece.HasFlag(PIECE_FLAG_GIFT))
                gifts.push_back(itemEntry);
    }

    // The first pieces come once an account: a character that already had its key when islands
    // were per character had them too.
    ObjectGuid::LowType home = HomeOf(player);
    bool firstOnAccount = !(GetCharacterFlags(home) & (CHAR_FLAG_GIFTS_GIVEN | CHAR_FLAG_KEY_GIVEN));
    if (firstOnAccount)
    {
        for (uint32 itemEntry : gifts)
            ReturnPiece(player, itemEntry);
        SetCharacterFlag(home, CHAR_FLAG_GIFTS_GIVEN, false);
    }

    SetCharacterFlag(guid, CHAR_FLAG_KEY_GIVEN, false);
    Say(player, firstOnAccount ? "You have a house! Your House Key (or /housing) opens the housing window: Go home takes you there."
        : "Here is your House Key: your island is shared by all your characters. The key (or /housing) opens the housing window. If you lose it, come back to me (I'm by the innkeeper in every capital city) for another.");
}

bool PlayerHousingMgr::HadHouseKey(Player const* player) const
{
    return GetCharacterFlags(player->GetGUID().GetCounter()) & CHAR_FLAG_KEY_GIVEN;
}

bool PlayerHousingMgr::GiveHouseKey(Player* player, std::string& reason)
{
    if (!player)
        return false;

    if (player->HasItemCount(HOUSE_KEY_ITEM, 1, true))
    {
        reason = "You already have your House Key.";
        return false;
    }

    if (!player->AddItem(HOUSE_KEY_ITEM, 1))
    {
        reason = "Your bags are full. Make room, then ask me again for your House Key.";
        return false;
    }

    reason = "Here is your House Key. Right-click it for the housing window.";
    return true;
}

void PlayerHousingMgr::OnPlayerLogin(Player* player)
{
    if (!_enabled || !player)
        return;

    // Playerbots get no house, items or unlocks; one that logged out on an island still leaves it.
    bool bot = player->GetSession()->IsHeadless();
    if (!bot)
        EnsureHouse(HomeOf(player));

    bool legacyEvac = false;
    for (uint32 legacyMapId : LEGACY_HOUSING_MAPS)
        if (player->GetMapId() == legacyMapId && legacyMapId != _layout.mapId)
            legacyEvac = true;

    // Nobody is tracked on an island right after login. Whoever logged out on their own
    // island goes back onto it, where they stood; anyone else goes back to where they came
    // from (SetEntryPoint is stored with the character).
    if (legacyEvac || IsInHousingArea(player))
    {
        bool backHome = false;
        if (!bot && !legacyEvac && (GetCharacterFlags(player->GetGUID().GetCounter()) & CHAR_FLAG_HOME_AT_LOGOUT))
        {
            Position at = player->GetPosition();
            std::string reason;
            backHome = EnterHouse(player, HomeOf(player), reason, false, &at);
        }
        if (!backHome)
            player->TeleportToEntryPoint();
    }

    if (bot)
        return;

    // The House Key comes from Krook, beside the innkeeper in every capital city.
    // Pieces from before the Collection kept count, in the bags or the bank, join it.
    SweepHousingItems(player, true);

    // Past progress counts: anything already earned unlocks now, including pieces added to
    // the Collection since the last login.
    uint32 credited = CreditPastProgress(player);
    if (credited)
    {
        if (GetCharacterFlags(player->GetGUID().GetCounter()) & CHAR_FLAG_VETERAN_DONE)
            Say(player, Acore::StringFormat("{} new pieces unlocked in your Collection.", credited));
        else
            Say(player, Acore::StringFormat("Your past adventures unlocked {} pieces for your Collection.", credited));
    }
    SetCharacterFlag(player->GetGUID().GetCounter(), CHAR_FLAG_VETERAN_DONE, false);

    // Lets the optional client addon know this server has housing.
    SendAddonState(player);
}

void PlayerHousingMgr::OnPlayerLogout(Player* player)
{
    if (!_enabled || !player)
        return;

    if (!player->GetSession()->IsHeadless())
    {
        if (IsOnOwnIsland(player))
            SetCharacterFlag(player->GetGUID().GetCounter(), CHAR_FLAG_HOME_AT_LOGOUT, false);
        else
            ClearCharacterFlag(player->GetGUID().GetCounter(), CHAR_FLAG_HOME_AT_LOGOUT);
    }

    {
        std::lock_guard<std::recursive_mutex> guard(_lock);
        _ambienceTimers.erase(player->GetGUID());
        _cooldowns.erase(player->GetGUID());
        _commandWindows.erase(player->GetGUID());
        _shiftWindows.erase(player->GetGUID());
        _pointWindows.erase(player->GetGUID());
        _editMode.erase(player->GetGUID());
        _addonClients.erase(player->GetGUID());
        _mouseClients.erase(player->GetGUID());
        _localGhostClients.erase(player->GetGUID());
        _knownHouses.erase(player->GetGUID().GetCounter());
        _gridSizes.erase(player->GetGUID().GetCounter());
        _notified.erase(player->GetGUID().GetCounter());
    }

    {
        std::lock_guard<std::recursive_mutex> guard(_lock);
        _pendingTrips.erase(player->GetGUID());
        _pendingTripCount.store(uint32(_pendingTrips.size()), std::memory_order_relaxed);
    }
    EndSessionIfEmpty(RemovePlayerTracking(player->GetGUID(), true));
}

void PlayerHousingMgr::UpdatePendingTrip(Player* player)
{
    PendingTrip trip;
    {
        std::lock_guard<std::recursive_mutex> guard(_lock);
        auto itr = _pendingTrips.find(player->GetGUID());
        if (itr == _pendingTrips.end())
            return;
        trip = itr->second;
    }

    bool moved = player->GetExactDist(trip.x, trip.y, trip.z) > 1.0f;
    if (moved || player->IsInCombat() || player->isDead())
    {
        {
            std::lock_guard<std::recursive_mutex> guard(_lock);
            _pendingTrips.erase(player->GetGUID());
        _pendingTripCount.store(uint32(_pendingTrips.size()), std::memory_order_relaxed);
        }
        Say(player, "Trip home canceled.");
        return;
    }

    if (std::time(nullptr) < trip.at)
        return;

    {
        std::lock_guard<std::recursive_mutex> guard(_lock);
        _pendingTrips.erase(player->GetGUID());
        _pendingTripCount.store(uint32(_pendingTrips.size()), std::memory_order_relaxed);
    }

    std::string reason;
    if (!EnterOwnHouse(player, reason))
        Say(player, reason);
}

void PlayerHousingMgr::OnPlayerUpdate(Player* player, uint32 diffMs)
{
    if (!_enabled || !player)
        return;

    // Most players have nothing to do with housing: not on the island, not in an island's
    // phase, no trip home under way. They leave without touching the lock.
    if (!IsInHousingArea(player) && !IsHousingPhase(player->GetPhaseMask()) && _pendingTripCount.load(std::memory_order_relaxed) == 0)
        return;

    // Every player runs this every update, on every map thread. When another thread holds
    // the housing lock (a big layout being set out, say), skip this update rather than
    // hold up the whole map: it all happens on the next one.
    std::unique_lock<std::recursive_mutex> busy(_lock, std::try_to_lock);
    if (!busy.owns_lock())
        return;

    UpdateAmbience(player, diffMs);
    UpdatePendingTrip(player);

    // Mid-teleport the position still belongs to where the player came from.
    if (player->IsBeingTeleported())
        return;

    bool tracked = false;
    bool arrived = false;
    ObjectGuid::LowType ownerGuid = 0;
    uint32 sessionPhase = 0;
    bool needsInit = false;
    {
        std::lock_guard<std::recursive_mutex> guard(_lock);
        auto playerItr = _playerOwnerByGuid.find(player->GetGUID());
        if (playerItr != _playerOwnerByGuid.end())
        {
            auto sessionItr = _sessionsByOwner.find(playerItr->second);
            if (sessionItr != _sessionsByOwner.end())
            {
                tracked = true;
                ownerGuid = playerItr->second;
                sessionPhase = sessionItr->second.phaseMask;
                needsInit = !sessionItr->second.initialized;
                arrived = _arrivals.erase(player->GetGUID()) > 0;
            }
        }
    }

    bool inArea = IsInHousingArea(player);

    if (tracked && !inArea)
    {
        // Left some other way (hearthstone, summon, death). Only the island map's own
        // thread may despawn the island; from anywhere else the next visit respawns it.
        ObjectGuid::LowType leftOwner = RemovePlayerTracking(player->GetGUID(), true);
        if (player->GetMapId() == _layout.mapId)
            EndSessionIfEmpty(leftOwner);
        RestoreNormalPhase(player);
        return;
    }

    if (!tracked)
    {
        if (IsHousingPhase(player->GetPhaseMask()))
            RestoreNormalPhase(player);

        if (!inArea || player->IsGameMaster() || player->GetSession()->GetSecurity() > SEC_PLAYER)
            return;

        if (player->GetSession()->IsHeadless() && TryAdmitGroupBot(player))
            return;

        // Only people let in with a House Key or a steward belong on the housing island.
        player->TeleportToEntryPoint();
        return;
    }

    // Phase auras and GM mode reset the phase mask; put the player back on the island.
    if (player->GetPhaseMask() != sessionPhase && !player->IsGameMaster())
        ApplyHousePhase(player, sessionPhase);

    if (needsInit)
    {
        std::string reason;
        if (!InitializeSession(ownerGuid, reason))
            LOG_WARN("module", "mod-playerhousing: Could not set up island of owner {}: {}", ownerGuid, reason);
    }

    if (arrived)
        OnArrived(player, ownerGuid);
    UpdateGhost(player);

    // Swimmers and anyone falling through the world go back to the landing spot rather than
    // dropping out of their private copy.
    float dx = player->GetPositionX() - _layout.centerX;
    float dy = player->GetPositionY() - _layout.centerY;
    float edge = _layout.radius - EDGE_MARGIN;
    if (dx * dx + dy * dy > edge * edge || player->GetPositionZ() < _layout.landing.GetPositionZ() - 60.0f)
    {
        player->NearTeleportTo(_layout.landing.GetPositionX(), _layout.landing.GetPositionY(), _layout.landing.GetPositionZ() + 0.5f, _layout.landing.GetOrientation());
        Say(player, "That's the edge of your island. The housing window's Leave takes you off it.");
    }
}

void PlayerHousingMgr::OnArrived(Player* player, ObjectGuid::LowType ownerGuid)
{
    // Whatever the addon thought before (a ghost left behind on another island, say) is over.
    SendAddonState(player);

    HouseRecord house;
    if (!GetHouseRecord(ownerGuid, house))
        return;

    SendAmbience(player, house, true);
    LogVisit(ownerGuid, player);

    if (HomeOf(player) == ownerGuid)
    {
        if (house.flags & HOUSE_FLAG_HALL_NOTICE)
        {
            Say(player, "The old guild hall is gone. Everything you had placed inside it is back in your Collection.");
            CharacterDatabase.DirectExecute("UPDATE mod_playerhousing_house SET flags = flags & ~{} WHERE owner_guid={}", uint32(HOUSE_FLAG_HALL_NOTICE), ownerGuid);
        }

        QuestEvent(player, QUEST_TOUR_HOME);

        // Who came by since the owner was last home.
        if (QueryResult result = CharacterDatabase.Query(
                "SELECT COUNT(*) FROM mod_playerhousing_visit_log l JOIN mod_playerhousing_house h ON h.owner_guid = l.owner_guid "
                "WHERE l.owner_guid={} AND l.visited_at > h.last_home", ownerGuid))
            if (uint32 visits = uint32((*result)[0].Get<uint64>()))
                Say(player, Acore::StringFormat("{} since you were last home. Island tab, Visitor log.",
                    visits == 1 ? std::string("One visit") : Acore::StringFormat("{} visits", visits)));
        CharacterDatabase.DirectExecute("UPDATE mod_playerhousing_house SET last_home=NOW() WHERE owner_guid={}", ownerGuid);
        if (uint32 notes = CountNewNotes(ownerGuid))
            Say(player, Acore::StringFormat("{} in your guestbook. Island tab, Guestbook.",
                notes == 1 ? std::string("A new note") : Acore::StringFormat("{} new notes", notes)));

        // Once an account (the flag sits on the home character).
        if (!(GetCharacterFlags(ownerGuid) & CHAR_FLAG_GREETED))
        {
            SetCharacterFlag(ownerGuid, CHAR_FLAG_GREETED, false);
            ChatHandler handler(player->GetSession());
            handler.SendSysMessage("|cffffd000Krook:|r Welcome to your island! Every character of yours shares it. Three things to know:");
            handler.SendSysMessage("|cffffd000Krook:|r 1. Click a piece in the housing window's Collection: it goes on your mouse. Click where it should go.");
            handler.SendSysMessage("|cffffd000Krook:|r 2. Right-click a piece on the island to pick it up again. The mouse wheel turns it; the window lists the rest.");
            handler.SendSysMessage("|cffffd000Krook:|r 3. Don't like it? Undo. Nothing is ever lost: picked up pieces go back to your Collection.");
        }
        SendAddonState(player);
        return;
    }

    std::string ownerName = NameOf(ownerGuid);
    if (!house.greeting.empty())
        ChatHandler(player->GetSession()).PSendSysMessage("|cffffd000{}'s island:|r {}", ownerName, house.greeting);
    else
        Say(player, Acore::StringFormat("Welcome to {}'s island.", ownerName));

    if (!player->GetSession()->IsHeadless())
        if (Player* owner = FindOwnerOnline(ownerGuid))
            if (owner != player)
                Say(owner, Acore::StringFormat("{} arrived on your island.", player->GetName()));
}

void PlayerHousingMgr::OnPlayerMapChanged(Player* player)
{
    if (!_enabled || !player)
        return;

    bool tracked = false;
    {
        std::lock_guard<std::recursive_mutex> guard(_lock);
        tracked = _playerOwnerByGuid.count(player->GetGUID()) > 0;
    }

    // Arriving at the island from another map.
    if (tracked && IsInHousingArea(player))
        return;

    // Runs on the world thread, so the island left behind can be despawned here.
    if (tracked)
    {
        EndSessionIfEmpty(RemovePlayerTracking(player->GetGUID(), true));
    }

    if (IsHousingPhase(player->GetPhaseMask()))
        RestoreNormalPhase(player);
}

void PlayerHousingMgr::OnPlayerDelete(ObjectGuid guid)
{
    if (!_enabled)
        return;

    // Every deletion, also one a GM can undo: the island closes, and what roommates put on it
    // goes back to them now, since nobody can reach it any more. The rest of the island stays
    // until the character is gone for good (OnPlayerDeleteFromDB), so a restored character
    // finds it as it was.
    ObjectGuid::LowType guidLow = guid.GetCounter();
    // The account's island is kept under this character: it moves, as it is, to another of the
    // account's characters (then nothing is left under this one to close).
    RehomeAccountOf(guidLow);
    RemovePlayerTracking(guid, true);

    std::lock_guard<std::recursive_mutex> guard(_lock);
    // Guests still there are moved out by OnPlayerUpdate once the session is gone.
    auto sessionItr = _sessionsByOwner.find(guidLow);
    if (sessionItr != _sessionsByOwner.end())
    {
        if (Map* map = GetHousingMap())
            DespawnSessionObjects(sessionItr->second, map);

        for (ObjectGuid const& occupant : sessionItr->second.occupants)
        {
            _playerOwnerByGuid.erase(occupant);
            // What followed them goes with the island.
            auto carrying = _carrying.find(occupant);
            if (carrying != _carrying.end())
            {
                DespawnGhost(carrying->second, GetHousingMap());
                _carrying.erase(carrying);
            }
        }

        _sessionsByOwner.erase(sessionItr);
    }
    _journals.erase(guidLow);
    ForgetJournals(guidLow);
    ReturnRoommatePieces(guidLow);
}

void PlayerHousingMgr::OnPlayerDeleteFromDB(ObjectGuid::LowType guidLow)
{
    // Before the module is up (the core purges old deleted characters early in startup), or
    // while it's off, PurgeLeftovers catches up at the next start.
    if (_ready && !RehomeAccountOf(guidLow))
        RemoveHousingOf(guidLow);
}

void PlayerHousingMgr::RemoveHousingOf(ObjectGuid::LowType guidLow)
{
    std::lock_guard<std::recursive_mutex> guard(_lock);
    ReturnRoommatePieces(guidLow);

    CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();
    trans->Append("DELETE FROM mod_playerhousing_acl WHERE owner_guid={} OR guest_guid={}", guidLow, guidLow);
    trans->Append("DELETE FROM mod_playerhousing_placement WHERE owner_guid={}", guidLow);
    // Gear on stands is out of the inventory, so the core's character deletion misses it. The
    // items go before the rows that point at them. (Only the owner's gear is left by now.)
    trans->Append("DELETE ii FROM item_instance ii JOIN mod_playerhousing_placement_gear g ON g.item_guid = ii.guid WHERE g.owner_guid={}", guidLow);
    trans->Append("DELETE FROM mod_playerhousing_placement_gear WHERE owner_guid={}", guidLow);
    trans->Append("DELETE FROM mod_playerhousing_storage WHERE owner_guid={}", guidLow);
    trans->Append("DELETE FROM mod_playerhousing_saved_piece WHERE owner_guid={}", guidLow);
    trans->Append("DELETE FROM mod_playerhousing_like WHERE owner_guid={} OR liker_guid={}", guidLow, guidLow);
    trans->Append("DELETE FROM mod_playerhousing_report WHERE owner_guid={}", guidLow);
    trans->Append("DELETE FROM mod_playerhousing_visit_log WHERE owner_guid={} OR visitor_guid={}", guidLow, guidLow);
    trans->Append("DELETE FROM mod_playerhousing_saved_layout WHERE owner_guid={}", guidLow);
    trans->Append("DELETE FROM mod_playerhousing_set_piece WHERE owner_guid={}", guidLow);
    trans->Append("DELETE FROM mod_playerhousing_set WHERE owner_guid={}", guidLow);
    trans->Append("DELETE FROM mod_playerhousing_guestbook WHERE owner_guid={} OR author_guid={}", guidLow, guidLow);
    trans->Append("DELETE FROM mod_playerhousing_collection WHERE guid={}", guidLow);
    trans->Append("DELETE FROM mod_playerhousing_character WHERE guid={}", guidLow);
    trans->Append("DELETE FROM mod_playerhousing_house WHERE owner_guid={}", guidLow);
    CharacterDatabase.CommitTransaction(trans);
    _knownHouses.erase(guidLow);
}

void PlayerHousingMgr::ReturnRoommatePieces(ObjectGuid::LowType ownerGuid)
{
    std::lock_guard<std::recursive_mutex> guard(_lock);
    std::map<uint32, Placement> theirs;
    if (QueryResult result = CharacterDatabase.Query(
            "SELECT placement_id, source_item_entry, placed_by FROM mod_playerhousing_placement WHERE owner_guid={} AND placed_by NOT IN (0, {})",
            ownerGuid, ownerGuid))
    {
        do
        {
            Placement placement;
            placement.id = (*result)[0].Get<uint32>();
            placement.itemEntry = (*result)[1].Get<uint32>();
            placement.placedBy = (*result)[2].Get<uint32>();
            theirs[placement.id] = placement;
        } while (result->NextRow());
    }
    if (theirs.empty())
        return;

    LoadGear(ownerGuid, theirs);
    _report = {};
    for (auto const& [id, placement] : theirs)
    {
        for (auto const& [slot, gear] : placement.gear)
            ReturnGear(nullptr, ownerGuid, placement.placedBy, id, slot, gear);
        DeletePlacement(ownerGuid, id);
        CharacterDatabase.DirectExecute("UPDATE mod_playerhousing_placement SET parent_id=0 WHERE owner_guid={} AND parent_id={}", ownerGuid, id);
        AddToStorage(placement.placedBy, placement.itemEntry, 1);
    }
    LOG_INFO("module", "mod-playerhousing: The island of {} closed: {} pieces went back to the roommates who placed them.", ownerGuid, theirs.size());
}

void PlayerHousingMgr::PurgeLeftovers()
{
    // Characters removed for good while the module wasn't running, or before it was up.
    QueryResult result = CharacterDatabase.Query(
        "SELECT h.guid FROM (SELECT owner_guid AS guid FROM mod_playerhousing_house UNION SELECT owner_guid FROM mod_playerhousing_placement "
        "UNION SELECT owner_guid FROM mod_playerhousing_storage UNION SELECT guid FROM mod_playerhousing_character "
        "UNION SELECT owner_guid FROM mod_playerhousing_set UNION SELECT author_guid FROM mod_playerhousing_guestbook) h "
        "LEFT JOIN characters c ON c.guid = h.guid WHERE c.guid IS NULL");
    if (!result)
        return;

    uint32 count = 0;
    do
    {
        // An account's home character: the island goes to another of its characters.
        if (!RehomeAccountOf((*result)[0].Get<uint32>()))
            RemoveHousingOf((*result)[0].Get<uint32>());
        ++count;
    } while (result->NextRow());
    LOG_INFO("server.loading", "mod-playerhousing: Removed the housing of {} characters deleted for good.", count);
}

bool PlayerHousingMgr::EnsureSession(ObjectGuid::LowType ownerGuid)
{
    if (!GetHousingMap())
        return false;

    std::lock_guard<std::recursive_mutex> guard(_lock);
    Session& session = _sessionsByOwner[ownerGuid];
    if (session.ownerGuid == 0)
    {
        session.ownerGuid = ownerGuid;
        session.phaseMask = GetHousePhase(ownerGuid);
        session.mapId = _layout.mapId;
    }

    // An empty session may still hold objects from an earlier visit, or none at all if the
    // grid unloaded meanwhile; respawn everything when someone arrives.
    if (session.occupants.empty())
        session.initialized = false;
    return true;
}

bool PlayerHousingMgr::InitializeSession(ObjectGuid::LowType ownerGuid, std::string& reason)
{
    std::lock_guard<std::recursive_mutex> guard(_lock);

    auto sessionItr = _sessionsByOwner.find(ownerGuid);
    if (sessionItr == _sessionsByOwner.end())
    {
        reason = "Island session does not exist.";
        return false;
    }

    Session& session = sessionItr->second;
    if (session.initialized)
        return true;

    Map* map = GetHousingMap();
    if (!map)
    {
        reason = "Could not load the island map.";
        return false;
    }

    DespawnSessionObjects(session, map);
    session.placements.clear();
    // A reload with people still there (the owner left mid-decorating) keeps counting up: their
    // undo lists may name ids of pieces picked up since, and those ids must not come back.

    if (QueryResult result = CharacterDatabase.Query(
            "SELECT placement_id, source_item_entry, pos_x, pos_y, pos_z, orientation, scale, look, parent_id, pitch, roll, placed_by FROM mod_playerhousing_placement "
            "WHERE owner_guid={} AND map_id={} ORDER BY placement_id", ownerGuid, session.mapId))
    {
        do
        {
            Field* fields = result->Fetch();
            Placement placement;
            placement.id = fields[0].Get<uint32>();
            placement.itemEntry = fields[1].Get<uint32>();
            placement.x = fields[2].Get<float>();
            placement.y = fields[3].Get<float>();
            placement.z = fields[4].Get<float>();
            placement.o = fields[5].Get<float>();
            placement.scale = std::max(0.05f, fields[6].Get<float>());
            placement.look = fields[7].Get<uint64>();
            placement.parent = fields[8].Get<uint32>();
            placement.pitch = fields[9].Get<float>();
            placement.roll = fields[10].Get<float>();
            placement.placedBy = fields[11].Get<uint32>();
            session.nextPlacementId = std::max(session.nextPlacementId, placement.id + 1);
            if (!_pieces.count(placement.itemEntry))
                continue;

            session.placements[placement.id] = placement;
        } while (result->NextRow());
    }

    LoadGear(ownerGuid, session.placements);
    session.roommates.clear();
    if (QueryResult result = CharacterDatabase.Query("SELECT guest_guid FROM mod_playerhousing_acl WHERE owner_guid={} AND roommate=1", ownerGuid))
    {
        do
        {
            session.roommates.insert((*result)[0].Get<uint32>());
        } while (result->NextRow());
    }
    for (auto const& [id, placement] : session.placements)
        SpawnPlacement(session, map, placement);

    // Placement ids are also used by rows of pieces no longer defined; never reuse those.
    if (QueryResult maxResult = CharacterDatabase.Query("SELECT IFNULL(MAX(placement_id), 0) FROM mod_playerhousing_placement WHERE owner_guid={}", ownerGuid))
        session.nextPlacementId = std::max(session.nextPlacementId, (*maxResult)[0].Get<uint32>() + 1);

    // Krook stands by until the welcome tour is done; then he comes only when called.
    if (!TourDone(ownerGuid))
        SpawnSteward(session, map);

    HouseRecord house;
    if (GetHouseRecord(ownerGuid, house) && !(house.flags & HOUSE_FLAG_WRECKAGE_PLACED))
    {
        PlaceStarterWreckage(session, map);
        CharacterDatabase.DirectExecute("UPDATE mod_playerhousing_house SET flags = flags | {} WHERE owner_guid={}", uint32(HOUSE_FLAG_WRECKAGE_PLACED), ownerGuid);
    }

    session.initialized = true;
    return true;
}

void PlayerHousingMgr::DespawnSessionObjects(Session& session, Map* map)
{
    if (!map)
        return;

    for (auto const& [placementId, spawned] : session.spawned)
        RemoveSpawned(map, spawned.guid);
    for (auto const& [player, highlight] : session.highlights)
        if (GameObject* ring = map->GetGameObject(highlight.ring))
            ring->AddObjectToRemoveList();
    session.highlights.clear();

    DespawnPhoto(session, map);

    if (session.stewardGuid)
        if (Creature* steward = map->GetCreature(session.stewardGuid))
            steward->AddObjectToRemoveList();

    session.spawned.clear();
    session.stewardGuid.Clear();
}

void PlayerHousingMgr::EndSessionIfEmpty(ObjectGuid::LowType ownerGuid)
{
    if (!ownerGuid)
        return;

    std::lock_guard<std::recursive_mutex> guard(_lock);

    auto sessionItr = _sessionsByOwner.find(ownerGuid);
    if (sessionItr == _sessionsByOwner.end() || !sessionItr->second.occupants.empty())
        return;

    if (Map* map = GetHousingMap())
        DespawnSessionObjects(sessionItr->second, map);

    _sessionsByOwner.erase(sessionItr);
}

ObjectGuid::LowType PlayerHousingMgr::RemovePlayerTracking(ObjectGuid playerGuid, bool eraseReturnLocation)
{
    std::lock_guard<std::recursive_mutex> guard(_lock);

    ObjectGuid::LowType ownerGuid = 0;
    auto playerItr = _playerOwnerByGuid.find(playerGuid);
    if (playerItr != _playerOwnerByGuid.end())
    {
        ownerGuid = playerItr->second;
        auto sessionItr = _sessionsByOwner.find(ownerGuid);
        if (sessionItr != _sessionsByOwner.end())
        {
            sessionItr->second.occupants.erase(playerGuid);

            // The owner leaving ends decorating. Anyone still there gets the island respawned
            // in its normal form by their next update. Whoever leaves loses their undo list.
            if (playerGuid.GetCounter() == ownerGuid && sessionItr->second.decorating)
            {
                sessionItr->second.decorating = false;
                sessionItr->second.initialized = false;
            }
            sessionItr->second.selected.erase(playerGuid.GetCounter());
            sessionItr->second.groups.erase(playerGuid.GetCounter());
            RemoveHighlight(sessionItr->second, GetHousingMap(), playerGuid.GetCounter());
            _journals.erase(playerGuid.GetCounter());
        }

        _playerOwnerByGuid.erase(playerItr);
    }

    _arrivals.erase(playerGuid);
    _editMode.erase(playerGuid);  // or it would come back with decorating, next visit
    _groupHold.erase(playerGuid);
    // Whatever was following them goes; nothing was set down.
    auto carrying = _carrying.find(playerGuid);
    if (carrying != _carrying.end())
    {
        DespawnGhost(carrying->second, GetHousingMap());
        _carrying.erase(carrying);
    }
    if (_photoTours.erase(playerGuid) && ownerGuid)
    {
        // A GM leaving mid-tour takes the building being photographed along.
        auto sessionItr = _sessionsByOwner.find(ownerGuid);
        if (sessionItr != _sessionsByOwner.end())
            DespawnPhoto(sessionItr->second, GetHousingMap());
    }
    if (eraseReturnLocation)
        _returnLocations.erase(playerGuid);

    return ownerGuid;
}

bool PlayerHousingMgr::TryAdmitGroupBot(Player* bot)
{
    Group* group = bot ? bot->GetGroup() : nullptr;
    if (!group)
        return false;

    uint32 phaseMask = 0;
    {
        std::lock_guard<std::recursive_mutex> guard(_lock);

        // Bots follow their party onto whichever island a party member is on.
        ObjectGuid::LowType ownerGuid = 0;
        for (Group::MemberSlot const& slot : group->GetMemberSlots())
        {
            if (slot.guid == bot->GetGUID())
                continue;

            auto memberItr = _playerOwnerByGuid.find(slot.guid);
            if (memberItr != _playerOwnerByGuid.end())
            {
                ownerGuid = memberItr->second;
                break;
            }
        }

        auto sessionItr = ownerGuid ? _sessionsByOwner.find(ownerGuid) : _sessionsByOwner.end();
        if (sessionItr == _sessionsByOwner.end() || bot->GetMapId() != sessionItr->second.mapId)
            return false;

        sessionItr->second.occupants.insert(bot->GetGUID());
        _playerOwnerByGuid[bot->GetGUID()] = ownerGuid;
        phaseMask = sessionItr->second.phaseMask;
    }

    ApplyHousePhase(bot, phaseMask);
    return true;
}

void PlayerHousingMgr::ApplyHousePhase(Player* player, uint32 phaseMask) const
{
    // Unit::SetPhaseMask carries pets, guardians and other summons along.
    player->SetPhaseMask(phaseMask, true);
}

void PlayerHousingMgr::RestoreNormalPhase(Player* player)
{
    if (!IsHousingPhase(player->GetPhaseMask()))
        return;

    RestoreAmbience(player);

    uint32 phaseMask = player->GetPhaseByAuras();
    if (!phaseMask)
        phaseMask = PHASEMASK_NORMAL;
    if (player->IsGameMaster())
        phaseMask = PHASEMASK_ANYWHERE;

    player->SetPhaseMask(phaseMask, true);
}

bool PlayerHousingMgr::EnterHouse(Player* player, ObjectGuid::LowType ownerGuid, std::string& reason, bool force, Position const* at)
{
    if (!_enabled || !player)
    {
        reason = "Housing is disabled.";
        return false;
    }

    if (player->IsInCombat())
    {
        reason = "You can't travel to an island while in combat.";
        return false;
    }

    if (player->IsInFlight())
    {
        reason = "You can't travel to an island while flying on a taxi.";
        return false;
    }

    if (ownerGuid == HomeOf(player))
        EnsureHouse(ownerGuid);

    HouseRecord house;
    if (!GetHouseRecord(ownerGuid, house))
    {
        reason = "That player doesn't have an island yet.";
        return false;
    }

    if (!force && !CanVisit(player, house, reason))
        return false;

    if (!EnsureSession(ownerGuid))
    {
        reason = "The island map is not available.";
        return false;
    }

    // Coming back at a spot (after logging out there) keeps the way out from before.
    bool wasOnIsland = at || (IsInHousingArea(player) && GetIslandOwner(player) != 0);
    ObjectGuid::LowType previousOwner = 0;
    uint32 phaseMask = 0;
    {
        std::lock_guard<std::recursive_mutex> guard(_lock);
        auto sessionItr = _sessionsByOwner.find(ownerGuid);
        if (sessionItr == _sessionsByOwner.end())
        {
            reason = "Island session was not created.";
            return false;
        }

        previousOwner = RemovePlayerTracking(player->GetGUID(), false);
        sessionItr = _sessionsByOwner.find(ownerGuid);
        if (sessionItr == _sessionsByOwner.end())
        {
            reason = "Island session was not created.";
            return false;
        }

        if (!wasOnIsland)
            _returnLocations[player->GetGUID()] = WorldLocation(player->GetMapId(), player->GetPositionX(), player->GetPositionY(), player->GetPositionZ(), player->GetOrientation());

        sessionItr->second.occupants.insert(player->GetGUID());
        _playerOwnerByGuid[player->GetGUID()] = ownerGuid;
        _arrivals.insert(player->GetGUID());
        phaseMask = sessionItr->second.phaseMask;
    }

    if (previousOwner && previousOwner != ownerGuid)
        EndSessionIfEmpty(previousOwner);

    // Stored with the character, so logging out on the island returns them here.
    if (!wasOnIsland)
        player->SetEntryPoint();

    ApplyHousePhase(player, phaseMask);

    // The island itself is spawned by the first occupant's update once they are on the map.
    // Visitors come in at the owner's door, if there is one.
    Position arrival = _layout.landing;
    if (house.hasDoor && ownerGuid != HomeOf(player)
            && IsSpotOnIsland(house.door.GetPositionX(), house.door.GetPositionY(), house.door.GetPositionZ()))
        arrival = house.door;
    if (at)
        arrival = *at;
    if (!player->TeleportTo(_layout.mapId, arrival.GetPositionX(), arrival.GetPositionY(), arrival.GetPositionZ() + 0.35f, arrival.GetOrientation()))
    {
        EndSessionIfEmpty(RemovePlayerTracking(player->GetGUID(), !wasOnIsland));
        RestoreNormalPhase(player);
        reason = "Teleport to the island failed.";
        return false;
    }

    // Kept up to date on the way in too, in case the server stops without logging them out.
    if (!player->GetSession()->IsHeadless())
    {
        if (ownerGuid == HomeOf(player))
            SetCharacterFlag(player->GetGUID().GetCounter(), CHAR_FLAG_HOME_AT_LOGOUT, false);
        else
            ClearCharacterFlag(player->GetGUID().GetCounter(), CHAR_FLAG_HOME_AT_LOGOUT);
    }
    return true;
}

bool PlayerHousingMgr::EnterOwnHouse(Player* player, std::string& reason)
{
    return player && EnterHouse(player, HomeOf(player), reason);
}

bool PlayerHousingMgr::VisitHouse(Player* player, ObjectGuid::LowType ownerGuid, std::string& reason)
{
    return player && EnterHouse(player, ownerGuid, reason);
}

bool PlayerHousingMgr::VisitHouseByName(Player* player, std::string const& ownerName, std::string& reason)
{
    ObjectGuid::LowType ownerGuid = 0;
    std::string normalizedName;
    if (!ResolvePlayerGuid(ownerName, ownerGuid, normalizedName))
    {
        reason = "No character with that name.";
        return false;
    }

    // Any of the owner's characters: their account's island.
    return VisitHouse(player, HomeOfCharacter(ownerGuid), reason);
}

bool PlayerHousingMgr::RequestGoHome(Player* player, std::string& reason)
{
    if (!player)
        return false;

    if (IsOnOwnIsland(player))
    {
        reason = "You're already home.";
        return false;
    }

    // The way home is the House Key, which Krook hands out beside every capital's innkeeper.
    if (!player->IsGameMaster() && !player->HasItemCount(HOUSE_KEY_ITEM, 1, true))
    {
        reason = "You need a House Key to go home. Krook, beside the innkeeper in every capital city, has one for you.";
        return false;
    }

    if (_freeMode || _keyDelaySeconds == 0 || player->IsGameMaster())
        return EnterOwnHouse(player, reason);

    if (player->IsInCombat())
    {
        reason = "You can't travel home while in combat.";
        return false;
    }

    {
        std::lock_guard<std::recursive_mutex> guard(_lock);
        PendingTrip& trip = _pendingTrips[player->GetGUID()];
        _pendingTripCount.store(uint32(_pendingTrips.size()), std::memory_order_relaxed);
        trip.at = std::time(nullptr) + _keyDelaySeconds;
        trip.x = player->GetPositionX();
        trip.y = player->GetPositionY();
        trip.z = player->GetPositionZ();
    }

    reason = Acore::StringFormat("Heading home in {} seconds. Moving cancels.", _keyDelaySeconds);
    return true;
}

bool PlayerHousingMgr::LeaveHouse(Player* player, std::string& reason)
{
    if (!_enabled || !player)
    {
        reason = "Housing is disabled.";
        return false;
    }

    if (!GetIslandOwner(player))
    {
        reason = "You're not on an island.";
        return false;
    }

    WorldLocation returnLocation;
    bool hasReturn = false;
    {
        std::lock_guard<std::recursive_mutex> guard(_lock);
        auto itr = _returnLocations.find(player->GetGUID());
        if (itr != _returnLocations.end())
        {
            returnLocation = itr->second;
            hasReturn = true;
        }
    }

    EndSessionIfEmpty(RemovePlayerTracking(player->GetGUID(), true));
    RestoreNormalPhase(player);

    bool teleportOk = hasReturn ? player->TeleportTo(returnLocation) : player->TeleportToEntryPoint();
    if (!teleportOk)
    {
        reason = "Could not leave the island.";
        return false;
    }

    return true;
}

bool PlayerHousingMgr::Unstuck(Player* player, std::string& reason)
{
    if (!player || !GetIslandOwner(player))
    {
        reason = "Unstuck works on an island. Elsewhere, use your hearthstone.";
        return false;
    }

    Position const& landing = _layout.landing;
    player->NearTeleportTo(landing.GetPositionX(), landing.GetPositionY(), landing.GetPositionZ() + 0.5f, landing.GetOrientation());
    reason = "Back at the landing spot.";
    return true;
}

void PlayerHousingMgr::SendAddonState(Player* player) const
{
    if (!player || !player->GetSession() || player->GetSession()->IsHeadless())
        return;

    ObjectGuid::LowType owner = GetIslandOwner(player);
    bool own = owner && owner == HomeOf(player);
    bool roommate = !own && CanDecorate(player);
    uint32 selected = (own || roommate) ? GetSelectedPlacement(player) : 0;
    std::string selectedName;
    bool selectedBuilding = false;
    bool selectedStand = false;
    if (selected)
    {
        if (std::optional<Placement> placement = GetPlacement(player, selected))
        {
            selectedName = PieceName(placement->itemEntry);
            if (PieceDefinition const* piece = GetPiece(placement->itemEntry))
            {
                selectedBuilding = piece->IsBuilding();
                selectedStand = piece->HasFlag(PIECE_FLAG_STAND);
            }
        }
        else
            selected = 0;  // picked up since
    }

    uint32 furnishings = 0;
    uint32 buildings = 0;
    if (own || roommate)
        CountPlaced(owner, furnishings, buildings);

    // Read by client-addon/PlayerHousing: tab separated, new fields only ever go at the end.
    size_t groupSize = (own || roommate) ? GetGroup(player).size() : 0;
    uint32 ghostItem = GetGhostItem(player);

    // Fields 15 and 19 (a copy waiting for its spot, edit mode's Move a Piece) went with ghosts,
    // and 14 (Move a Piece waiting for its circle) with the circle: 0.
    std::string message = Acore::StringFormat("state\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}",
        own ? 1 : 0, IsDecorating(player) ? 1 : 0, selected, selectedName,
        furnishings, _maxFurnishings, buildings, _maxBuildings, UndoLabel(player),
        owner ? NameOf(owner) : "", (own || roommate) ? RedoLabel(player) : "", selectedBuilding ? 1 : 0, 0,
        0, roommate ? 1 : 0, IsInEditMode(player) ? 1 : 0,
        FormatYards(GetGridSize(player->GetGUID().GetCounter())), 0, groupSize,
        ghostItem, ghostItem ? (IsGhostMove(player) ? "move" : "new") : "", _ghosts ? 1 : 0, ghostItem ? GetGhostNote(player) : "",
        selectedStand ? 1 : 0);

    // What follows the player: its size (percent), tilt (degrees forward and to its right),
    // how many pieces come with it, and where its front points relative to the player. For the
    // last field, 0 is away (the same direction the player faces), +90 left, -90 right.
    uint32 ghostSize = 100;
    int32 ghostPitch = 0;
    int32 ghostRoll = 0;
    size_t ghostCount = 0;
    int32 ghostFront = 0;
    {
        std::lock_guard<std::recursive_mutex> guard(_lock);
        auto itr = _carrying.find(player->GetGUID());
        if (itr != _carrying.end() && !itr->second.pieces.empty())
        {
            GhostPiece const& lead = itr->second.pieces.front();
            PieceDefinition const* piece = GetPiece(lead.itemEntry);
            float normal = piece && piece->scale > 0.0f ? piece->scale : 1.0f;
            ghostSize = uint32(std::lround(lead.scale / normal * 100.0f));
            ghostPitch = int32(std::lround(lead.pitch * 57.29578f));  // degrees
            ghostRoll = int32(std::lround(lead.roll * 57.29578f));
            ghostCount = itr->second.pieces.size();
            ghostFront = int32(std::lround(std::remainder(GhostFacing(itr->second) - player->GetOrientation(), 6.28318530717958647692f) * 57.29578f));
        }
    }
    message += Acore::StringFormat("\t{}\t{}\t{}\t{}\t{}", ghostSize, ghostPitch, ghostRoll, ghostCount, ghostFront);
    SendAddon(player, message);
}

bool PlayerHousingMgr::TourDone(ObjectGuid::LowType ownerGuid) const
{
    // The tour's last quest, handed in by any character of the account the island is kept for.
    return bool(CharacterDatabase.Query(
        "SELECT 1 FROM character_queststatus_rewarded r JOIN characters c ON c.guid = r.guid "
        "JOIN characters h ON h.account = c.account WHERE h.guid={} AND r.quest={} LIMIT 1", ownerGuid, QUEST_TOUR_OPEN));
}

bool PlayerHousingMgr::CallSteward(Player* player, std::string& reason)
{
    std::lock_guard<std::recursive_mutex> guard(_lock);
    Session* session = GetOwnerSession(player, reason);
    if (!session)
        return false;
    Map* map = player->GetMap();

    // A couple of yards ahead of the player, facing them.
    float o = player->GetOrientation();
    float x = player->GetPositionX() + std::cos(o) * 2.5f;
    float y = player->GetPositionY() + std::sin(o) * 2.5f;
    float z = player->GetPositionZ();
    float facing = Position::NormalizeOrientation(o + 3.14159265f);
    // He appears there (one already out goes: a creature moved by teleport doesn't reach every
    // client reliably).
    if (Creature* steward = session->stewardGuid ? map->GetCreature(session->stewardGuid) : nullptr)
        steward->DespawnOrUnsummon();
    session->stewardGuid.Clear();
    Position position;
    position.Relocate(x, y, z + 0.1f, facing);
    SpawnStewardAt(*session, map, position);
    reason = "Krook hurries over.";
    return true;
}
