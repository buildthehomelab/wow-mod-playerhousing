#include "PlayerHousingMgr.h"

#include "Bag.h"
#include "Creature.h"
#include "DatabaseEnv.h"
#include "DBCStores.h"
#include "Item.h"
#include "Log.h"
#include "Mail.h"
#include "Map.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "StringFormat.h"
#include "TemporarySummon.h"
#include "WorldPacket.h"
#include "WorldSession.h"

#include <cctype>
#include <functional>
#include <iterator>

using namespace Housing;

// Stands are mannequins: a figure (a creature with the mirror image flag) that wears real
// gear. Its armor reaches the client as mirror image data, which the client asks for when
// the figure comes into view (see SendMannequinLook); its weapons are virtual items.
// Changing what a stand wears respawns the figure, so every viewer asks again.

namespace
{
    // The slots a mirror image shows, in the order of SMSG_MIRRORIMAGE_DATA.

    constexpr uint8 MIRROR_SLOTS[] =
    {
        EQUIPMENT_SLOT_HEAD, EQUIPMENT_SLOT_SHOULDERS, EQUIPMENT_SLOT_BODY, EQUIPMENT_SLOT_CHEST,
        EQUIPMENT_SLOT_WAIST, EQUIPMENT_SLOT_LEGS, EQUIPMENT_SLOT_FEET, EQUIPMENT_SLOT_WRISTS,
        EQUIPMENT_SLOT_HANDS, EQUIPMENT_SLOT_BACK, EQUIPMENT_SLOT_TABARD
    };

    // The figures a stand can take, in the order "Change the figure" goes through them.
    constexpr uint8 RACES[] = { RACE_HUMAN, RACE_DWARF, RACE_NIGHTELF, RACE_GNOME, RACE_DRAENEI,
                                RACE_ORC, RACE_UNDEAD_PLAYER, RACE_TAUREN, RACE_TROLL, RACE_BLOODELF };

    // A stand's look, in one number (older ones have only race and gender, the rest 0):
    //   bits 0-3 race, 4-7 pose, 8 gender, 9-13 skin, 14-18 face, 19-23 hair, 24-27 hair
    //   color, 28-31 facial hair, 32-35 the pose's high bits (poses past the first 16).
    uint8 LookRace(uint64 look)
    {
        uint8 race = uint8(look & 0x0F);
        for (uint8 known : RACES)
            if (known == race)
                return race;
        return RACE_HUMAN;
    }

    uint8 LookGender(uint64 look) { return (look >> 8) & 1 ? GENDER_FEMALE : GENDER_MALE; }

    struct Features
    {
        uint8 skin{0};
        uint8 face{0};
        uint8 hair{0};
        uint8 hairColor{0};
        uint8 facial{0};
    };

    Features LookFeatures(uint64 look)
    {
        return Features{ uint8((look >> 9) & 0x1F), uint8((look >> 14) & 0x1F), uint8((look >> 19) & 0x1F),
                         uint8((look >> 24) & 0x0F), uint8((look >> 28) & 0x0F) };
    }

    // Poses are for version 2 (only some animations hold still the same for every viewer):
    // every stand stands. Its pose's bits in the look are kept as stored, unused for now.
    uint64 MakeLook(uint8 race, uint8 gender, uint8 pose, Features const& f)
    {
        return uint64(race & 0x0F) | (uint64(pose & 0x0F) << 4) | (uint64(gender & 1) << 8) | (uint64(f.skin & 0x1F) << 9)
            | (uint64(f.face & 0x1F) << 14) | (uint64(f.hair & 0x1F) << 19) | (uint64(f.hairColor & 0x0F) << 24)
            | (uint64(f.facial & 0x0F) << 28) | (uint64((pose >> 4) & 0x0F) << 32);
    }

    // A random look a character of that race and gender could have at creation: skin, then a
    // face for that skin, a hairstyle and a color it comes in, facial hair in that color.
    Features RandomFeatures(uint8 race, uint8 gender)
    {
        std::vector<CharSectionsEntry const*> sections;
        for (uint32 i = 0; i < sCharSectionsStore.GetNumRows(); ++i)
            if (CharSectionsEntry const* entry = sCharSectionsStore.LookupEntry(i))
                if (entry->RaceID == race && entry->SexID == gender && (entry->Flags & SECTION_FLAG_PLAYER) && !(entry->Flags & SECTION_FLAG_DEATH_KNIGHT))
                    sections.push_back(entry);
        auto pick = [&](uint32 genType, std::function<bool(CharSectionsEntry const*)> fits, bool type) -> int32
        {
            std::vector<uint32> values;
            for (CharSectionsEntry const* entry : sections)
                if (entry->BaseSection == genType && fits(entry))
                    values.push_back(type ? entry->VariationIndex : entry->ColorIndex);
            return values.empty() ? -1 : int32(values[urand(0, uint32(values.size()) - 1)]);
        };
        auto any = [](CharSectionsEntry const*) { return true; };
        Features f;
        int32 skin = pick(0, any, false);
        f.skin = uint8(std::max(skin, 0));
        int32 face = pick(1, [&](CharSectionsEntry const* e) { return int32(e->ColorIndex) == skin; }, true);
        f.face = uint8(std::max(face >= 0 ? face : pick(1, any, true), 0));
        int32 hair = pick(3, any, true);
        f.hair = uint8(std::max(hair, 0));
        int32 hairColor = pick(3, [&](CharSectionsEntry const* e) { return int32(e->VariationIndex) == hair; }, false);
        f.hairColor = uint8(std::max(hairColor, 0));
        int32 facial = pick(2, [&](CharSectionsEntry const* e) { return int32(e->ColorIndex) == hairColor; }, true);
        f.facial = uint8(std::max(facial, 0));
        return f;
    }

    char const* RaceName(uint8 race)
    {
        switch (race)
        {
            case RACE_DWARF: return "Dwarf";
            case RACE_NIGHTELF: return "Night Elf";
            case RACE_GNOME: return "Gnome";
            case RACE_DRAENEI: return "Draenei";
            case RACE_ORC: return "Orc";
            case RACE_UNDEAD_PLAYER: return "Forsaken";
            case RACE_TAUREN: return "Tauren";
            case RACE_TROLL: return "Troll";
            case RACE_BLOODELF: return "Blood Elf";
            default: return "Human";
        }
    }

    ObjectGuid ItemGuid(uint32 low) { return ObjectGuid::Create<HighGuid::Item>(low); }

    // In the bags or the backpack: not worn, not in the bank, not being traded.
    bool IsInBags(Player* player, Item* item)
    {
        return item && item->GetOwnerGUID() == player->GetGUID() && Player::IsInventoryPos(item->GetPos()) && !item->IsInTrade();
    }

    std::string Capitalized(std::string text)
    {
        if (!text.empty())
            text[0] = char(std::toupper(static_cast<unsigned char>(text[0])));
        return text;
    }
}

std::string PlayerHousingMgr::StandItemName(uint32 itemEntry)
{
    ItemTemplate const* proto = sObjectMgr->GetItemTemplate(itemEntry);
    return proto ? proto->Name1 : "an item";
}

int8 PlayerHousingMgr::StandSlotFor(ItemTemplate const* proto, std::map<uint8, GearItem> const& worn)
{
    if (!proto || (proto->Class != ITEM_CLASS_WEAPON && proto->Class != ITEM_CLASS_ARMOR))
        return -1;

    switch (proto->InventoryType)
    {
        case INVTYPE_HEAD: return EQUIPMENT_SLOT_HEAD;
        case INVTYPE_SHOULDERS: return EQUIPMENT_SLOT_SHOULDERS;
        case INVTYPE_BODY: return EQUIPMENT_SLOT_BODY;
        case INVTYPE_CHEST:
        case INVTYPE_ROBE: return EQUIPMENT_SLOT_CHEST;
        case INVTYPE_WAIST: return EQUIPMENT_SLOT_WAIST;
        case INVTYPE_LEGS: return EQUIPMENT_SLOT_LEGS;
        case INVTYPE_FEET: return EQUIPMENT_SLOT_FEET;
        case INVTYPE_WRISTS: return EQUIPMENT_SLOT_WRISTS;
        case INVTYPE_HANDS: return EQUIPMENT_SLOT_HANDS;
        case INVTYPE_CLOAK: return EQUIPMENT_SLOT_BACK;
        case INVTYPE_TABARD: return EQUIPMENT_SLOT_TABARD;
        case INVTYPE_2HWEAPON:
        case INVTYPE_WEAPONMAINHAND: return EQUIPMENT_SLOT_MAINHAND;
        case INVTYPE_SHIELD:
        case INVTYPE_WEAPONOFFHAND:
        case INVTYPE_HOLDABLE: return EQUIPMENT_SLOT_OFFHAND;
        // A one-handed weapon goes in the free hand, the main hand first.
        case INVTYPE_WEAPON:
            return worn.count(EQUIPMENT_SLOT_MAINHAND) && !worn.count(EQUIPMENT_SLOT_OFFHAND) ? EQUIPMENT_SLOT_OFFHAND : EQUIPMENT_SLOT_MAINHAND;
        case INVTYPE_RANGED:
        case INVTYPE_RANGEDRIGHT:
        case INVTYPE_THROWN: return EQUIPMENT_SLOT_RANGED;
        default: return -1;  // rings, trinkets, necks and relics don't show
    }
}

char const* PlayerHousingMgr::StandSlotName(uint8 slot)
{
    switch (slot)
    {
        case EQUIPMENT_SLOT_HEAD: return "head";
        case EQUIPMENT_SLOT_SHOULDERS: return "shoulders";
        case EQUIPMENT_SLOT_BODY: return "shirt";
        case EQUIPMENT_SLOT_CHEST: return "chest";
        case EQUIPMENT_SLOT_WAIST: return "waist";
        case EQUIPMENT_SLOT_LEGS: return "legs";
        case EQUIPMENT_SLOT_FEET: return "feet";
        case EQUIPMENT_SLOT_WRISTS: return "wrists";
        case EQUIPMENT_SLOT_HANDS: return "hands";
        case EQUIPMENT_SLOT_BACK: return "back";
        case EQUIPMENT_SLOT_TABARD: return "tabard";
        case EQUIPMENT_SLOT_MAINHAND: return "main hand";
        case EQUIPMENT_SLOT_OFFHAND: return "off hand";
        case EQUIPMENT_SLOT_RANGED: return "ranged";
        default: return "gear";
    }
}

std::string PlayerHousingMgr::LookName(uint64 look)
{
    return Acore::StringFormat("{} {}", RaceName(LookRace(look)), LookGender(look) == GENDER_FEMALE ? "woman" : "man");
}

std::vector<Item*> PlayerHousingMgr::GetWearableItems(Player* player) const
{
    std::vector<Item*> items;
    std::map<uint8, GearItem> none;
    auto consider = [&](Item* item)
    {
        if (item && IsInBags(player, item) && StandSlotFor(item->GetTemplate(), none) >= 0)
            items.push_back(item);
    };

    for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
        consider(player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot));
    for (uint8 bagSlot = INVENTORY_SLOT_BAG_START; bagSlot < INVENTORY_SLOT_BAG_END; ++bagSlot)
        if (Bag* bag = player->GetBagByPos(bagSlot))
            for (uint32 slot = 0; slot < bag->GetBagSize(); ++slot)
                consider(bag->GetItemByPos(uint8(slot)));
    return items;
}

bool PlayerHousingMgr::MoveGearToStand(Player* player, ObjectGuid::LowType ownerGuid, uint32 placementId, uint8 slot, uint32 itemGuid, std::string& reason)
{
    Item* item = player->GetItemByGuid(ItemGuid(itemGuid));
    if (!IsInBags(player, item))
    {
        reason = "That isn't in your bags any more.";
        return false;
    }

    // Out of the inventory, but kept in item_instance with its enchants, gems and binding,
    // the way mail keeps its items.
    player->MoveItemFromInventory(item->GetBagSlot(), item->GetSlot(), true);
    CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();
    item->DeleteFromInventoryDB(trans);
    item->SetState(ITEM_CHANGED);
    item->SaveToDB(trans);
    trans->Append("REPLACE INTO mod_playerhousing_placement_gear (owner_guid, placement_id, slot, item_guid, item_entry) VALUES ({}, {}, {}, {}, {})",
        ownerGuid, placementId, uint32(slot), itemGuid, item->GetEntry());
    player->SaveInventoryAndGoldToDB(trans);
    CharacterDatabase.CommitTransaction(trans);
    delete item;
    return true;
}

Item* PlayerHousingMgr::LoadGearItem(ObjectGuid::LowType ownerGuid, GearItem const& gear) const
{
    ItemTemplate const* proto = sObjectMgr->GetItemTemplate(gear.itemEntry);
    QueryResult result = proto ? CharacterDatabase.Query(
        "SELECT creatorGuid, giftCreatorGuid, count, duration, charges, flags, enchantments, randomPropertyId, durability, playedTime, text, "
        "guid, itemEntry, owner_guid FROM item_instance WHERE guid={}", gear.itemGuid) : QueryResult();
    Item* item = proto ? NewItemOrBag(proto) : nullptr;
    if (!result || !item || !item->LoadFromDB(gear.itemGuid, ObjectGuid::Create<HighGuid::Player>(ownerGuid), result->Fetch(), gear.itemEntry))
    {
        delete item;
        return nullptr;
    }
    return item;
}

void PlayerHousingMgr::ReturnGear(Player* player, ObjectGuid::LowType islandOwner, ObjectGuid::LowType gearOwner, uint32 placementId, uint8 slot,
    GearItem const& gear)
{
    std::string deleteRow = Acore::StringFormat(
        "DELETE FROM mod_playerhousing_placement_gear WHERE owner_guid={} AND placement_id={} AND slot={}", islandOwner, placementId, uint32(slot));
    ObjectGuid::LowType ownerGuid = gearOwner;
    // Someone else's gear (a roommate's mannequin picked up by the owner) goes by mail.
    if (player && HomeOf(player) != gearOwner)
        player = nullptr;

    ItemTemplate const* proto = sObjectMgr->GetItemTemplate(gear.itemEntry);
    Item* item = LoadGearItem(ownerGuid, gear);
    if (!item)
    {
        LOG_ERROR("module", "mod-playerhousing: Item {} (entry {}) on stand {} of {} is gone from item_instance.", gear.itemGuid, gear.itemEntry, placementId, ownerGuid);
        CharacterDatabase.DirectExecute(deleteRow);
        return;
    }

    CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();
    trans->Append(deleteRow);

    ItemPosCountVec dest;
    if (player && player->CanStoreItem(NULL_BAG, NULL_SLOT, dest, item, false) == EQUIP_ERR_OK)
    {
        item->SetState(ITEM_UNCHANGED);
        player->MoveItemToInventory(dest, item, true);
        player->SaveInventoryAndGoldToDB(trans);
        CharacterDatabase.CommitTransaction(trans);
        ++_report.gearToBags;
        return;
    }

    // Bags full (or not there to take it): Krook mails it, so nothing is ever lost.
    MailDraft(Acore::StringFormat("Your {}", proto->Name1),
              "This came off your mannequin while your bags were full (or you weren't there), so I sent it on. Krook")
        .AddItem(item)
        .SendMailTo(trans, MailReceiver(player ? player : ObjectAccessor::FindPlayerByLowGUID(ownerGuid), ownerGuid),
            MailSender(MAIL_CREATURE, _stewardEntry));
    CharacterDatabase.CommitTransaction(trans);
    ++_report.gearMailed;
}

void PlayerHousingMgr::LoadGear(ObjectGuid::LowType ownerGuid, std::map<uint32, Placement>& placements) const
{
    QueryResult result = CharacterDatabase.Query(
        "SELECT placement_id, slot, item_guid, item_entry FROM mod_playerhousing_placement_gear WHERE owner_guid={}", ownerGuid);
    if (!result)
        return;

    do
    {
        Field* fields = result->Fetch();
        auto itr = placements.find(fields[0].Get<uint32>());
        if (itr == placements.end())
            continue;  // its stand isn't loaded (a piece no longer defined): the row stays

        GearItem gear;
        gear.itemGuid = fields[2].Get<uint32>();
        gear.itemEntry = fields[3].Get<uint32>();
        itr->second.gear[fields[1].Get<uint8>()] = gear;
    } while (result->NextRow());
}

bool PlayerHousingMgr::SpawnStand(Session& session, Map* map, Placement const& placement)
{
    uint8 race = LookRace(placement.look);
    uint8 gender = LookGender(placement.look);
    ChrRacesEntry const* raceEntry = sChrRacesStore.LookupEntry(race);
    uint32 displayId = raceEntry ? (gender == GENDER_FEMALE ? raceEntry->model_f : raceEntry->model_m) : 49;

    Position position;
    position.Relocate(placement.x, placement.y, placement.z, placement.o);
    TempSummon* figure = map->SummonCreature(MANNEQUIN_ENTRY, position);
    if (!figure)
    {
        LOG_WARN("module", "mod-playerhousing: Failed to summon a mannequin (entry {}) for owner {}.", MANNEQUIN_ENTRY, session.ownerGuid);
        return false;
    }

    // Everything is set before the figure joins the island's phase, so the first thing any
    // viewer sees is the dressed figure.
    figure->SetDisplayId(displayId);
    figure->SetNativeDisplayId(displayId);
    figure->SetObjectScale(placement.scale);
    figure->SetUnitFlag2(UNIT_FLAG2_MIRROR_IMAGE);
    figure->SetReactState(REACT_PASSIVE);
    figure->SetControlled(true, UNIT_STATE_ROOT);
    // Held still like the figurines: no shifting weight or looking around.
    figure->AddAura(SPELL_FREEZE_ANIM, figure);
    uint8 virtualSlot = 0;
    for (uint8 slot : { EQUIPMENT_SLOT_MAINHAND, EQUIPMENT_SLOT_OFFHAND, EQUIPMENT_SLOT_RANGED })
    {
        auto itr = placement.gear.find(slot);
        figure->SetVirtualItem(virtualSlot++, itr != placement.gear.end() ? itr->second.itemEntry : 0);
    }
    figure->SetSheath(SHEATH_STATE_MELEE);

    MannequinLook look;
    look.displayId = displayId;
    look.race = race;
    look.gender = gender;
    Features features = LookFeatures(placement.look);
    look.skin = features.skin;
    look.face = features.face;
    look.hair = features.hair;
    look.hairColor = features.hairColor;
    look.facial = features.facial;
    for (size_t i = 0; i < std::size(MIRROR_SLOTS); ++i)
    {
        auto itr = placement.gear.find(MIRROR_SLOTS[i]);
        ItemTemplate const* proto = itr != placement.gear.end() ? sObjectMgr->GetItemTemplate(itr->second.itemEntry) : nullptr;
        look.displays[i] = proto ? proto->DisplayInfoID : 0;
    }
    _mannequins[figure->GetGUID()] = look;

    figure->SetPhaseMask(session.phaseMask, true);

    SpawnedPiece& spawned = session.spawned[placement.id];
    spawned.guid = figure->GetGUID();
    spawned.editCopy = false;
    return true;
}

// A figurine: its own creature (a copy of its boss, named after the figurine), shrunk to fit a
// table and frozen mid-pose.
bool PlayerHousingMgr::SpawnFigure(Session& session, Map* map, PieceDefinition const& piece, Placement const& placement)
{
    Position position;
    position.Relocate(placement.x, placement.y, placement.z, placement.o);
    TempSummon* figure = map->SummonCreature(piece.creatureEntry, position);
    if (!figure)
    {
        LOG_WARN("module", "mod-playerhousing: Failed to summon figurine {} (creature {}) for owner {}.", piece.name, piece.creatureEntry, session.ownerGuid);
        return false;
    }

    figure->SetObjectScale(placement.scale);
    figure->SetReactState(REACT_PASSIVE);
    figure->SetControlled(true, UNIT_STATE_ROOT);
    figure->SetDisableGravity(true);
    figure->AddAura(SPELL_FREEZE_ANIM, figure);
    figure->SetPhaseMask(session.phaseMask, true);

    SpawnedPiece& spawned = session.spawned[placement.id];
    spawned.guid = figure->GetGUID();
    spawned.editCopy = false;
    return true;
}

void PlayerHousingMgr::RemoveSpawned(Map* map, ObjectGuid const& guid)
{
    _mannequins.erase(guid);
    if (!map)
        return;

    if (guid.IsGameObject())
    {
        if (GameObject* object = map->GetGameObject(guid))
            object->AddObjectToRemoveList();
    }
    else if (Creature* creature = map->GetCreature(guid))
    {
        if (TempSummon* summon = creature->ToTempSummon())
            summon->UnSummon();
        else
            creature->AddObjectToRemoveList();
    }
}

bool PlayerHousingMgr::SendMannequinLook(WorldSession* session, ObjectGuid const& guid) const
{
    MannequinLook look;
    {
        std::lock_guard<std::recursive_mutex> guard(_lock);
        auto itr = _mannequins.find(guid);
        if (itr == _mannequins.end())
            return false;
        look = itr->second;
    }

    WorldPacket data(SMSG_MIRRORIMAGE_DATA, 68);
    data << guid;
    data << uint32(look.displayId);
    data << uint8(look.race);
    data << uint8(look.gender);
    data << uint8(CLASS_WARRIOR);
    data << uint8(look.skin);
    data << uint8(look.face);
    data << uint8(look.hair);
    data << uint8(look.hairColor);
    data << uint8(look.facial);
    data << uint32(0);  // guild
    for (uint32 display : look.displays)
        data << uint32(display);
    session->SendPacket(&data);
    return true;
}

bool PlayerHousingMgr::ChangeStand(Player* player, uint32 placementId, Placement const& after, std::string const& label, std::string& reason)
{
    Session* session = GetOwnerSession(player, reason);
    if (!session)
        return false;

    auto itr = session->placements.find(placementId);
    if (itr == session->placements.end())
    {
        reason = "That stand isn't here any more.";
        return false;
    }

    // Gear on a stand belongs to whoever placed the stand, so only they dress it.
    if (ItemOwnerOf(*session, itr->second) != HomeOf(player))
    {
        reason = Acore::StringFormat("Only {} can dress this mannequin: it holds their gear.", NameOf(ItemOwnerOf(*session, itr->second)));
        return false;
    }

    Placement before = itr->second;
    _report = {};
    std::string failure;
    if (!ApplyState(player, *session, player->GetMap(), placementId, after, failure))
    {
        reason = failure;
        return false;
    }

    // What really happened (an item may have gone missing), so undo returns exactly that.
    Placement applied = session->placements[placementId];
    Record(player, label, { Change{ placementId, before, applied } });
    reason = Capitalized(label) + "." + DescribeReturns();
    SendAddonState(player);
    return true;
}

bool PlayerHousingMgr::PutOnStand(Player* player, uint32 placementId, uint32 itemGuid, std::string& reason)
{
    std::lock_guard<std::recursive_mutex> guard(_lock);
    Session* session = GetOwnerSession(player, reason);
    if (!session)
        return false;

    auto itr = session->placements.find(placementId);
    PieceDefinition const* piece = itr != session->placements.end() ? GetPiece(itr->second.itemEntry) : nullptr;
    if (!piece || !piece->HasFlag(PIECE_FLAG_STAND))
    {
        reason = "That isn't a stand.";
        return false;
    }

    Item* item = player->GetItemByGuid(ItemGuid(itemGuid));
    if (!IsInBags(player, item))
    {
        reason = "That isn't in your bags any more.";
        return false;
    }

    int8 slot = StandSlotFor(item->GetTemplate(), itr->second.gear);
    if (slot < 0)
    {
        reason = "That doesn't show on a mannequin.";
        return false;
    }

    Placement after = itr->second;
    std::string label = Acore::StringFormat("put {} on the {}", item->GetTemplate()->Name1, piece->name);
    auto worn = after.gear.find(uint8(slot));
    if (worn != after.gear.end())
        label += Acore::StringFormat(" ({} came off)", StandItemName(worn->second.itemEntry));
    after.gear[uint8(slot)] = GearItem{ itemGuid, item->GetEntry() };
    return ChangeStand(player, placementId, after, label, reason);
}

bool PlayerHousingMgr::PutOnStandByEntry(Player* player, uint32 placementId, uint32 itemEntry, std::string& reason)
{
    for (Item* item : GetWearableItems(player))
        if (item->GetEntry() == itemEntry)
            return PutOnStand(player, placementId, item->GetGUID().GetCounter(), reason);
    reason = "That isn't in your bags any more.";
    return false;
}

std::string PlayerHousingMgr::DescribeStand(Player const* player, uint32 placementId) const
{
    std::optional<Placement> placement = GetPlacement(player, placementId);
    PieceDefinition const* piece = placement ? GetPiece(placement->itemEntry) : nullptr;
    if (!piece)
        return "";
    if (placement->gear.empty())
        return Acore::StringFormat("The {} isn't wearing anything yet.", piece->name);
    std::string worn;
    for (auto const& [slot, gear] : placement->gear)
        worn += (worn.empty() ? "" : ", ") + Acore::StringFormat("{} ({})", StandItemName(gear.itemEntry), StandSlotName(slot));
    return Acore::StringFormat("The {} wears {}.", piece->name, worn);
}

bool PlayerHousingMgr::TakeOffStand(Player* player, uint32 placementId, int32 slot, std::string& reason)
{
    std::lock_guard<std::recursive_mutex> guard(_lock);
    Session* session = GetOwnerSession(player, reason);
    if (!session)
        return false;

    auto itr = session->placements.find(placementId);
    PieceDefinition const* piece = itr != session->placements.end() ? GetPiece(itr->second.itemEntry) : nullptr;
    if (!piece || !piece->HasFlag(PIECE_FLAG_STAND))
    {
        reason = "That isn't a stand.";
        return false;
    }

    Placement after = itr->second;
    std::string label;
    if (slot < 0)
    {
        if (after.gear.empty())
        {
            reason = Acore::StringFormat("The {} isn't wearing anything.", piece->name);
            return false;
        }
        after.gear.clear();
        label = "took everything off the " + piece->name;
    }
    else
    {
        auto worn = after.gear.find(uint8(slot));
        if (worn == after.gear.end())
        {
            reason = Acore::StringFormat("Nothing on the {}'s {}.", piece->name, StandSlotName(uint8(slot)));
            return false;
        }
        label = Acore::StringFormat("took {} off the {}", StandItemName(worn->second.itemEntry), piece->name);
        after.gear.erase(worn);
    }
    return ChangeStand(player, placementId, after, label, reason);
}

bool PlayerHousingMgr::ChangeStandFigure(Player* player, uint32 placementId, std::string& reason)
{
    std::lock_guard<std::recursive_mutex> guard(_lock);
    Session* session = GetOwnerSession(player, reason);
    if (!session)
        return false;

    auto itr = session->placements.find(placementId);
    PieceDefinition const* piece = itr != session->placements.end() ? GetPiece(itr->second.itemEntry) : nullptr;
    if (!piece || !piece->HasFlag(PIECE_FLAG_STAND))
    {
        reason = "That isn't a stand.";
        return false;
    }

    // Next gender, then the next race.
    uint8 race = LookRace(itr->second.look);
    uint8 gender = LookGender(itr->second.look);
    if (gender == GENDER_MALE)
        gender = GENDER_FEMALE;
    else
    {
        gender = GENDER_MALE;
        size_t index = 0;
        while (index < std::size(RACES) && RACES[index] != race)
            ++index;
        race = RACES[(index + 1) % std::size(RACES)];
    }

    Placement after = itr->second;
    after.look = MakeLook(race, gender, 0, RandomFeatures(race, gender));
    return ChangeStand(player, placementId, after, Acore::StringFormat("made the {} a {}", piece->name, LookName(after.look)), reason);
}

bool PlayerHousingMgr::SetStandLook(Player* player, uint32 placementId, uint8 race, uint8 gender, std::string& reason)
{
    std::lock_guard<std::recursive_mutex> guard(_lock);
    Session* session = GetOwnerSession(player, reason);
    if (!session)
        return false;
    auto itr = session->placements.find(placementId);
    PieceDefinition const* piece = itr != session->placements.end() ? GetPiece(itr->second.itemEntry) : nullptr;
    if (!piece || !piece->HasFlag(PIECE_FLAG_STAND))
    {
        reason = "That isn't a mannequin.";
        return false;
    }
    if (LookRace(race) != race || race == 0)
    {
        reason = "Mannequins come in the playable races only.";
        return false;
    }
    gender = gender == GENDER_FEMALE ? GENDER_FEMALE : GENDER_MALE;
    Placement after = itr->second;
    after.look = MakeLook(race, gender, 0, RandomFeatures(race, gender));
    return ChangeStand(player, placementId, after, Acore::StringFormat("made the {} a {}", piece->name, LookName(after.look)), reason);
}

// Trading gear: what the mannequin wears goes on the character, and what the character wears
// (that a mannequin shows) goes on the mannequin, in one go. What the character can't wear
// goes to the bags (or by mail when they're full). One transaction, so nothing is ever in
// neither place.
bool PlayerHousingMgr::TradeStandGear(Player* player, uint32 placementId, std::string& reason)
{
    std::lock_guard<std::recursive_mutex> guard(_lock);
    Session* session = GetOwnerSession(player, reason);
    if (!session)
        return false;
    auto itr = session->placements.find(placementId);
    PieceDefinition const* piece = itr != session->placements.end() ? GetPiece(itr->second.itemEntry) : nullptr;
    if (!piece || !piece->HasFlag(PIECE_FLAG_STAND))
    {
        reason = "That isn't a mannequin.";
        return false;
    }
    if (ItemOwnerOf(*session, itr->second) != HomeOf(player))
    {
        reason = Acore::StringFormat("Only {} can dress this mannequin: it holds their gear.", NameOf(ItemOwnerOf(*session, itr->second)));
        return false;
    }
    if (!player->IsAlive() || player->IsInCombat())
    {
        reason = "Not now: trading gear waits until you're alive and out of combat.";
        return false;
    }

    // What the character wears that shows on a mannequin (rings, trinkets and relics stay).
    std::map<uint8, GearItem> none;
    std::vector<Item*> fromCharacter;
    for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
    {
        Item* worn = player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
        if (!worn || worn->IsInTrade() || StandSlotFor(worn->GetTemplate(), none) < 0)
            continue;
        if (player->CanUnequipItem(uint16(INVENTORY_SLOT_BAG_0) << 8 | slot, false) != EQUIP_ERR_OK)
            continue;
        fromCharacter.push_back(worn);
    }
    Placement before = itr->second;
    if (fromCharacter.empty() && before.gear.empty())
    {
        reason = Acore::StringFormat("Neither you nor the {} is wearing anything to trade.", piece->name);
        return false;
    }

    // The mannequin's gear, out of its rows.
    std::vector<std::pair<uint8, Item*>> fromStand;
    std::vector<std::string> missing;
    CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();
    trans->Append("DELETE FROM mod_playerhousing_placement_gear WHERE owner_guid={} AND placement_id={}", session->ownerGuid, placementId);
    for (auto const& [slot, gear] : before.gear)
    {
        if (Item* item = LoadGearItem(HomeOf(player), gear))
            fromStand.emplace_back(slot, item);
        else
            missing.push_back(StandItemName(gear.itemEntry));
    }

    // The character's gear onto the mannequin, in the slot it was worn in.
    Placement after = before;
    after.gear.clear();
    for (Item* worn : fromCharacter)
    {
        uint8 slot = worn->GetSlot();
        uint32 itemGuid = worn->GetGUID().GetCounter();
        uint32 entry = worn->GetEntry();
        player->MoveItemFromInventory(INVENTORY_SLOT_BAG_0, slot, true);
        worn->DeleteFromInventoryDB(trans);
        worn->SetState(ITEM_CHANGED);
        worn->SaveToDB(trans);
        trans->Append("REPLACE INTO mod_playerhousing_placement_gear (owner_guid, placement_id, slot, item_guid, item_entry) VALUES ({}, {}, {}, {}, {})",
            session->ownerGuid, placementId, uint32(slot), itemGuid, entry);
        after.gear[slot] = GearItem{ itemGuid, entry };
        delete worn;
    }
    player->AutoUnequipOffhandIfNeed();

    // The mannequin's gear onto the character: the slot it had, else any it fits, else the
    // bags, else the mail.
    uint32 toBags = 0;
    uint32 mailed = 0;
    for (auto const& [slot, item] : fromStand)
    {
        item->SetOwnerGUID(player->GetGUID());
        uint16 dest = 0;
        if (player->CanEquipItem(slot, dest, item, false) == EQUIP_ERR_OK
            || player->CanEquipItem(NULL_SLOT, dest, item, false) == EQUIP_ERR_OK)
        {
            item->SetState(ITEM_UNCHANGED);
            item->SetState(ITEM_NEW, player);  // a new row in the character's inventory
            player->EquipItem(dest, item, true);
            continue;
        }
        ItemPosCountVec bagDest;
        if (player->CanStoreItem(NULL_BAG, NULL_SLOT, bagDest, item, false) == EQUIP_ERR_OK)
        {
            item->SetState(ITEM_UNCHANGED);
            player->MoveItemToInventory(bagDest, item, true);
            ++toBags;
            continue;
        }
        ItemTemplate const* proto = item->GetTemplate();
        MailDraft(Acore::StringFormat("Your {}", proto->Name1),
                  "This came off your mannequin while your bags were full, so I sent it on. Krook")
            .AddItem(item)
            .SendMailTo(trans, MailReceiver(player, player->GetGUID().GetCounter()), MailSender(MAIL_CREATURE, _stewardEntry));
        ++mailed;
    }
    player->AutoUnequipOffhandIfNeed();
    player->SaveInventoryAndGoldToDB(trans);
    CharacterDatabase.CommitTransaction(trans);

    itr->second = after;
    SavePlacement(session->ownerGuid, after, session->mapId);
    RespawnPlacement(*session, player->GetMap(), placementId);
    session->selected[player->GetGUID().GetCounter()] = placementId;

    reason = Acore::StringFormat("Traded gear with the {}.", piece->name);
    if (toBags)
        reason += Acore::StringFormat(" {} you can't wear went to your bags.", toBags == 1 ? "One piece" : Acore::StringFormat("{} pieces", toBags));
    if (mailed)
        reason += Acore::StringFormat(" {} went by mail (your bags are full).", mailed == 1 ? "One piece" : Acore::StringFormat("{} pieces", mailed));
    for (std::string const& name : missing)
        reason += Acore::StringFormat(" {} was missing.", name);
    SendAddonState(player);
    return true;
}
