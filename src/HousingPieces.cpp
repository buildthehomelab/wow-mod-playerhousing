#include "PlayerHousingMgr.h"

#include "Bag.h"
#include "Chat.h"
#include "Creature.h"
#include "DatabaseEnv.h"
#include "GameObject.h"
#include "GameTime.h"
#include "Item.h"
#include "Log.h"
#include "Map.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "StringFormat.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <functional>

using namespace Housing;

namespace
{
    constexpr float PI_F = 3.14159265358979323846f;
    constexpr float TWO_PI_F = 6.28318530717958647692f;
    constexpr size_t JOURNAL_SIZE = 30;
    constexpr size_t BIG_STEP = 20;  // pieces; undoing or redoing more waits between goes
    constexpr uint64 MERGE_WINDOW_MS = 4000;  // edit mode: moves this close together are one step
    constexpr float NO_HEIGHT = -50000.0f;

    float NormalizeAngle(float angle)
    {
        angle = std::fmod(angle, TWO_PI_F);
        return angle < 0.0f ? angle + TWO_PI_F : angle;
    }

    float GroundZ(Map* map, uint32 phaseMask, float x, float y, float zHint)
    {
        float height = map->GetHeight(phaseMask, x, y, zHint + 5.0f, true, 50.0f);
        if (height <= NO_HEIGHT)
            height = map->GetHeight(phaseMask, x, y, zHint + 50.0f, true, 200.0f);
        return height > NO_HEIGHT ? height : zHint;
    }
}

PieceDefinition const* PlayerHousingMgr::GetPiece(uint32 itemEntry) const
{
    std::lock_guard<std::recursive_mutex> guard(_lock);
    auto itr = _pieces.find(itemEntry);
    return itr != _pieces.end() ? &itr->second : nullptr;
}

std::string PlayerHousingMgr::PieceName(uint32 itemEntry) const
{
    PieceDefinition const* piece = GetPiece(itemEntry);
    return piece ? piece->name : "furniture";
}

PlayerHousingMgr::Session const* PlayerHousingMgr::FindSessionOf(Player const* player) const
{
    auto playerItr = _playerOwnerByGuid.find(player->GetGUID());
    if (playerItr == _playerOwnerByGuid.end())
        return nullptr;

    auto sessionItr = _sessionsByOwner.find(playerItr->second);
    return sessionItr != _sessionsByOwner.end() ? &sessionItr->second : nullptr;
}

PlayerHousingMgr::Session* PlayerHousingMgr::GetOwnerSession(Player* player, std::string& reason, bool ownerOnly)
{
    if (!_enabled || !player)
    {
        reason = "Housing is disabled.";
        return nullptr;
    }

    ObjectGuid::LowType owner = GetIslandOwner(player);
    if (owner == 0)
    {
        reason = "Go home first: the housing window's Go home.";
        return nullptr;
    }

    if (owner != HomeOf(player))
    {
        auto islandItr = _sessionsByOwner.find(owner);
        bool roommate = islandItr != _sessionsByOwner.end() && islandItr->second.roommates.count(player->GetGUID().GetCounter());
        if (!roommate || ownerOnly)
        {
            reason = roommate ? "Only the island's owner can do that." : "Only the owner can change things on this island.";
            return nullptr;
        }
    }

    auto sessionItr = _sessionsByOwner.find(owner);
    if (sessionItr == _sessionsByOwner.end())
    {
        reason = "Your island is still loading. Try again in a moment.";
        return nullptr;
    }

    if (!sessionItr->second.initialized)
    {
        std::string initReason;
        if (!InitializeSession(owner, initReason))
        {
            reason = initReason;
            return nullptr;
        }
    }

    return &sessionItr->second;
}

void PlayerHousingMgr::CountPlaced(ObjectGuid::LowType ownerGuid, uint32& furnishings, uint32& buildings) const
{
    furnishings = 0;
    buildings = 0;

    std::lock_guard<std::recursive_mutex> guard(_lock);
    auto sessionItr = _sessionsByOwner.find(ownerGuid);
    if (sessionItr != _sessionsByOwner.end() && sessionItr->second.initialized)
    {
        for (auto const& [id, placement] : sessionItr->second.placements)
        {
            auto pieceItr = _pieces.find(placement.itemEntry);
            if (pieceItr != _pieces.end() && pieceItr->second.IsBuilding())
                ++buildings;
            else
                ++furnishings;
        }
        return;
    }

    if (QueryResult result = CharacterDatabase.Query(
            "SELECT source_item_entry FROM mod_playerhousing_placement WHERE owner_guid={} AND map_id={}", ownerGuid, _layout.mapId))
    {
        do
        {
            auto pieceItr = _pieces.find((*result)[0].Get<uint32>());
            if (pieceItr == _pieces.end())
                continue;
            if (pieceItr->second.IsBuilding())
                ++buildings;
            else
                ++furnishings;
        } while (result->NextRow());
    }
}

std::string PlayerHousingMgr::CountsText(ObjectGuid::LowType ownerGuid) const
{
    uint32 furnishings = 0;
    uint32 buildings = 0;
    CountPlaced(ownerGuid, furnishings, buildings);
    return Acore::StringFormat("{}/{} furnishings, {}/{} buildings", furnishings, _maxFurnishings, buildings, _maxBuildings);
}

bool PlayerHousingMgr::CheckLimit(Session const& session, PieceDefinition const& piece, std::string& reason) const
{
    uint32 furnishings = 0;
    uint32 buildings = 0;
    for (auto const& [id, placement] : session.placements)
    {
        auto pieceItr = _pieces.find(placement.itemEntry);
        if (pieceItr != _pieces.end() && pieceItr->second.IsBuilding())
            ++buildings;
        else
            ++furnishings;
    }

    if (piece.IsBuilding() && buildings >= _maxBuildings)
    {
        reason = Acore::StringFormat("Your island has {}/{} buildings. Pick one up first.", buildings, _maxBuildings);
        return false;
    }

    if (!piece.IsBuilding() && furnishings >= _maxFurnishings)
    {
        reason = Acore::StringFormat("Your island is full ({}/{} furnishings). Pick something up first.", furnishings, _maxFurnishings);
        return false;
    }

    return true;
}

bool PlayerHousingMgr::IsSpotOnIsland(float x, float y, float z) const
{
    if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z))
        return false;

    float bottom = _layout.landing.GetPositionZ() - 40.0f;
    float top = _layout.landing.GetPositionZ() + 250.0f;
    return IsOnIslandGround(x, y) && z > bottom && z < top;
}

void PlayerHousingMgr::AddToStorage(ObjectGuid::LowType ownerGuid, uint32 itemEntry, int32 delta) const
{
    if (delta > 0)
    {
        CharacterDatabase.DirectExecute(
            "INSERT INTO mod_playerhousing_storage (owner_guid, item_entry, count) VALUES ({}, {}, {}) "
            "ON DUPLICATE KEY UPDATE count = count + {}", ownerGuid, itemEntry, delta, delta);
        return;
    }

    CharacterDatabase.DirectExecute(
        "UPDATE mod_playerhousing_storage SET count = GREATEST(0, CAST(count AS SIGNED) + {}) WHERE owner_guid={} AND item_entry={}",
        delta, ownerGuid, itemEntry);
    CharacterDatabase.DirectExecute("DELETE FROM mod_playerhousing_storage WHERE owner_guid={} AND count=0", ownerGuid);
}

std::map<uint32, uint32> PlayerHousingMgr::GetStorage(ObjectGuid::LowType ownerGuid) const
{
    std::map<uint32, uint32> storage;
    if (QueryResult result = CharacterDatabase.Query(
            "SELECT item_entry, count FROM mod_playerhousing_storage WHERE owner_guid={} AND count > 0", ownerGuid))
    {
        do
        {
            storage[(*result)[0].Get<uint32>()] = (*result)[1].Get<uint32>();
        } while (result->NextRow());
    }
    return storage;
}

void PlayerHousingMgr::ReturnPiece(Player* player, uint32 itemEntry)
{
    AddToStorage(HomeOf(player), itemEntry, 1);
}

bool PlayerHousingMgr::TakePieceFor(Player* player, ObjectGuid::LowType itemOwner, uint32 itemEntry)
{
    if (itemOwner == HomeOf(player))
        return TakePiece(player, itemEntry);

    std::map<uint32, uint32> storage = GetStorage(itemOwner);
    auto itr = storage.find(itemEntry);
    if (itr == storage.end() || itr->second == 0)
        return false;
    AddToStorage(itemOwner, itemEntry, -1);
    return true;
}

bool PlayerHousingMgr::TakePiece(Player* player, uint32 itemEntry)
{
    ObjectGuid::LowType home = HomeOf(player);
    std::map<uint32, uint32> storage = GetStorage(home);
    auto itr = storage.find(itemEntry);
    if (itr == storage.end() || itr->second == 0)
        return false;

    AddToStorage(home, itemEntry, -1);
    return true;
}

bool PlayerHousingMgr::SweepHousingItems(Player* player, bool bankToo)
{
    if (!_enabled || !player || player->GetSession()->IsHeadless())
        return false;

    // Where each one is first, then they go: the positions don't move as items are destroyed.
    std::vector<std::pair<uint8, uint8>> found;
    auto check = [&](Item const* item, uint8 bag, uint8 slot)
    {
        if (!item || item->GetEntry() == HOUSE_KEY_ITEM)
            return;
        uint32 entry = item->GetEntry();
        if (IsMoverItem(entry) || GetPiece(entry))
            found.emplace_back(bag, slot);
    };
    auto checkBag = [&](uint8 bagSlot)
    {
        if (Bag* bag = player->GetBagByPos(bagSlot))
            for (uint32 slot = 0; slot < bag->GetBagSize(); ++slot)
                check(bag->GetItemByPos(uint8(slot)), bagSlot, uint8(slot));
    };

    for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
        check(player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot), INVENTORY_SLOT_BAG_0, slot);
    for (uint8 slot = KEYRING_SLOT_START; slot < CURRENCYTOKEN_SLOT_END; ++slot)
        check(player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot), INVENTORY_SLOT_BAG_0, slot);
    for (uint8 bag = INVENTORY_SLOT_BAG_START; bag < INVENTORY_SLOT_BAG_END; ++bag)
        checkBag(bag);
    if (bankToo)
    {
        for (uint8 slot = BANK_SLOT_ITEM_START; slot < BANK_SLOT_ITEM_END; ++slot)
            check(player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot), INVENTORY_SLOT_BAG_0, slot);
        for (uint8 bag = BANK_SLOT_BAG_START; bag < BANK_SLOT_BAG_END; ++bag)
            checkBag(bag);
    }
    if (found.empty())
        return false;

    std::map<uint32, uint32> pieces;
    for (auto const& [bag, slot] : found)
    {
        Item* item = player->GetItemByPos(bag, slot);
        if (!item)
            continue;
        if (!IsMoverItem(item->GetEntry()))
            pieces[item->GetEntry()] += item->GetCount();
        player->DestroyItem(bag, slot, true);
    }

    ObjectGuid::LowType home = HomeOf(player);
    std::string names;
    uint32 total = 0;
    for (auto const& [entry, count] : pieces)
    {
        AddToStorage(home, entry, int32(count));
        total += count;
        if (total <= 6)
            names += (names.empty() ? "" : ", ") + (count > 1 ? Acore::StringFormat("{} x{}", PieceName(entry), count) : PieceName(entry));
    }
    if (total)
        Say(player, Acore::StringFormat("Housing pieces don't take bag space any more: {}{} went into your Collection (the housing window, /housing).",
            names, total > 6 ? " and more" : ""));
    return true;
}

void PlayerHousingMgr::SavePlacement(ObjectGuid::LowType ownerGuid, Placement const& placement, uint32 mapId) const
{
    // The gear on a stand is saved as it moves (see HousingStands.cpp), never from here.
    CharacterDatabase.DirectExecute(
        "REPLACE INTO mod_playerhousing_placement "
        "(owner_guid, placement_id, source_item_entry, map_id, scale, pos_x, pos_y, pos_z, orientation, look, parent_id, pitch, roll, placed_by) "
        "VALUES ({}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {})",
        ownerGuid, placement.id, placement.itemEntry, mapId, placement.scale, placement.x, placement.y, placement.z, placement.o, placement.look,
        placement.parent, placement.pitch, placement.roll, placement.placedBy);
}

void PlayerHousingMgr::DeletePlacement(ObjectGuid::LowType ownerGuid, uint32 placementId) const
{
    CharacterDatabase.DirectExecute("DELETE FROM mod_playerhousing_placement WHERE owner_guid={} AND placement_id={}", ownerGuid, placementId);
}

bool PlayerHousingMgr::SpawnPlacement(Session& session, Map* map, Placement const& placement)
{
    auto pieceItr = _pieces.find(placement.itemEntry);
    if (!map || pieceItr == _pieces.end())
        return false;

    PieceDefinition const& piece = pieceItr->second;
    if (piece.HasFlag(PIECE_FLAG_STAND))
        return SpawnStand(session, map, placement);
    if (piece.HasFlag(PIECE_FLAG_FIGURE))
        return SpawnFigure(session, map, piece, placement);

    bool editCopy = session.decorating && piece.editGoEntry != 0;
    uint32 entry = editCopy ? piece.editGoEntry : piece.goEntry;
    GameObjectTemplate const* goInfo = sObjectMgr->GetGameObjectTemplate(entry);
    if (!goInfo)
        return false;

    GameObject* object = new GameObject();
    if (!object->Create(map->GenerateLowGuid<HighGuid::GameObject>(), entry, map, PHASEMASK_NORMAL,
            placement.x, placement.y, placement.z, placement.o, G3D::Quat(0.0f, 0.0f, 0.0f, 0.0f), 100, GO_STATE_READY))
    {
        delete object;
        return false;
    }

    object->SetRespawnTime(0);
    object->SetSpawnedByDefault(false);
    object->SetObjectScale(goInfo->size * (placement.scale / piece.scale));
    if (placement.pitch != 0.0f || placement.roll != 0.0f)
        SetRotationAngles(object, placement.o, placement.pitch, placement.roll);
    // Buildings are seen from across the island instead of popping in at the normal range.
    if (piece.IsBuilding())
        object->SetVisibilityDistanceOverride(VisibilityDistanceType::Large);

    if (!map->AddToMap(object))
    {
        delete object;
        return false;
    }

    object->SetPhaseMask(session.phaseMask, true);
    // Server-side collision compares phases bit by bit, so every island would collide with
    // every other island's pieces; clients collide with what they see anyway.
    object->EnableCollision(false);

    SpawnedPiece& spawned = session.spawned[placement.id];
    spawned.guid = object->GetGUID();
    spawned.editCopy = editCopy;
    return true;
}

void PlayerHousingMgr::DespawnPlacement(Session& session, Map* map, uint32 placementId)
{
    auto itr = session.spawned.find(placementId);
    if (itr != session.spawned.end())
    {
        RemoveSpawned(map, itr->second.guid);
        session.spawned.erase(itr);
    }
    // Moved or put away: its ring doesn't stay behind.
    RemoveHighlightsOf(session, map, placementId);
}

void PlayerHousingMgr::RespawnPlacement(Session& session, Map* map, uint32 placementId)
{
    DespawnPlacement(session, map, placementId);
    auto itr = session.placements.find(placementId);
    if (itr == session.placements.end())
        return;

    SpawnPlacement(session, map, itr->second);
}

void PlayerHousingMgr::SpawnSteward(Session& session, Map* map)
{
    Position const& landing = _layout.landing;
    float forwardX = std::cos(landing.GetOrientation());
    float forwardY = std::sin(landing.GetOrientation());
    float leftX = -forwardY;
    float leftY = forwardX;

    float x = landing.GetPositionX() + forwardX * _layout.stewardOffsetX + leftX * _layout.stewardOffsetY;
    float y = landing.GetPositionY() + forwardY * _layout.stewardOffsetX + leftY * _layout.stewardOffsetY;
    float z = GroundZ(map, PHASEMASK_NORMAL, x, y, landing.GetPositionZ());
    float o = NormalizeAngle(std::atan2(landing.GetPositionY() - y, landing.GetPositionX() - x));

    Position position;
    position.Relocate(x, y, z + 0.1f, o);
    SpawnStewardAt(session, map, position);
}

void PlayerHousingMgr::SpawnStewardAt(Session& session, Map* map, Position const& position)
{
    if (TempSummon* steward = map->SummonCreature(_stewardEntry, position))
    {
        steward->SetPhaseMask(session.phaseMask, true);
        steward->SetDisplayId(_stewardDisplayId);
        steward->SetNativeDisplayId(_stewardDisplayId);
        session.stewardGuid = steward->GetGUID();
    }
    else
        LOG_WARN("module", "mod-playerhousing: Failed to summon steward entry {} for owner {}.", _stewardEntry, session.ownerGuid);
}

void PlayerHousingMgr::PlaceStarterWreckage(Session& session, Map* map)
{
    // The fallen cart and the shredded tent stand a few yards up the island from the landing
    // spot, one to each side, facing the new arrival.
    Position const& landing = _layout.landing;
    float offsets[][2] = { { 9.0f, 5.0f }, { 9.0f, -6.0f }, { 14.0f, 0.0f }, { 5.0f, 10.0f } };
    uint32 index = 0;
    for (auto const& [itemEntry, piece] : _pieces)
    {
        if (!piece.HasFlag(PIECE_FLAG_WRECKAGE) || index >= std::size(offsets))
            continue;

        float forward = offsets[index][0];
        float left = offsets[index][1];
        ++index;

        float o = landing.GetOrientation();
        Placement placement;
        placement.id = session.nextPlacementId++;
        placement.itemEntry = itemEntry;
        placement.x = landing.GetPositionX() + std::cos(o) * forward - std::sin(o) * left;
        placement.y = landing.GetPositionY() + std::sin(o) * forward + std::cos(o) * left;
        placement.z = GroundZ(map, PHASEMASK_NORMAL, placement.x, placement.y, landing.GetPositionZ());
        placement.o = NormalizeAngle(std::atan2(landing.GetPositionY() - placement.y, landing.GetPositionX() - placement.x));
        placement.scale = piece.scale;

        session.placements[placement.id] = placement;
        SavePlacement(session.ownerGuid, placement, session.mapId);
        SpawnPlacement(session, map, placement);
    }
}

bool PlayerHousingMgr::AddNewPlacement(Player* player, Session& session, Placement const& placement, std::string& reason)
{
    PieceDefinition const* piece = GetPiece(placement.itemEntry);
    Map* map = player->GetMap();
    if (!piece || !SpawnPlacement(session, map, placement))
    {
        reason = "Couldn't place that here.";
        return false;
    }

    session.placements[placement.id] = placement;
    SelectOne(session, player->GetGUID().GetCounter(), placement.id);
    SavePlacement(session.ownerGuid, placement, session.mapId);

    Record(player, "placed " + piece->name, { Change{ placement.id, std::nullopt, placement } });
    // It's there to see: said only when the island is nearly full.
    reason.clear();
    uint32 furnishings = 0;
    uint32 buildings = 0;
    CountPlaced(session.ownerGuid, furnishings, buildings);
    if (piece->IsBuilding() ? buildings + 2 >= _maxBuildings : furnishings + 10 >= _maxFurnishings)
        reason = Acore::StringFormat("Placed {} ({}).", piece->name, CountsText(session.ownerGuid));
    QuestEvent(player, QUEST_TOUR_PLACE);
    Tip(player, TIP_FIRST_PLACE, "Right-click a piece to pick it up again; the mouse wheel turns it. Mistake? Undo, in the housing window.");
    return true;
}

bool PlayerHousingMgr::ApplyState(Player* player, Session& session, Map* map, uint32 placementId, std::optional<Placement> const& target, std::string& reason)
{
    auto currentItr = session.placements.find(placementId);
    bool exists = currentItr != session.placements.end();

    if (exists && !target)
    {
        Placement current = currentItr->second;
        ObjectGuid::LowType itemOwner = ItemOwnerOf(session, current);
        for (auto const& [slot, gear] : current.gear)
            ReturnGear(player, session.ownerGuid, itemOwner, placementId, slot, gear);
        DespawnPlacement(session, map, placementId);
        session.placements.erase(currentItr);
        DeletePlacement(session.ownerGuid, placementId);
        for (auto& [who, selected] : session.selected)
            if (selected == placementId)
                selected = 0;
        for (auto& [who, group] : session.groups)
            group.erase(std::remove(group.begin(), group.end(), placementId), group.end());

        // A piece goes back to the Collection of whoever placed it: the owner, or a roommate.
        if (itemOwner != HomeOf(player))
        {
            AddToStorage(itemOwner, current.itemEntry, 1);
            ++_report.toOthers;
            return true;
        }

        ReturnPiece(player, current.itemEntry);
        ++_report.toCollection;
        return true;
    }

    if (!exists && target)
    {
        auto pieceItr = _pieces.find(target->itemEntry);
        if (pieceItr == _pieces.end())
        {
            reason = "That piece no longer exists.";
            return false;
        }

        if (!CheckLimit(session, pieceItr->second, reason))
            return false;

        if (!TakePieceFor(player, ItemOwnerOf(session, *target), target->itemEntry))
        {
            reason = ItemOwnerOf(session, *target) == HomeOf(player)
                ? Acore::StringFormat("You no longer have a {} in your Collection.", pieceItr->second.name)
                : Acore::StringFormat("{} no longer has the {} in their Collection.", NameOf(ItemOwnerOf(session, *target)), pieceItr->second.name);
            return false;
        }

        // A stand comes back with the gear it had, as far as that gear is still in the bags.
        Placement placed = *target;
        placed.gear.clear();
        SavePlacement(session.ownerGuid, placed, session.mapId);
        for (auto const& [slot, gear] : target->gear)
        {
            std::string ignored;
            if (MoveGearToStand(player, session.ownerGuid, placementId, slot, gear.itemGuid, ignored))
                placed.gear[slot] = gear;
            else
                _report.gearMissing.push_back(StandItemName(gear.itemEntry));
        }

        session.placements[placementId] = placed;
        session.nextPlacementId = std::max(session.nextPlacementId, placementId + 1);
        SpawnPlacement(session, map, placed);
        session.selected[player->GetGUID().GetCounter()] = placementId;
        ++_report.placed;
        return true;
    }

    if (exists && target)
    {
        Placement current = currentItr->second;
        Placement applied = *target;
        auto sameItem = [](std::map<uint8, GearItem> const& gear, uint8 slot, uint32 itemGuid)
        {
            auto itr = gear.find(slot);
            return itr != gear.end() && itr->second.itemGuid == itemGuid;
        };

        // Gear on a stand is only ever changed by whoever it belongs to; anyone else's undo
        // moves the stand and leaves what it wears alone.
        if (ItemOwnerOf(session, current) != HomeOf(player))
            applied.gear = current.gear;

        // Gear comes off first, so a swap frees the slot, then the new gear goes on.
        for (auto const& [slot, gear] : current.gear)
            if (!sameItem(applied.gear, slot, gear.itemGuid))
                ReturnGear(player, session.ownerGuid, ItemOwnerOf(session, current), placementId, slot, gear);
        for (auto const& [slot, gear] : std::map<uint8, GearItem>(applied.gear))
        {
            if (sameItem(current.gear, slot, gear.itemGuid))
                continue;
            std::string ignored;
            if (!MoveGearToStand(player, session.ownerGuid, placementId, slot, gear.itemGuid, ignored))
            {
                applied.gear.erase(slot);
                _report.gearMissing.push_back(StandItemName(gear.itemEntry));
            }
        }

        currentItr->second = applied;
        SavePlacement(session.ownerGuid, applied, session.mapId);
        RespawnPlacement(session, map, placementId);
        session.selected[player->GetGUID().GetCounter()] = placementId;
        return true;
    }

    return true;
}

bool PlayerHousingMgr::ApplyChanges(Player* player, Session& session, std::vector<Change> const& changes, bool towardsAfter, std::string& reason)
{
    Map* map = player->GetMap();
    bool allOk = true;
    auto apply = [&](Change const& change)
    {
        std::string failure;
        if (!ApplyState(player, session, map, change.placementId, towardsAfter ? change.after : change.before, failure))
        {
            allOk = false;
            reason = failure;
        }
    };

    // Undo walks the changes backwards, so a building comes back before what stood in it.
    if (towardsAfter)
        for (Change const& change : changes)
            apply(change);
    else
        for (auto itr = changes.rbegin(); itr != changes.rend(); ++itr)
            apply(*itr);
    return allOk;
}

void PlayerHousingMgr::Record(Player* player, std::string const& label, std::vector<Change> changes, bool merge)
{
    Journal& journal = _journals[player->GetGUID().GetCounter()];
    ObjectGuid::LowType island = GetIslandOwner(player);
    if (journal.island != island)
    {
        journal = Journal{};
        journal.island = island;
    }

    uint64 now = GameTime::GetGameTimeMS().count();
    if (merge && !journal.undo.empty())
    {
        // Another small move of the same pieces soon after the last: that step grows instead,
        // so undo takes back the whole run of key presses.
        JournalEntry& last = journal.undo.back();
        bool same = last.mergeable && now - last.at <= MERGE_WINDOW_MS && last.changes.size() == changes.size();
        for (size_t i = 0; same && i < changes.size(); ++i)
            same = last.changes[i].placementId == changes[i].placementId && last.changes[i].after && changes[i].before && changes[i].after;
        if (same)
        {
            for (size_t i = 0; i < changes.size(); ++i)
                last.changes[i].after = changes[i].after;
            last.label = label;
            last.at = now;
            journal.redo.clear();
            return;
        }
    }

    journal.undo.push_back(JournalEntry{ label, std::move(changes), now, merge });
    while (journal.undo.size() > JOURNAL_SIZE)
        journal.undo.pop_front();
    journal.redo.clear();
}

bool PlayerHousingMgr::JournalStillApplies(Session const& session, JournalEntry const& entry, bool towardsAfter) const
{
    // Undo expects each piece as the step left it, redo as it was before. A piece that's gone
    // is fine (it comes back, paid for as ever); a different piece, or one where there was
    // none, means the island changed under this step, and it no longer applies.
    for (Change const& change : entry.changes)
    {
        std::optional<Placement> const& expected = towardsAfter ? change.before : change.after;
        auto itr = session.placements.find(change.placementId);
        if (itr == session.placements.end())
            continue;
        if (!expected || itr->second.itemEntry != expected->itemEntry || itr->second.placedBy != expected->placedBy)
            return false;
    }
    return true;
}

void PlayerHousingMgr::ForgetJournals(ObjectGuid::LowType ownerGuid)
{
    std::lock_guard<std::recursive_mutex> guard(_lock);
    for (auto itr = _journals.begin(); itr != _journals.end();)
    {
        if (itr->second.island == ownerGuid)
            itr = _journals.erase(itr);
        else
            ++itr;
    }
}

std::string PlayerHousingMgr::UndoLabel(Player const* player) const
{
    std::lock_guard<std::recursive_mutex> guard(_lock);
    auto itr = _journals.find(player->GetGUID().GetCounter());
    return itr != _journals.end() && !itr->second.undo.empty() ? itr->second.undo.back().label : "";
}

std::string PlayerHousingMgr::RedoLabel(Player const* player) const
{
    std::lock_guard<std::recursive_mutex> guard(_lock);
    auto itr = _journals.find(player->GetGUID().GetCounter());
    return itr != _journals.end() && !itr->second.redo.empty() ? itr->second.redo.back().label : "";
}

std::string PlayerHousingMgr::DescribeReturns(bool collectionToo) const
{
    std::string gear;
    // Gear (real items, unlike the pieces) taken off a stand is the subject of the sentence
    // before; gear coming back with a picked-up stand is extra news.
    bool piecesReturned = _report.toCollection || _report.toOthers;
    if (_report.gearToBags && !piecesReturned)
        gear += _report.gearToBags == 1 ? " It's back in your bags." : " They're back in your bags.";
    else if (_report.gearToBags)
        gear += _report.gearToBags == 1 ? " Its gear went back to your bags." : Acore::StringFormat(" {} pieces of gear went back to your bags.", _report.gearToBags);
    if (_report.gearMailed)
        gear += Acore::StringFormat(" Your bags were full, so Krook mailed you {} (check your mailbox).",
            _report.gearMailed == 1 ? "a piece of gear" : Acore::StringFormat("{} pieces of gear", _report.gearMailed));
    if (!_report.gearMissing.empty())
    {
        std::string names;
        for (std::string const& name : _report.gearMissing)
            names += (names.empty() ? "" : ", ") + name;
        gear += " Not in your bags any more, so not put back: " + names + ".";
    }
    return DescribeItemReturns(collectionToo) + gear;
}

std::string PlayerHousingMgr::DescribeItemReturns(bool collectionToo) const
{
    std::string mine;
    if (_report.toCollection && collectionToo)
        mine = _report.toCollection == 1 ? " It's back in your Collection." : " They're back in your Collection.";

    // Pieces someone else placed went back to them.
    if (_report.toOthers == 1)
        mine += _report.toCollection ? " One piece went back to the Collection of whoever placed it."
                                     : " It went back to the Collection of whoever placed it.";
    else if (_report.toOthers > 1)
        mine += Acore::StringFormat(" {} pieces went back to the Collections of whoever placed them.", _report.toOthers);
    return mine;
}

bool PlayerHousingMgr::Undo(Player* player, std::string& reason, bool cooldown)
{
    std::lock_guard<std::recursive_mutex> guard(_lock);
    Session* session = GetOwnerSession(player, reason);
    if (!session)
        return false;

    Journal& journal = _journals[player->GetGUID().GetCounter()];
    if (journal.island != session->ownerGuid)
        journal = Journal{};
    if (journal.undo.empty())
    {
        reason = "Nothing to undo.";
        return false;
    }

    // A step with many pieces is real work for the server: not over and over.
    if (cooldown && journal.undo.back().changes.size() > BIG_STEP && OnCooldown(player, COOLDOWN_UNDO, 3 * IN_MILLISECONDS, reason))
        return false;

    JournalEntry entry = journal.undo.back();
    journal.undo.pop_back();
    if (!JournalStillApplies(*session, entry, false))
    {
        // Dropped: older steps touch pieces of their own and are checked when their turn comes.
        reason = Acore::StringFormat("Can't undo \"{}\": those pieces have changed since (someone else moved, picked up or replaced them).",
            entry.label);
        SendAddonState(player);
        return false;
    }

    ObjectGuid::LowType self = player->GetGUID().GetCounter();
    uint32 selectedBefore = session->selected.count(self) ? session->selected[self] : 0;
    std::vector<uint32> groupBefore = session->groups.count(self) ? session->groups[self] : std::vector<uint32>{};
    _report = {};
    std::string failure;
    bool ok = ApplyChanges(player, *session, entry.changes, false, failure);
    KeepGroupAfterStep(*session, self, selectedBefore, groupBefore, entry.changes);
    journal.redo.push_back(entry);

    reason = Acore::StringFormat("Undid: {}.{}", entry.label, DescribeReturns(false));
    QuestEvent(player, QUEST_TOUR_UNDO);
    if (!ok)
        reason += " Not everything could be undone: " + failure;
    SendAddonState(player);
    return ok;
}

bool PlayerHousingMgr::Redo(Player* player, std::string& reason)
{
    std::lock_guard<std::recursive_mutex> guard(_lock);
    Session* session = GetOwnerSession(player, reason);
    if (!session)
        return false;

    Journal& journal = _journals[player->GetGUID().GetCounter()];
    if (journal.island != session->ownerGuid)
        journal = Journal{};
    if (journal.redo.empty())
    {
        reason = "Nothing to redo.";
        return false;
    }

    // A step with many pieces is real work for the server: not over and over.
    if (journal.redo.back().changes.size() > BIG_STEP && OnCooldown(player, COOLDOWN_UNDO, 3 * IN_MILLISECONDS, reason))
        return false;

    JournalEntry entry = journal.redo.back();
    journal.redo.pop_back();
    if (!JournalStillApplies(*session, entry, true))
    {
        // Dropped: older steps touch pieces of their own and are checked when their turn comes.
        reason = Acore::StringFormat("Can't redo \"{}\": those pieces have changed since (someone else moved, picked up or replaced them).",
            entry.label);
        SendAddonState(player);
        return false;
    }

    ObjectGuid::LowType self = player->GetGUID().GetCounter();
    uint32 selectedBefore = session->selected.count(self) ? session->selected[self] : 0;
    std::vector<uint32> groupBefore = session->groups.count(self) ? session->groups[self] : std::vector<uint32>{};
    _report = {};
    std::string failure;
    bool ok = ApplyChanges(player, *session, entry.changes, true, failure);
    KeepGroupAfterStep(*session, self, selectedBefore, groupBefore, entry.changes);
    journal.undo.push_back(entry);

    reason = Acore::StringFormat("Redid: {}.{}", entry.label, DescribeReturns(false));
    if (!ok)
        reason += " Not everything could be redone: " + failure;
    SendAddonState(player);
    return ok;
}

uint32 PlayerHousingMgr::GetSelectedPlacement(Player const* player) const
{
    std::lock_guard<std::recursive_mutex> guard(_lock);
    Session const* session = FindSessionOf(player);
    if (!session)
        return 0;
    auto itr = session->selected.find(player->GetGUID().GetCounter());
    return itr != session->selected.end() ? itr->second : 0;
}

uint32 PlayerHousingMgr::ResolvePlacementArgument(Player* player, uint32 placementId) const
{
    if (placementId)
        return placementId;

    if (uint32 selected = GetSelectedPlacement(player))
        return selected;

    // Nothing selected yet: the nearest piece within reach.
    std::vector<std::pair<Placement, float>> nearby = GetNearbyPlacements(player, 10.0f);
    return nearby.empty() ? 0 : nearby.front().first.id;
}

std::optional<Placement> PlayerHousingMgr::GetPlacement(Player const* player, uint32 placementId) const
{
    std::lock_guard<std::recursive_mutex> guard(_lock);
    Session const* session = FindSessionOf(player);
    if (!session)
        return std::nullopt;

    auto itr = session->placements.find(placementId);
    if (itr == session->placements.end())
        return std::nullopt;
    return itr->second;
}

uint32 PlayerHousingMgr::GetPlacementForObject(Player const* player, ObjectGuid const& guid) const
{
    std::lock_guard<std::recursive_mutex> guard(_lock);
    Session const* session = FindSessionOf(player);
    if (!session)
        return 0;

    for (auto const& [id, spawned] : session->spawned)
        if (spawned.guid == guid)
            return id;
    return 0;
}

std::vector<std::pair<Placement, float>> PlayerHousingMgr::GetNearbyPlacements(Player const* player, float range) const
{
    std::vector<std::pair<Placement, float>> nearby;
    std::lock_guard<std::recursive_mutex> guard(_lock);
    Session const* session = FindSessionOf(player);
    if (!session)
        return nearby;

    for (auto const& [id, placement] : session->placements)
    {
        float distance = player->GetExactDist(placement.x, placement.y, placement.z);
        auto pieceItr = _pieces.find(placement.itemEntry);
        // Buildings count from their edge, so standing inside one finds it.
        if (pieceItr != _pieces.end() && pieceItr->second.IsBuilding())
            distance = std::max(0.0f, player->GetExactDist2d(placement.x, placement.y) - pieceItr->second.footprint);
        if (distance <= range)
            nearby.emplace_back(placement, distance);
    }

    std::sort(nearby.begin(), nearby.end(), [](auto const& left, auto const& right) { return left.second < right.second; });
    return nearby;
}

std::vector<Placement> PlayerHousingMgr::GetPiecesInside(ObjectGuid::LowType ownerGuid, uint32 buildingPlacementId) const
{
    std::vector<Placement> inside;
    std::lock_guard<std::recursive_mutex> guard(_lock);
    auto sessionItr = _sessionsByOwner.find(ownerGuid);
    if (sessionItr == _sessionsByOwner.end())
        return inside;

    Session const& session = sessionItr->second;
    auto buildingItr = session.placements.find(buildingPlacementId);
    if (buildingItr == session.placements.end())
        return inside;

    auto buildingPieceItr = _pieces.find(buildingItr->second.itemEntry);
    if (buildingPieceItr == _pieces.end() || !buildingPieceItr->second.IsBuilding())
        return inside;

    Placement const& building = buildingItr->second;
    PieceDefinition const& buildingPiece = buildingPieceItr->second;
    float radius = buildingPiece.footprint;
    float top = building.z + std::max(4.0f, buildingPiece.height);
    // Into the building's own frame (x forward), where its outline is a plain rectangle.
    float cosO = std::cos(building.o);
    float sinO = std::sin(building.o);
    float scale = buildingPiece.scale > 0.0f ? building.scale / buildingPiece.scale : 1.0f;
    for (auto const& [id, placement] : session.placements)
    {
        if (id == buildingPlacementId)
            continue;

        auto pieceItr = _pieces.find(placement.itemEntry);
        if (pieceItr == _pieces.end() || pieceItr->second.IsBuilding())
            continue;

        float dx = placement.x - building.x;
        float dy = placement.y - building.y;
        bool within;
        if (buildingPiece.HasOutline())
        {
            float localX = (cosO * dx + sinO * dy) / scale;
            float localY = (-sinO * dx + cosO * dy) / scale;
            within = localX >= buildingPiece.outlineMinX && localX <= buildingPiece.outlineMaxX &&
                     localY >= buildingPiece.outlineMinY && localY <= buildingPiece.outlineMaxY;
        }
        else
            within = dx * dx + dy * dy <= radius * radius;
        if (within && placement.z >= building.z - 1.5f && placement.z <= top)
            inside.push_back(placement);
    }
    return inside;
}

bool PlayerHousingMgr::PickUp(Player* player, uint32 placementId, bool withInside, std::string& reason)
{
    std::lock_guard<std::recursive_mutex> guard(_lock);
    Session* session = GetOwnerSession(player, reason);
    if (!session)
        return false;

    // Several pieces selected: all of them.
    if (!placementId)
    {
        std::vector<uint32> group = GetGroup(player);
        if (group.size() > 1)
            return PickUpGroup(player, *session, group, reason);
    }

    placementId = ResolvePlacementArgument(player, placementId);
    auto itr = session->placements.find(placementId);
    if (itr == session->placements.end())
    {
        reason = "Nothing here to pick up. Stand next to a piece, or click it while decorating.";
        return false;
    }

    Placement placement = itr->second;
    PieceDefinition const* piece = GetPiece(placement.itemEntry);
    std::string name = piece ? piece->name : "furniture";

    // What stands on it comes along; a building brings what's inside only when asked.
    std::vector<Change> changes;
    changes.push_back(Change{ placementId, placement, std::nullopt });
    uint32 insideCount = 0;
    for (uint32 carriedId : CarriedBy(*session, placementId, withInside))
    {
        changes.push_back(Change{ carriedId, session->placements[carriedId], std::nullopt });
        ++insideCount;
    }

    _report = {};
    std::string failure;
    ApplyChanges(player, *session, changes, true, failure);

    std::string pieces = Acore::StringFormat("{} {}", insideCount, insideCount == 1 ? "piece" : "pieces");
    char const* where = piece && piece->IsBuilding() ? "inside" : "on it";
    std::string label = insideCount ? Acore::StringFormat("picked up {} and {} {}", name, pieces, where) : "picked up " + name;
    Record(player, label, std::move(changes));

    // Gone from where it stood, back in the Collection: said only when it went somewhere else.
    reason = DescribeReturns(false);
    if (!reason.empty())
        reason = Acore::StringFormat("Picked up {}{}.{}", name, insideCount ? Acore::StringFormat(" and the {} {}", pieces,
            piece && piece->IsBuilding() ? "inside it" : "on it") : std::string(), reason);
    SendAddonState(player);
    return true;
}

bool PlayerHousingMgr::Transform(Player* player, uint32 placementId, std::string const& label, float dx, float dy, float dz, float dO, bool absoluteO, float o,
    std::string& reason, bool merge)
{
    std::lock_guard<std::recursive_mutex> guard(_lock);
    Session* session = GetOwnerSession(player, reason);
    if (!session)
        return false;

    placementId = ResolvePlacementArgument(player, placementId);
    auto itr = session->placements.find(placementId);
    if (itr == session->placements.end())
    {
        reason = "Choose a piece first: click it while decorating, or stand next to it.";
        return false;
    }

    Placement before = itr->second;
    float turn = absoluteO ? o - before.o : dO;
    Placement after = before;
    after.x += dx;
    after.y += dy;
    after.z += dz;
    after.o = NormalizeAngle(before.o + turn);

    // What stands on it (and, for a building, what's inside) moves and turns with it, as if
    // it were one piece.
    std::vector<uint32> carriedIds = CarriedBy(*session, placementId, true);
    PieceDefinition const* movedPiece = GetPiece(before.itemEntry);
    if ((dx != 0.0f || dy != 0.0f || dz != 0.0f) && movedPiece && !movedPiece->IsBuilding())
    {
        // Moved onto a table, or off one.
        std::set<uint32> exclude(carriedIds.begin(), carriedIds.end());
        exclude.insert(placementId);
        after.parent = FindSurfaceUnder(*session, after.x, after.y, after.z, exclude);
    }

    std::vector<Change> changes{ Change{ placementId, before, after } };
    float cosTurn = std::cos(turn);
    float sinTurn = std::sin(turn);
    for (uint32 carriedId : carriedIds)
    {
        Placement const& carried = session->placements[carriedId];
        Placement moved = carried;
        float relX = carried.x - before.x;
        float relY = carried.y - before.y;
        moved.x = after.x + relX * cosTurn - relY * sinTurn;
        moved.y = after.y + relX * sinTurn + relY * cosTurn;
        moved.z = carried.z + dz;
        moved.o = NormalizeAngle(carried.o + turn);
        changes.push_back(Change{ carriedId, carried, moved });
    }

    return Commit(player, *session, label, std::move(changes), reason, merge);
}

bool PlayerHousingMgr::Commit(Player* player, Session& session, std::string const& label, std::vector<Change> changes, std::string& reason,
    bool merge, bool labelIsFinal)
{
    for (Change const& change : changes)
    {
        if (change.after && !IsSpotOnIsland(change.after->x, change.after->y, change.after->z))
        {
            reason = "That would take it off your island.";
            return false;
        }
        if (change.after && !(std::isfinite(change.after->o) && std::isfinite(change.after->pitch) && std::isfinite(change.after->roll)
                && std::isfinite(change.after->scale)))
        {
            reason = "That isn't a number the island understands.";
            return false;
        }
    }

    ObjectGuid::LowType self = player->GetGUID().GetCounter();
    auto selectedItr = session.selected.find(self);
    uint32 selectedBefore = selectedItr != session.selected.end() ? selectedItr->second : 0;
    std::vector<uint32> groupBefore = session.groups.count(self) ? session.groups[self] : std::vector<uint32>{};

    _report = {};
    std::string failure;
    if (!ApplyChanges(player, session, changes, true, failure))
    {
        reason = failure;
        return false;
    }

    // Applying the changes selected each piece in turn: the step is about the first one (or,
    // moving a group, the group as it was).
    uint32 subject = changes.front().after ? changes.front().placementId : 0;
    if (!KeepGroupAfterStep(session, self, selectedBefore, groupBefore, changes) && subject && session.placements.count(subject))
        session.selected[self] = subject;

    std::string entryLabel = label;
    if (!labelIsFinal)
    {
        // "turned 45° left" becomes "turned Westfall Chair 45° left".
        Placement const& before = changes.front().before ? *changes.front().before : *changes.front().after;
        std::string name = PieceName(before.itemEntry);
        size_t space = label.find(' ');
        std::string verb = label.substr(0, space);
        std::string rest = space == std::string::npos ? "" : label.substr(space);
        entryLabel = verb + " " + name + rest;
        if (changes.size() == 2)
            entryLabel += Acore::StringFormat(" with the {}", PieceName(changes[1].before ? changes[1].before->itemEntry : changes[1].after->itemEntry));
        else if (changes.size() > 2)
            entryLabel += Acore::StringFormat(" with {} pieces", changes.size() - 1);
    }
    Record(player, entryLabel, std::move(changes), merge);

    // It's there to see (and Undo names it): nothing to say.
    reason.clear();
    QuestEvent(player, QUEST_TOUR_CHANGE);
    SendAddonState(player);
    return true;
}

std::vector<uint32> PlayerHousingMgr::CarriedBy(Session const& session, uint32 placementId, bool includeInside) const
{
    std::vector<uint32> carried;
    std::set<uint32> seen{ placementId };
    auto add = [&](uint32 id)
    {
        if (seen.insert(id).second)
            carried.push_back(id);
    };

    auto root = session.placements.find(placementId);
    auto rootPiece = root != session.placements.end() ? _pieces.find(root->second.itemEntry) : _pieces.end();
    if (includeInside && rootPiece != _pieces.end() && rootPiece->second.IsBuilding())
        for (Placement const& inside : GetPiecesInside(session.ownerGuid, placementId))
            add(inside.id);

    // Then whatever stands on anything carried so far, level by level.
    for (size_t index = 0; index <= carried.size(); ++index)
    {
        uint32 holder = index == 0 ? placementId : carried[index - 1];
        for (auto const& [id, placement] : session.placements)
            if (placement.parent == holder)
                add(id);
    }
    return carried;
}

uint32 PlayerHousingMgr::FindSurfaceUnder(Session const& session, float x, float y, float z, std::set<uint32> const& exclude) const
{
    for (auto const& [id, surface] : session.placements)
    {
        if (exclude.count(id))
            continue;

        auto pieceItr = _pieces.find(surface.itemEntry);
        if (pieceItr == _pieces.end() || !pieceItr->second.HasFlag(PIECE_FLAG_SURFACE))
            continue;

        PieceDefinition const& piece = pieceItr->second;
        float scale = piece.scale > 0.0f ? surface.scale / piece.scale : 1.0f;
        float top = surface.z + piece.height * scale;
        if (std::fabs(z - top) <= 0.35f && IsOverSurface(piece, surface, x, y))
            return id;
    }
    return 0;
}

bool PlayerHousingMgr::IsOverSurface(PieceDefinition const& piece, Placement const& surface, float x, float y) const
{
    float scale = piece.scale > 0.0f ? surface.scale / piece.scale : 1.0f;
    float dx = x - surface.x;
    float dy = y - surface.y;
    if (!piece.HasOutline())
        return dx * dx + dy * dy <= piece.footprint * piece.footprint * scale * scale;
    float localX = (std::cos(surface.o) * dx + std::sin(surface.o) * dy) / scale;
    float localY = (-std::sin(surface.o) * dx + std::cos(surface.o) * dy) / scale;
    return localX >= piece.outlineMinX && localX <= piece.outlineMaxX && localY >= piece.outlineMinY && localY <= piece.outlineMaxY;
}

void PlayerHousingMgr::SelectPlacement(Player const* player, uint32 placementId)
{
    std::lock_guard<std::recursive_mutex> guard(_lock);
    if (!CanDecorate(player))
        return;
    auto itr = _sessionsByOwner.find(GetIslandOwner(player));
    if (itr != _sessionsByOwner.end() && itr->second.placements.count(placementId))
    {
        itr->second.selected[player->GetGUID().GetCounter()] = placementId;
        itr->second.groups.erase(player->GetGUID().GetCounter());  // one piece again
    }
}

// A green ring under the piece chosen in the Placed list, as wide as the piece (the rune model
// is about 3.4 yards across at size 1), so it's clear which of several alike it is. One per
// player; it goes when another is chosen, the list closes, the piece moves or the player
// leaves.
void PlayerHousingMgr::HighlightPlacement(Player* player, uint32 placementId)
{
    std::lock_guard<std::recursive_mutex> guard(_lock);
    auto ownerItr = _playerOwnerByGuid.find(player->GetGUID());
    auto sessionItr = ownerItr != _playerOwnerByGuid.end() ? _sessionsByOwner.find(ownerItr->second) : _sessionsByOwner.end();
    if (sessionItr == _sessionsByOwner.end())
        return;
    Session& session = sessionItr->second;
    Map* map = player->GetMap();
    ObjectGuid::LowType self = player->GetGUID().GetCounter();
    RemoveHighlight(session, map, self);

    auto placementItr = session.placements.find(placementId);
    PieceDefinition const* piece = placementItr != session.placements.end() ? GetPiece(placementItr->second.itemEntry) : nullptr;
    if (!piece)
        return;
    Placement const& placement = placementItr->second;
    float scale = std::clamp(piece->footprint * (piece->scale > 0.0f ? placement.scale / piece->scale : 1.0f) / 1.5f, 0.3f, 12.0f);
    GameObject* ring = new GameObject();
    if (!ring->Create(map->GenerateLowGuid<HighGuid::GameObject>(), HIGHLIGHT_RING_GO, map, PHASEMASK_NORMAL,
            placement.x, placement.y, placement.z + 0.03f, 0.0f, G3D::Quat(0.0f, 0.0f, 0.0f, 0.0f), 100, GO_STATE_READY))
    {
        delete ring;
        return;
    }
    ring->SetObjectScale(scale);
    ring->SetRespawnTime(0);
    ring->SetSpawnedByDefault(false);
    if (!map->AddToMap(ring))
    {
        delete ring;
        return;
    }
    ring->SetPhaseMask(session.phaseMask, true);
    ring->EnableCollision(false);
    session.highlights[self] = Highlight{ placementId, ring->GetGUID() };
}

void PlayerHousingMgr::RemoveHighlight(Session& session, Map* map, ObjectGuid::LowType player)
{
    auto itr = session.highlights.find(player);
    if (itr == session.highlights.end())
        return;
    if (map)
        if (GameObject* ring = map->GetGameObject(itr->second.ring))
            ring->AddObjectToRemoveList();
    session.highlights.erase(itr);
}

void PlayerHousingMgr::RemoveHighlightsOf(Session& session, Map* map, uint32 placementId)
{
    std::vector<ObjectGuid::LowType> players;
    for (auto const& [player, highlight] : session.highlights)
        if (highlight.placementId == placementId)
            players.push_back(player);
    for (ObjectGuid::LowType player : players)
        RemoveHighlight(session, map, player);
}

ObjectGuid PlayerHousingMgr::GetObjectForPlacement(Player const* player, uint32 placementId) const
{
    std::lock_guard<std::recursive_mutex> guard(_lock);
    Session const* session = FindSessionOf(player);
    if (!session)
        return ObjectGuid::Empty;

    auto itr = session->spawned.find(placementId);
    return itr != session->spawned.end() ? itr->second.guid : ObjectGuid::Empty;
}

bool PlayerHousingMgr::Rotate(Player* player, uint32 placementId, float degrees, std::string& reason)
{
    if (!placementId)
    {
        std::vector<uint32> group = GetGroup(player);
        if (group.size() > 1)
        {
            std::lock_guard<std::recursive_mutex> guard(_lock);
            Session* session = GetOwnerSession(player, reason);
            degrees = std::clamp(degrees, -360.0f, 360.0f);
            return session && ShiftGroup(player, *session, group, 0.0f, 0.0f, 0.0f, degrees * PI_F / 180.0f, false, "turned",
                Acore::StringFormat(" {:.0f}° {}", std::fabs(degrees), degrees >= 0.0f ? "left" : "right"), reason, false);
        }
    }

    std::string label = Acore::StringFormat("turned {:.0f}° {}", std::fabs(degrees), degrees >= 0.0f ? "left" : "right");
    return Transform(player, placementId, label, 0.0f, 0.0f, 0.0f, degrees * PI_F / 180.0f, false, 0.0f, reason);
}

bool PlayerHousingMgr::Nudge(Player* player, uint32 placementId, float forward, float left, float up, std::string& reason)
{
    // Directions are the player's: forward is where the player is facing.
    float po = player->GetOrientation();
    float dx = std::cos(po) * forward - std::sin(po) * left;
    float dy = std::sin(po) * forward + std::cos(po) * left;

    // On the grid: one square along the grid line closest to that direction, onto the grid.
    float grid = GetGridSize(player->GetGUID().GetCounter());
    std::optional<Placement> placement = GetPlacement(player, ResolvePlacementArgument(player, placementId));
    if (grid > 0.0f && placement && (dx != 0.0f || dy != 0.0f))
    {
        float x = placement->x;
        float y = placement->y;
        if (std::fabs(dx) >= std::fabs(dy))
            x += dx > 0.0f ? grid : -grid;
        else
            y += dy > 0.0f ? grid : -grid;
        SnapToGrid(player->GetGUID().GetCounter(), x, y);
        dx = x - placement->x;
        dy = y - placement->y;
    }

    std::string label = "nudged";
    if (up > 0.0f)
        label = "raised";
    else if (up < 0.0f)
        label = "lowered";

    // Several pieces selected: all of them, the same step (on the grid, the first one lands
    // on it and the rest keep their places around it).
    if (!placementId)
    {
        std::vector<uint32> group = GetGroup(player);
        if (group.size() > 1)
        {
            std::lock_guard<std::recursive_mutex> guard(_lock);
            Session* session = GetOwnerSession(player, reason);
            return session && ShiftGroup(player, *session, group, dx, dy, up, 0.0f, true, label, "", reason, false);
        }
    }
    return Transform(player, placementId, label, dx, dy, up, 0.0f, false, 0.0f, reason);
}

bool PlayerHousingMgr::Shift(Player* player, uint32 placementId, float forward, float left, float up, float degrees, std::string& reason)
{
    // Several pieces selected: they move and turn together, about their middle.
    if (!placementId)
    {
        std::vector<uint32> group = GetGroup(player);
        if (group.size() > 1)
        {
            std::lock_guard<std::recursive_mutex> guard(_lock);
            Session* session = GetOwnerSession(player, reason);
            if (!session)
                return false;
            forward = std::clamp(forward, -5.0f, 5.0f);
            left = std::clamp(left, -5.0f, 5.0f);
            float po = player->GetOrientation();
            if (!ShiftGroup(player, *session, group, std::cos(po) * forward - std::sin(po) * left, std::sin(po) * forward + std::cos(po) * left,
                    std::clamp(up, -2.0f, 2.0f), std::clamp(degrees, -360.0f, 360.0f) * PI_F / 180.0f, true, "adjusted", "", reason, true))
                return false;
            reason.clear();  // quiet, like one piece: the addon shows it
            return true;
        }
    }

    std::optional<Placement> placement = GetPlacement(player, ResolvePlacementArgument(player, placementId));
    if (!placement)
    {
        reason = "Choose a piece first: right-click it with the housing window open, or click it in the Placed tab.";
        return false;
    }

    // A few yards and a full turn at most per message: the addon sends several a second.
    forward = std::clamp(forward, -5.0f, 5.0f);
    left = std::clamp(left, -5.0f, 5.0f);
    up = std::clamp(up, -2.0f, 2.0f);
    degrees = std::clamp(degrees, -360.0f, 360.0f);
    if (forward == 0.0f && left == 0.0f && up == 0.0f && degrees == 0.0f)
        return true;

    float po = player->GetOrientation();
    float dx = std::cos(po) * forward - std::sin(po) * left;
    float dy = std::sin(po) * forward + std::cos(po) * left;
    if (dx != 0.0f || dy != 0.0f)
    {
        // With the grid on, the piece lands on it (the addon steps a square at a time).
        float x = placement->x + dx;
        float y = placement->y + dy;
        if (GetGridSize(player->GetGUID().GetCounter()) > 0.0f)
            SnapToGrid(player->GetGUID().GetCounter(), x, y);
        dx = x - placement->x;
        dy = y - placement->y;
    }

    return Transform(player, placement->id, "adjusted", dx, dy, up, degrees * PI_F / 180.0f, false, 0.0f, reason, true);
}

bool PlayerHousingMgr::SetEditMode(Player* player, bool on, std::string& reason)
{
    {
        std::lock_guard<std::recursive_mutex> guard(_lock);
        if (on)
            _editMode.insert(player->GetGUID());
        else
            _editMode.erase(player->GetGUID());
    }

    // Edit mode is decorating with the keys: pieces are clickable.
    if (on && !IsDecorating(player) && !SetDecorating(player, true, reason))
    {
        std::lock_guard<std::recursive_mutex> guard(_lock);
        _editMode.erase(player->GetGUID());
        return false;
    }
    if (!on && IsDecorating(player))
        SetDecorating(player, false, reason);

    reason.clear();  // the addon's banner says so
    SendAddonState(player);
    return true;
}

bool PlayerHousingMgr::IsInEditMode(Player const* player) const
{
    std::lock_guard<std::recursive_mutex> guard(_lock);
    return _editMode.count(player->GetGUID()) && CanDecorate(player) && IsDecorating(player);
}

uint32 PlayerHousingMgr::SelectNext(Player* player, bool backwards, std::string& reason)
{
    std::vector<std::pair<Placement, float>> nearby = GetNearbyPlacements(player, 40.0f);
    if (nearby.empty() || !CanDecorate(player))
    {
        reason = "No pieces within 40 yards.";
        return 0;
    }

    uint32 current = GetSelectedPlacement(player);
    size_t index = 0;
    for (size_t i = 0; i < nearby.size(); ++i)
        if (nearby[i].first.id == current)
            index = backwards ? (i + nearby.size() - 1) % nearby.size() : (i + 1) % nearby.size();
    if (!current || std::none_of(nearby.begin(), nearby.end(), [&](auto const& entry) { return entry.first.id == current; }))
        index = 0;

    uint32 id = nearby[index].first.id;
    SelectPlacement(player, id);
    SendAddonState(player);
    return id;
}

bool PlayerHousingMgr::FaceMe(Player* player, uint32 placementId, std::string& reason)
{
    std::optional<Placement> placement = GetPlacement(player, ResolvePlacementArgument(player, placementId));
    if (!placement)
    {
        reason = "Choose a piece first: click it while decorating, or stand next to it.";
        return false;
    }

    float o = std::atan2(player->GetPositionY() - placement->y, player->GetPositionX() - placement->x);
    if (!placementId)
    {
        std::vector<uint32> group = GetGroup(player);
        if (group.size() > 1)
        {
            // The group turns about its middle until its first piece faces the player.
            std::lock_guard<std::recursive_mutex> guard(_lock);
            Session* session = GetOwnerSession(player, reason);
            float turn = std::remainder(o - placement->o, 2.0f * PI_F);
            return session && ShiftGroup(player, *session, group, 0.0f, 0.0f, 0.0f, turn, false, "turned", " toward you", reason, false);
        }
    }
    return Transform(player, placement->id, "turned toward you", 0.0f, 0.0f, 0.0f, 0.0f, true, o, reason);
}

bool PlayerHousingMgr::MoveHere(Player* player, uint32 placementId, std::string& reason)
{
    std::optional<Placement> placement = GetPlacement(player, ResolvePlacementArgument(player, placementId));
    if (!placement)
    {
        reason = "Choose a piece first: click it while decorating, or stand next to it.";
        return false;
    }

    if (!placementId)
    {
        std::vector<uint32> group = GetGroup(player);
        if (group.size() > 1)
        {
            std::lock_guard<std::recursive_mutex> guard(_lock);
            Session* session = GetOwnerSession(player, reason);
            return session && MoveGroupTo(player, *session, group, player->GetPosition(), reason);
        }
    }

    float x = player->GetPositionX();
    float y = player->GetPositionY();
    SnapToGrid(player->GetGUID().GetCounter(), x, y);
    return Transform(player, placement->id, "moved", x - placement->x, y - placement->y, player->GetPositionZ() - placement->z,
        0.0f, false, 0.0f, reason);
}

bool PlayerHousingMgr::Resize(Player* player, uint32 placementId, float percent, bool relative, std::string& reason)
{
    if (!placementId && IsCarrying(player))
        return ResizeGhost(player, percent, relative, reason);
    if (!placementId && GetGroup(player).size() > 1)
    {
        reason = "Size and tilt change one piece at a time: click just that piece.";
        return false;
    }

    std::lock_guard<std::recursive_mutex> guard(_lock);
    Session* session = GetOwnerSession(player, reason);
    if (!session)
        return false;

    placementId = ResolvePlacementArgument(player, placementId);
    auto itr = session->placements.find(placementId);
    PieceDefinition const* piece = itr != session->placements.end() ? GetPiece(itr->second.itemEntry) : nullptr;
    if (!piece)
    {
        reason = "Choose a piece first: click it while decorating, or stand next to it.";
        return false;
    }

    int32 lowest = int32(std::lround(_sizeMin * 100.0f));
    int32 highest = int32(std::lround(_sizeMax * 100.0f));
    if (lowest >= 100 && highest <= 100)
    {
        reason = "Pieces keep their size on this server.";
        return false;
    }

    // Sizes are whole percents of the piece's normal size, so steps land on round numbers.
    Placement const before = itr->second;
    float normal = piece->scale > 0.0f ? piece->scale : 1.0f;
    int32 current = int32(std::lround(before.scale / normal * 100.0f));
    int32 wanted = std::clamp(relative ? current + int32(std::lround(percent)) : int32(std::lround(percent)), lowest, highest);
    if (wanted == current)
    {
        if (!relative && wanted == 100)
            reason = "It's already its normal size.";
        else if (wanted == highest && (relative ? percent > 0.0f : percent >= highest))
            reason = Acore::StringFormat("That's as big as it gets ({}%).", highest);
        else if (wanted == lowest)
            reason = Acore::StringFormat("That's as small as it gets ({}%).", lowest);
        else
            reason = Acore::StringFormat("It's already {}%.", wanted);
        return false;
    }

    Placement after = before;
    after.scale = normal * float(wanted) / 100.0f;
    float k = after.scale / before.scale;

    // What stands on it keeps its place on the bigger or smaller top; what's inside a
    // building keeps its place in the rooms. Pieces on those move with what they stand on.
    std::vector<uint32> carriedIds = CarriedBy(*session, placementId, true);
    std::set<uint32> carriedSet(carriedIds.begin(), carriedIds.end());
    std::map<uint32, std::array<float, 3>> shifts;
    std::function<std::array<float, 3>(uint32, uint32)> shiftOf = [&](uint32 id, uint32 depth) -> std::array<float, 3>
    {
        auto known = shifts.find(id);
        if (known != shifts.end())
            return known->second;
        Placement const& carried = session->placements[id];
        std::array<float, 3> shift;
        if (depth < 16 && carried.parent != placementId && carriedSet.count(carried.parent))
            shift = shiftOf(carried.parent, depth + 1);
        else
            shift = { (carried.x - before.x) * (k - 1.0f), (carried.y - before.y) * (k - 1.0f), (carried.z - before.z) * (k - 1.0f) };
        shifts[id] = shift;
        return shift;
    };

    std::vector<Change> changes{ Change{ placementId, before, after } };
    for (uint32 carriedId : carriedIds)
    {
        Placement const& carried = session->placements[carriedId];
        std::array<float, 3> shift = shiftOf(carriedId, 0);
        Placement moved = carried;
        moved.x += shift[0];
        moved.y += shift[1];
        moved.z += shift[2];
        changes.push_back(Change{ carriedId, carried, moved });
    }

    std::string label;
    if (wanted == 100)
        label = "brought back to normal size";
    else
        label = Acore::StringFormat("made {} ({}%)", wanted > current ? "bigger" : "smaller", wanted);
    return Commit(player, *session, label, std::move(changes), reason);
}

bool PlayerHousingMgr::Tilt(Player* player, uint32 placementId, float forwardDegrees, float rightDegrees, bool straighten, std::string& reason)
{
    if (!placementId && IsCarrying(player))
        return TiltGhost(player, forwardDegrees, rightDegrees, straighten, reason);
    if (!placementId && GetGroup(player).size() > 1)
    {
        reason = "Size and tilt change one piece at a time: click just that piece.";
        return false;
    }

    std::lock_guard<std::recursive_mutex> guard(_lock);
    Session* session = GetOwnerSession(player, reason);
    if (!session)
        return false;

    placementId = ResolvePlacementArgument(player, placementId);
    auto itr = session->placements.find(placementId);
    PieceDefinition const* piece = itr != session->placements.end() ? GetPiece(itr->second.itemEntry) : nullptr;
    if (!piece)
    {
        reason = "Choose a piece first: click it while decorating, or stand next to it.";
        return false;
    }

    if (piece->IsCreature())
    {
        reason = piece->HasFlag(PIECE_FLAG_STAND) ? "Mannequins always stand upright." : "Figurines always stand upright.";
        return false;
    }

    if (_tiltMax <= 0.0f && !straighten)
    {
        reason = "Pieces stand upright on this server.";
        return false;
    }

    // Tenths of a degree, so steps back and forth land back on level. Up to the server's limit
    // either way; at 180 there's no limit: a piece turns right over (upside down) and on round,
    // kept between -180 and 180.
    auto toTenths = [](float radians) { return int32(std::lround(radians * 1800.0f / PI_F)); };
    auto toRadians = [](int32 tenths) { return float(tenths) * PI_F / 1800.0f; };
    int32 limit = int32(std::lround(_tiltMax * 10.0f));
    bool allTheWay = _tiltMax >= 180.0f;
    auto tilted = [&](float radians, float degrees)
    {
        int32 tenths = toTenths(radians) + int32(std::lround(degrees * 10.0f));
        if (!allTheWay)
            return std::clamp(tenths, -limit, limit);
        tenths %= 3600;
        if (tenths > 1800)
            tenths -= 3600;
        else if (tenths <= -1800)
            tenths += 3600;
        return tenths;
    };

    Placement const before = itr->second;
    int32 pitch = straighten ? 0 : tilted(before.pitch, forwardDegrees);
    int32 roll = straighten ? 0 : tilted(before.roll, rightDegrees);
    if (pitch == toTenths(before.pitch) && roll == toTenths(before.roll))
    {
        reason = straighten ? "It's already standing straight." : Acore::StringFormat("That's as far as it tilts ({:.0f}°).", _tiltMax);
        return false;
    }

    Placement after = before;
    after.pitch = toRadians(pitch);
    after.roll = toRadians(roll);

    // Only the piece tilts: what stands on it stays where it is.
    std::string label;
    if (straighten)
        label = "stood straight";
    else if (forwardDegrees != 0.0f)
        label = Acore::StringFormat("tilted {:.0f}° {}", std::fabs(forwardDegrees), forwardDegrees > 0.0f ? "forward" : "back");
    else
        label = Acore::StringFormat("tilted {:.0f}° to its {}", std::fabs(rightDegrees), rightDegrees > 0.0f ? "right" : "left");
    return Commit(player, *session, label, { Change{ placementId, before, after } }, reason);
}

bool PlayerHousingMgr::PlaceAnother(Player* player, uint32 placementId, std::string& reason)
{
    // A ghost of a new one follows the player, with this one's turn, size and tilt.
    placementId = ResolvePlacementArgument(player, placementId);
    std::optional<Placement> source = GetPlacement(player, placementId);
    if (!source)
    {
        reason = "Choose a piece first: click it while decorating, or stand next to it.";
        return false;
    }
    return StartGhostNew(player, source->itemEntry, source->id, reason);
}

void PlayerHousingMgr::SnapToGrid(ObjectGuid::LowType guid, float& x, float& y) const
{
    float grid = GetGridSize(guid);
    if (grid <= 0.0f)
        return;
    x = std::round(x / grid) * grid;
    y = std::round(y / grid) * grid;
}

bool PlayerHousingMgr::PackUpEverything(Player* player, std::string& reason)
{
    if (OnCooldown(player, COOLDOWN_HEAVY, 3000, reason))
        return false;
    std::lock_guard<std::recursive_mutex> guard(_lock);
    Session* session = GetOwnerSession(player, reason, true);
    if (!session)
        return false;

    if (session->placements.empty())
    {
        reason = "There's nothing to pack up.";
        return false;
    }

    std::vector<Change> changes;
    for (auto const& [id, placement] : session->placements)
        changes.push_back(Change{ id, placement, std::nullopt });

    _report = {};
    std::string failure;
    ApplyChanges(player, *session, changes, true, failure);
    uint32 count = uint32(changes.size());
    Record(player, Acore::StringFormat("packed up {} pieces", count), std::move(changes));

    reason = Acore::StringFormat("Packed up {} pieces.{} Changed your mind? Undo.", count, DescribeReturns());
    SendAddonState(player);
    return true;
}

bool PlayerHousingMgr::OpenBankAtChest(Player* player, uint32 placementId, std::string& reason)
{
    std::lock_guard<std::recursive_mutex> guard(_lock);
    Session* session = GetOwnerSession(player, reason, true);
    if (!session)
        return false;

    auto itr = session->placements.find(placementId);
    PieceDefinition const* piece = itr != session->placements.end() ? GetPiece(itr->second.itemEntry) : nullptr;
    if (!piece || !piece->HasFlag(PIECE_FLAG_CHEST))
    {
        reason = "That isn't a Bank Chest.";
        return false;
    }

    Placement const& chest = itr->second;
    if (player->GetExactDist2d(chest.x, chest.y) > 8.0f)
    {
        reason = "Step closer to the chest.";
        return false;
    }

    // The banker stays for a few minutes; the bank works only near it, as with any banker.
    Map* map = player->GetMap();
    auto previous = _chestBankers.find(player->GetGUID());
    if (previous != _chestBankers.end())
    {
        if (Creature* old = map->GetCreature(previous->second))
            old->DespawnOrUnsummon();
        _chestBankers.erase(previous);
    }

    Creature* banker = player->SummonCreature(CHEST_BANKER_ENTRY, chest.x, chest.y, chest.z + 0.5f, chest.o, TEMPSUMMON_TIMED_DESPAWN,
        5 * MINUTE * IN_MILLISECONDS);
    if (!banker)
    {
        reason = "The chest won't open. Try again in a moment.";
        return false;
    }
    banker->SetPhaseMask(session->phaseMask, true);
    _chestBankers[player->GetGUID()] = banker->GetGUID();
    player->GetSession()->SendShowBank(banker->GetGUID());
    return true;
}

bool PlayerHousingMgr::SetDecorating(Player* player, bool on, std::string& reason)
{
    std::lock_guard<std::recursive_mutex> guard(_lock);
    Session* session = GetOwnerSession(player, reason);
    if (!session)
        return false;

    if (session->decorating == on)
    {
        reason = on ? "You're already decorating." : "You're not decorating.";
        return false;
    }

    Map* map = player->GetMap();
    session->decorating = on;
    for (auto const& [id, placement] : session->placements)
    {
        auto pieceItr = _pieces.find(placement.itemEntry);
        if (pieceItr != _pieces.end() && pieceItr->second.editGoEntry)
            RespawnPlacement(*session, map, id);
    }

    // The addon shows it; typed, a line.
    bool quiet = HasAddon(player);
    if (on)
        reason = quiet ? "" : "Decorating: click any piece to select it. .house decorate off when you're finished.";
    else
        reason = quiet ? "" : "Done decorating: chairs, mailboxes and crafting stations work again.";

    SendAddonState(player);
    return true;
}
