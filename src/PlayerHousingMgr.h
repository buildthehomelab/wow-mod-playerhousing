#ifndef MOD_PLAYERHOUSING_MGR_H
#define MOD_PLAYERHOUSING_MGR_H

#include "Define.h"
#include "ObjectGuid.h"
#include "Position.h"

#include <array>
#include <atomic>
#include <cmath>
#include <ctime>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

class GameObject;
class Item;
class Map;
class Player;
class WorldObject;
class WorldSession;
struct ItemTemplate;

namespace Housing
{
    // Tilts a game object. Older cores call this SetLocalRotationAngles, newer ones
    // SetWorldRotationAngles; whichever the core has is used.
    template <typename Object>
    auto SetRotationAngles(Object* object, float z, float y, float x, int) -> decltype(object->SetLocalRotationAngles(z, y, x), void())
    {
        object->SetLocalRotationAngles(z, y, x);
    }

    template <typename Object>
    auto SetRotationAngles(Object* object, float z, float y, float x, long) -> decltype(object->SetWorldRotationAngles(z, y, x), void())
    {
        object->SetWorldRotationAngles(z, y, x);
    }

    // The core takes a rotation with no z and w part for "no rotation data" and stands the
    // object up again (GameObject::SetLocalRotation). Turned right over (180 degrees about a
    // level line: upside down), a rotation has exactly that, so it's tipped 0.06 degrees off.
    template <typename Object>
    void SetRotationAngles(Object* object, float z, float y, float x)
    {
        float cz = std::cos(z / 2), sz = std::sin(z / 2), cy = std::cos(y / 2), sy = std::sin(y / 2);
        float cx = std::cos(x / 2), sx = std::sin(x / 2);
        float qz = sz * cy * cx - cz * sy * sx;
        float qw = cz * cy * cx + sz * sy * sx;
        if (std::fabs(qz) < 0.0001f && std::fabs(qw) < 0.0001f)
            y += 0.001f;
        SetRotationAngles(object, z, y, x, 0);
    }

    // Items, objects and texts owned by the module (see sql/db_world).
    constexpr uint32 HOUSE_KEY_ITEM = 902000;
    constexpr uint32 KEY_SPELL = 18282;        // "Dummy Spell": lets the House Key be used
    constexpr uint32 MANNEQUIN_ENTRY = 900201;  // the figure that shows a stand's gear
    constexpr uint32 CHEST_BANKER_ENTRY = 900202;  // unseen banker at an opened Bank Chest
    constexpr uint32 HIGHLIGHT_RING_GO = 903991;  // under the piece chosen in the Placed list
    constexpr uint32 GUILD_HALL_GO = 903992;      // GM Island's hall on the regular map (world SQL)
    constexpr uint32 SPELL_FREEZE_ANIM = 16245;     // holds a figurine still, mid-pose
    constexpr uint32 GHOST_ENTRY = 900203;          // a see-through piece following its player
    // Each piece's ghost model (tools/content/build_content.py): its see-through creature
    // display is this plus the item's offset from 900000 (the client patch adds them).
    constexpr uint32 GHOST_DISPLAY_BASE = 60000;

    // The equipment slots a mirror image shows (EquipmentSlots), in the order of SMSG_MIRRORIMAGE_DATA:
    // head, shoulders, shirt, chest, waist, legs, feet, wrists, hands, back, tabard.
    constexpr uint8 MIRROR_SLOTS[] = { 0, 2, 3, 4, 5, 6, 7, 8, 9, 14, 18 };

    // Krook's welcome tour: each quest completes when the player does the thing.
    constexpr uint32 QUEST_TOUR_HOME = 900400;
    constexpr uint32 QUEST_TOUR_PLACE = 900401;
    constexpr uint32 QUEST_TOUR_CHANGE = 900402;
    constexpr uint32 QUEST_TOUR_UNDO = 900403;
    constexpr uint32 QUEST_TOUR_OPEN = 900404;
    // The old "Move a Piece" items (targeting circles), retired: any still around are taken
    // away (see SweepHousingItems).
    constexpr uint32 MOVER_ITEM_FIRST = 901190;
    constexpr uint32 MOVER_ITEM_LAST = 901199;

    constexpr uint32 TEXT_HOME = 900300;  // Krook's greeting above his quests
    constexpr uint32 TEXT_NO_KEY = 900301; // Krook's greeting to someone without a House Key

    enum PieceKind : uint8
    {
        PIECE_FURNISHING = 0,
        PIECE_BUILDING = 1
    };

    enum PieceFlags : uint32
    {
        PIECE_FLAG_SURFACE = 0x01,        // other pieces can be put on top
        PIECE_FLAG_SMALL = 0x02,          // small enough for a table top
        PIECE_FLAG_PER_CHARACTER = 0x04,  // unlock belongs to the character, not the account
        PIECE_FLAG_GIFT = 0x08,           // given on first login
        PIECE_FLAG_WRECKAGE = 0x10,       // standing on the island at the first visit
        PIECE_FLAG_STAND = 0x20,          // a mannequin that wears real gear from the bags
        PIECE_FLAG_CHEST = 0x40,          // opens its owner's bank
        PIECE_FLAG_MUSIC = 0x80,          // a music box: plays the island's music
        PIECE_FLAG_FIGURE = 0x100         // a figurine: a creature's model, frozen and small
    };

    enum Category : uint8
    {
        CATEGORY_STARTER = 0,
        CATEGORY_BUILDINGS,
        CATEGORY_EXPLORATION,
        CATEGORY_DUNGEONS,
        CATEGORY_RAIDS,
        CATEGORY_REPUTATION,
        CATEGORY_PROFESSIONS,
        CATEGORY_HOLIDAYS,
        CATEGORY_CAPSTONES,
        CATEGORY_FIGURINES,
        CATEGORY_CATALOG,   // every other object (PlayerHousing.Catalog = everything)
        CATEGORY_COUNT
    };

    enum RuleType : uint8
    {
        RULE_LEVEL = 1,        // param1 level
        RULE_ACHIEVEMENT = 2,  // param1 achievement id
        RULE_REPUTATION = 3,   // param1 faction id, param2 rank (7 = Exalted)
        RULE_QUEST = 4,        // param1 quest id (rewarded)
        RULE_KILL = 5,         // param1 creature entry (killer and group members nearby)
        RULE_EXPLORE = 6,      // param1 area or zone id
        RULE_SKILL = 7,        // param1 skill id, param2 value
        RULE_NEVER = 8         // only through a GM or UnlockAll
    };

    enum Privacy : uint8
    {
        PRIVACY_PUBLIC = 0,
        PRIVACY_PRIVATE = 1,
        PRIVACY_FRIENDS = 2
    };

    enum HouseFlags : uint32
    {
        HOUSE_FLAG_WRECKAGE_PLACED = 0x01,
        HOUSE_FLAG_HALL_NOTICE = 0x02,     // things from the old guild hall went to storage
        HOUSE_FLAG_LAYOUT_COPYABLE = 0x04, // visitors may save a copy of the layout
        HOUSE_FLAG_HIDDEN = 0x08           // a GM closed it to all but its guests
    };

    // Flags 0x08, 0x10, 0x20 and 0x40 belonged to the retired menus (when a piece's menu
    // opened by itself, the menu Collection's filter); they're left alone in old rows.
    enum CharacterFlags : uint32
    {
        CHAR_FLAG_KEY_GIVEN = 0x01,
        CHAR_FLAG_VETERAN_DONE = 0x02,
        CHAR_FLAG_GREETED = 0x04,
        CHAR_FLAG_HOME_AT_LOGOUT = 0x80,  // last seen on their own island: logging in puts them back
        CHAR_FLAG_GIFTS_GIVEN = 0x100     // on the account's home character: its first pieces came
    };

    // 0x04 was the first trip to House Storage, when pieces still went to the bags.
    enum Tip : uint32
    {
        TIP_FIRST_PLACE = 0x01,
        TIP_FIRST_UNLOCK = 0x02,
        TIP_LIMIT = 0x08,
        TIP_DECORATE = 0x10,
        TIP_GHOST = 0x20
    };

    // Rules in the same group must all be met; any complete group unlocks the piece (so a
    // raid trophy can come from the 10-player or the 25-player achievement).
    struct PieceRule
    {
        uint8 group{0};
        uint8 type{0};
        uint32 param1{0};
        uint32 param2{0};
    };

    struct PieceDefinition
    {
        uint32 itemEntry{0};
        uint8 kind{PIECE_FURNISHING};
        uint8 category{CATEGORY_STARTER};
        std::string name;
        uint32 goEntry{0};
        uint32 editGoEntry{0};
        uint32 creatureEntry{0};  // figurines
        float scale{1.0f};
        float footprint{1.0f};
        float height{1.0f};
        uint32 flags{0};
        uint32 copyCost{0};
        uint32 sortOrder{0};
        std::string hint;
        uint32 legacyCatalogId{0};
        // The rectangle on the ground in the piece's own frame (x forward): inside a building.
        float outlineMinX{0.0f};
        float outlineMinY{0.0f};
        float outlineMaxX{0.0f};
        float outlineMaxY{0.0f};
        std::vector<PieceRule> rules;

        bool IsBuilding() const { return kind == PIECE_BUILDING; }
        bool HasOutline() const { return outlineMaxX > outlineMinX && outlineMaxY > outlineMinY; }
        bool HasFlag(uint32 flag) const { return (flags & flag) != 0; }
        // Shown as a creature (mannequins, figurines): no tilting.
        bool IsCreature() const { return HasFlag(PIECE_FLAG_STAND) || HasFlag(PIECE_FLAG_FIGURE); }
    };

    struct LayoutDefinition
    {
        std::string code;
        uint32 mapId{1};
        Position landing;
        float stewardOffsetX{7.0f};
        float stewardOffsetY{2.0f};
        float centerX{0.0f};
        float centerY{0.0f};
        float radius{250.0f};
    };

    // An item on a stand. It keeps its own guid (and so its enchants and gems) while it's
    // there: it leaves the owner's inventory but stays in item_instance, like mail.
    struct GearItem
    {
        uint32 itemGuid{0};
        uint32 itemEntry{0};
    };

    struct Placement
    {
        uint32 id{0};
        uint32 itemEntry{0};
        float x{0.0f};
        float y{0.0f};
        float z{0.0f};
        float o{0.0f};
        float scale{1.0f};
        float pitch{0.0f};                // tilt, radians: + tips its front down
        float roll{0.0f};                 // tilt, radians: + leans it to its right
        uint64 look{0};                   // stands: race, pose, gender and looks (HousingStands.cpp)
        uint32 parent{0};                 // the surface it stands on: it moves with it
        uint32 placedBy{0};               // a roommate who placed it (0: the island's owner); it goes back to them
        std::map<uint8, GearItem> gear;   // stands: equipment slot -> item
    };

    // One undoable step: every placement it touched, as it was before and after.
    struct Change
    {
        uint32 placementId{0};
        std::optional<Placement> before;
        std::optional<Placement> after;
    };

    struct JournalEntry
    {
        std::string label;
        std::vector<Change> changes;
        uint64 at{0};            // game time (ms) of its last change
        bool mergeable{false};   // edit mode: the next small move of the same pieces joins it
    };

    struct HouseRecord
    {
        ObjectGuid::LowType ownerGuid{0};
        uint8 privacy{PRIVACY_PRIVATE};
        uint32 flags{0};
        std::string greeting;
        uint8 weather{0};    // PlayerHousingMgr::WeatherName
        uint8 timeOfDay{0};  // PlayerHousingMgr::TimeOfDayName; 0 follows the server's clock
        uint32 music{0};     // SoundEntries id, 0 for none
        bool hasDoor{false}; // visitors arrive at the door instead of the landing spot
        Position door;
    };

    // A note a visitor left in an island's guestbook.
    struct GuestbookNote
    {
        uint32 id{0};
        std::string author;
        std::string when;    // YYYY-MM-DD HH:MM
        std::string text;
        bool fresh{false};   // the owner hasn't read it yet
    };

    // A few pieces saved together, to set down anywhere (they follow the player like a piece).
    struct SavedSet
    {
        uint32 id{0};
        std::string name;
        uint32 pieces{0};
        std::string savedAt;  // YYYY-MM-DD
    };

    struct IslandReport
    {
        uint32 id{0};
        ObjectGuid::LowType ownerGuid{0};
        ObjectGuid::LowType reporterGuid{0};
        std::string reason;
        std::string when;
        bool closed{false};
    };

    struct SavedLayout
    {
        uint32 id{0};
        std::string name;
        std::string source;   // who it came from, when sent or copied
        std::string savedAt;  // YYYY-MM-DD
        uint32 pieces{0};
    };

    struct VisitEntry
    {
        ObjectGuid::LowType ownerGuid{0};
        std::string ownerName;
        bool roommate{false};  // guest lists: may decorate
        uint32 likes{0};       // the most liked list
    };
}

class PlayerHousingMgr
{
public:
    // What a mannequin wears, for the mirror image data its viewers ask for.
    struct MannequinLook
    {
        uint32 displayId{0};
        uint8 race{1};
        uint8 gender{0};
        uint8 classId{1};
        uint8 skin{0};
        uint8 face{0};
        uint8 hair{0};
        uint8 hairColor{0};
        uint8 facial{0};
        std::array<uint32, 11> displays{};  // in the order of Housing::MIRROR_SLOTS
    };

    static PlayerHousingMgr* instance();

    // ---- lifecycle and player hooks (PlayerHousingMgr.cpp)
    void OnStartup();
    void OnPlayerLogin(Player* player);
    void OnPlayerLogout(Player* player);
    void OnPlayerUpdate(Player* player, uint32 diffMs);
    void OnPlayerMapChanged(Player* player);
    void OnPlayerDelete(ObjectGuid guid);
    // The character is gone for good (not only deleted to be restorable): its housing goes too.
    void OnPlayerDeleteFromDB(ObjectGuid::LowType guid);
    void OnBeforeSetPhaseMask(uint32 oldPhaseMask, uint32 newPhaseMask, bool& useCombinedPhases) const;

    bool IsEnabled() const { return _enabled; }
    bool IsFreeMode() const { return _freeMode; }
    bool IsUnlockAll() const { return _unlockAll; }
    uint32 GetStewardEntry() const { return _stewardEntry; }
    uint32 GetStewardDisplayId() const { return _stewardDisplayId; }
    bool IsStewardEntry(uint32 entry) const { return _enabled && entry == _stewardEntry; }
    static bool IsHousingPhase(uint32 phaseMask);
    static uint32 PhaseOfOwner(ObjectGuid::LowType ownerGuid);

    // ---- travel (PlayerHousingMgr.cpp)
    bool EnterOwnHouse(Player* player, std::string& reason);
    bool VisitHouse(Player* player, ObjectGuid::LowType ownerGuid, std::string& reason);
    bool VisitHouseByName(Player* player, std::string const& ownerName, std::string& reason);
    bool RequestGoHome(Player* player, std::string& reason);
    bool LeaveHouse(Player* player, std::string& reason);
    bool Unstuck(Player* player, std::string& reason);
    // Krook comes over to the player (the window's Call Krook), on their island.
    bool CallSteward(Player* player, std::string& reason);
    bool GiveHouseKey(Player* player, std::string& reason);
    // Whether Krook (or the old login gift) ever gave this character a House Key.
    bool HadHouseKey(Player const* player) const;
    // Krook hands over a House Key: the first on the account comes with its first pieces.
    void GiveKeyFromKrook(Player* player);

    // Where the player stands: 0 when not on anyone's island.
    ObjectGuid::LowType GetIslandOwner(Player const* player) const;
    bool IsOnOwnIsland(Player const* player) const;
    // On their own island, or on one where they're a roommate.
    bool CanDecorate(Player const* player) const;
    bool IsRoommate(ObjectGuid::LowType ownerGuid, ObjectGuid::LowType guid) const;
    bool SetRoommate(Player* owner, ObjectGuid::LowType guestGuid, bool roommate, std::string& reason);
    bool IsDecorating(Player const* player) const;

    // ---- pieces and editing (HousingPieces.cpp)
    Housing::PieceDefinition const* GetPiece(uint32 itemEntry) const;
    bool SetDecorating(Player* player, bool on, std::string& reason);
    bool PickUp(Player* player, uint32 placementId, bool withInside, std::string& reason);
    bool Rotate(Player* player, uint32 placementId, float degrees, std::string& reason);
    bool Nudge(Player* player, uint32 placementId, float forward, float left, float up, std::string& reason);
    // Edit mode (the client addon): keys move the selected piece. Moves and turns in one go,
    // relative to the player's facing; a quick run of them on one piece is a single undo step.
    bool Shift(Player* player, uint32 placementId, float forward, float left, float up, float degrees, std::string& reason);
    bool SetEditMode(Player* player, bool on, std::string& reason);
    bool IsInEditMode(Player const* player) const;
    // The next (or previous) piece nearby after the selected one, by distance; 0 if none.
    uint32 SelectNext(Player* player, bool backwards, std::string& reason);
    bool FaceMe(Player* player, uint32 placementId, std::string& reason);
    bool MoveHere(Player* player, uint32 placementId, std::string& reason);
    // Percent of the piece's normal size, or (relative) percentage points more or less.
    bool Resize(Player* player, uint32 placementId, float percent, bool relative, std::string& reason);
    // Degrees; straighten sets both back to level.
    bool Tilt(Player* player, uint32 placementId, float forwardDegrees, float rightDegrees, bool straighten, std::string& reason);
    bool PlaceAnother(Player* player, uint32 placementId, std::string& reason);
    // Grid snapping, in yards (0: off).
    float GetGridSize(ObjectGuid::LowType guid) const;
    void SetGridSize(Player* player, float yards, std::string& reason) const;
    float GetMinSize() const { return _sizeMin; }
    float GetMaxSize() const { return _sizeMax; }
    float GetMaxTilt() const { return _tiltMax; }
    static bool IsMoverItem(uint32 itemEntry) { return itemEntry >= Housing::MOVER_ITEM_FIRST && itemEntry <= Housing::MOVER_ITEM_LAST; }
    bool PackUpEverything(Player* player, std::string& reason);
    // A Bank Chest: the owner's bank, through a banker standing unseen at the chest.
    bool OpenBankAtChest(Player* player, uint32 placementId, std::string& reason);
    // cooldown false: part of undoing several steps at once, which checked it once.
    bool Undo(Player* player, std::string& reason, bool cooldown = true);
    bool Redo(Player* player, std::string& reason);
    std::string UndoLabel(Player const* player) const;
    std::string RedoLabel(Player const* player) const;
    uint32 GetSelectedPlacement(Player const* player) const;
    uint32 ResolvePlacementArgument(Player* player, uint32 placementId) const;
    std::optional<Housing::Placement> GetPlacement(Player const* player, uint32 placementId) const;
    uint32 GetPlacementForObject(Player const* player, ObjectGuid const& guid) const;
    // A green ring under the piece chosen in the Placed list, to show which one it is (0: none).
    void HighlightPlacement(Player* player, uint32 placementId);
    std::vector<std::pair<Housing::Placement, float>> GetNearbyPlacements(Player const* player, float range) const;
    std::vector<Housing::Placement> GetPiecesInside(ObjectGuid::LowType ownerGuid, uint32 buildingPlacementId) const;
    void CountPlaced(ObjectGuid::LowType ownerGuid, uint32& furnishings, uint32& buildings) const;
    uint32 GetMaxFurnishings() const { return _maxFurnishings; }
    uint32 GetMaxBuildings() const { return _maxBuildings; }
    // The pieces a character owns that aren't placed, by item entry: their Collection's counts
    // (kept in mod_playerhousing_storage, once called House Storage).
    std::map<uint32, uint32> GetStorage(ObjectGuid::LowType ownerGuid) const;
    // Furnishing items from before pieces were kept as counts (in the bags, the bank, back from
    // the mail) go into the Collection's counts; old "Move a Piece" items go. True if any did.
    bool SweepHousingItems(Player* player, bool bankToo);
    std::string CountsText(ObjectGuid::LowType ownerGuid) const;
    void SelectPlacement(Player const* player, uint32 placementId);
    ObjectGuid GetObjectForPlacement(Player const* player, uint32 placementId) const;

    // ---- groups, rows, sets and helpers (HousingGroups.cpp)
    // Several pieces selected at once: the selected piece first, then the others, which
    // move, turn and get picked up with it. A plain click selects one piece again.
    std::vector<uint32> GetGroup(Player const* player) const;
    // Ctrl-click (the addon says when Ctrl is down): in or out of the group.
    bool ToggleGroupMember(Player* player, uint32 placementId, std::string& reason);
    bool SetGroupMember(Player* player, uint32 placementId, bool member, std::string& reason);
    void ClearGroup(Player* player);
    void SetGroupHold(Player* player, bool held);
    bool IsGroupHold(Player const* player) const;
    // count copies of a piece in a row next to it, spacing yards apart (0: its own length),
    // toward the player's right, left, forward or back.
    bool PlaceRow(Player* player, uint32 placementId, uint32 count, float spacing, std::string const& direction, std::string& reason);
    // The group lines up with its first piece: height, turn, line (a straight row across
    // the player's view) or space (evenly along that row).
    bool MatchGroup(Player* player, std::string const& how, std::string& reason);
    std::vector<Housing::SavedSet> GetSavedSets(ObjectGuid::LowType ownerGuid) const;
    std::optional<Housing::SavedSet> FindSavedSet(ObjectGuid::LowType ownerGuid, std::string const& nameOrNumber) const;
    uint32 GetMaxSavedSets() const { return MAX_SAVED_SETS; }
    bool SaveSet(Player* player, std::string const& name, std::string& reason);
    bool DeleteSet(Player* player, uint32 setId, std::string& reason);
    // Undo several steps at once, newest first.
    bool UndoSteps(Player* player, uint32 steps, std::string& reason);
    std::vector<std::string> JournalLabels(Player const* player, bool redo, uint32 limit) const;
    // Walks the player over to a piece on the island, and selects it.
    bool GoTo(Player* player, uint32 placementId, std::string& reason);
    // GM: a building set up in front of the camera, one after another, for the addon's
    // pictures. what: start, next, stop, or an item entry.
    bool PhotoTour(Player* gm, std::string const& what, std::string& reason);

    // ---- ghosts (HousingGhosts.cpp): a piece that follows its player until it's set down.
    // Furniture and M2 buildings show as exact see-through copies (creatures with their model,
    // from the client patch). A world-model building, or any piece without a creature ghost,
    // is carried as its real collisionless object.
    // A new piece, one the player owns (or a new copy from the Collection, paid for when it's
    // set down); with copyOf, it takes after that placed piece (turn, size, tilt).
    bool StartGhostNew(Player* player, uint32 itemEntry, uint32 copyOf, std::string& reason);
    // Pieces already placed (0: the selection, with the rest of its group).
    bool StartGhostMove(Player* player, uint32 placementId, std::string& reason);
    // A saved set, all its pieces following together, set down as one.
    bool StartGhostSet(Player* player, uint32 setId, std::string& reason);
    // Farther (forward), to the left, up, and turned (degrees), from where the player faces.
    bool AdjustGhost(Player* player, float forward, float left, float up, float degrees, std::string& reason);
    // Size and tilt of what follows the player, set with it when it's set down (Resize and
    // Tilt come here while something follows).
    bool ResizeGhost(Player* player, float percent, bool relative, std::string& reason);
    bool TiltGhost(Player* player, float forwardDegrees, float rightDegrees, bool straighten, std::string& reason);
    // Shown at a point in the world from now on: where the mouse points (the addon with
    // PlayerHousing.dll sends it a few times a second). facing: which way the surface there
    // faces (x, y, z), or null; on a wall, the piece faces out from it.
    bool GhostAt(Player* player, float x, float y, float z, float const* facing, std::string& reason);
    // Why the ghost isn't where the mouse points ("off your island"), or empty.
    std::string GetGhostNote(Player const* player) const;
    // Sets it down where it's shown; another: then a new one of the same, if there is one.
    bool PlaceGhost(Player* player, bool another, std::string& reason);
    void CancelGhost(Player* player);
    uint32 GetGhostItem(Player const* player) const;  // 0: not carrying anything
    bool IsGhostMove(Player const* player) const;
    // One island per account: the character it's kept under (HousingAccounts.cpp). Every
    // character of the account is at home on it.
    ObjectGuid::LowType HomeOf(Player const* player) const;
    ObjectGuid::LowType HomeOfCharacter(ObjectGuid::LowType guid) const;
    // The island's owner in the game now, on any of the account's characters.
    Player* FindOwnerOnline(ObjectGuid::LowType home) const;
    // Something follows the player (placing or moving), until it's set down.
    bool IsCarrying(Player const* player) const;
    void UpdateGhost(Player* player);
    bool GhostsEnabled() const { return _ghosts; }

    // ---- the client addon's housing window (HousingAddon.cpp): the only way into housing
    // besides typing .house commands.
    void SendAddon(Player* player, std::string const& text) const;
    // A list for one of the window's tabs: collection, placed, layouts, guests, visits, island,
    // history, sets, guestbook, stand <placement>.
    bool SendAddonData(Player* player, std::string const& kind, std::string const& argument, std::string& reason);
    // The addon says it's there; with mouse, PlayerHousing.dll is there too (ghosts follow the
    // mouse); with local, the DLL moves the ghost on the player's screen itself, so the server
    // only keeps up with where it is instead of moving it there.
    void SetAddonClient(Player* player, bool mouse, bool local);
    bool HasMouse(Player const* player) const;
    bool HasLocalGhosts(Player const* player) const;
    // The addon is there: its window and banner show what just happened, so chat needn't.
    bool HasAddon(Player const* player) const;
    // Opens the window (on a tab, or as it was), or says how to get the addon without it.
    void OpenWindow(Player* player, std::string const& tab = "") const;
    void MarkAllSeen(Player* player) const;

    // ---- moderation (HousingModeration.cpp)
    bool ReportIsland(Player* reporter, std::string const& text, std::string& reason);
    std::vector<Housing::IslandReport> GetReports(bool includeClosed, uint32 limit) const;
    bool CloseReport(Player* gm, uint32 reportId, std::string& reason);
    bool GmInspect(Player* gm, ObjectGuid::LowType ownerGuid, std::string& reason);
    bool GmClearGreeting(Player* gm, ObjectGuid::LowType ownerGuid, std::string& reason);
    bool GmSetHidden(Player* gm, ObjectGuid::LowType ownerGuid, bool hidden, std::string& reason);
    bool GmPackUp(Player* gm, ObjectGuid::LowType ownerGuid, std::string& reason);

    // ---- saved layouts (HousingLayouts.cpp)
    std::vector<Housing::SavedLayout> GetSavedLayouts(ObjectGuid::LowType ownerGuid) const;
    std::optional<Housing::SavedLayout> GetSavedLayout(ObjectGuid::LowType ownerGuid, uint32 layoutId) const;
    std::optional<Housing::SavedLayout> FindSavedLayout(ObjectGuid::LowType ownerGuid, std::string const& nameOrNumber) const;
    uint32 GetMaxSavedLayouts() const { return _maxSavedLayouts; }
    // layoutId 0: a new layout with this name; otherwise save the island over that one.
    bool SaveLayout(Player* player, uint32 layoutId, std::string const& name, std::string& reason);
    bool RenameLayout(Player* player, uint32 layoutId, std::string const& name, std::string& reason);
    bool DeleteLayout(Player* player, uint32 layoutId, std::string& reason);
    bool SwitchLayout(Player* player, uint32 layoutId, std::string& reason);
    // Pieces the layout needs that the player doesn't have anywhere: item entry -> how many.
    std::map<uint32, uint32> LayoutShortfall(Player* player, uint32 layoutId) const;
    void DescribeShortfall(Player* player, std::map<uint32, uint32> const& missing, uint32& gettable, uint64& cost, uint32& locked) const;
    bool GetMissingForLayout(Player* player, uint32 layoutId, std::string& reason);
    bool IsLayoutCopyable(ObjectGuid::LowType ownerGuid) const;
    void SetLayoutCopyable(Player* player, bool copyable, std::string& reason) const;
    bool CopyIslandLayout(Player* visitor, std::string& reason);
    bool SendLayout(Player* player, uint32 layoutId, std::string const& recipientName, std::string& reason);

    // ---- ambience (HousingAmbience.cpp)
    static uint8 WeatherCount();
    static char const* WeatherName(uint8 weather);
    static uint8 TimeOfDayCount();
    static char const* TimeOfDayName(uint8 timeOfDay);
    static std::vector<std::pair<uint32, char const*>> const& MusicTracks();
    static char const* MusicName(uint32 soundId);
    bool SetWeather(Player* player, uint8 weather, std::string& reason);
    bool SetTimeOfDay(Player* player, uint8 timeOfDay, std::string& reason);
    bool SetMusic(Player* player, uint32 soundId, std::string& reason);
    bool HasMusicBox(ObjectGuid::LowType ownerGuid) const;

    // ---- stands (HousingStands.cpp)
    static int8 StandSlotFor(ItemTemplate const* proto, std::map<uint8, Housing::GearItem> const& worn);
    static char const* StandSlotName(uint8 slot);
    static std::string StandItemName(uint32 itemEntry);
    static std::string LookName(uint64 look);
    std::vector<Item*> GetWearableItems(Player* player) const;
    bool PutOnStand(Player* player, uint32 placementId, uint32 itemGuid, std::string& reason);
    // The first wearable one of that item in the bags (the addon only knows items by entry).
    bool PutOnStandByEntry(Player* player, uint32 placementId, uint32 itemEntry, std::string& reason);
    // What a stand wears, for a visitor.
    std::string DescribeStand(Player const* player, uint32 placementId) const;
    bool TakeOffStand(Player* player, uint32 placementId, int32 slot, std::string& reason);
    bool ChangeStandFigure(Player* player, uint32 placementId, std::string& reason);
    // A race and gender, with a random look one could be made with.
    bool SetStandLook(Player* player, uint32 placementId, uint8 race, uint8 gender, std::string& reason);
    // What the mannequin wears onto the character, and what the character wears onto it.
    bool TradeStandGear(Player* player, uint32 placementId, std::string& reason);
    bool SendMannequinLook(WorldSession* session, ObjectGuid const& guid) const;
    // For figures spawned outside a house (the home screen): the look its viewers get, and taking
    // it away again with the figure.
    void SetFigureLook(ObjectGuid const& guid, MannequinLook const& look);
    void RemoveFigure(Map* map, ObjectGuid const& guid);

    // ---- collection (HousingCollection.cpp)
    bool IsUnlocked(Player const* player, Housing::PieceDefinition const& piece, std::set<uint32> const* known = nullptr) const;
    std::set<uint32> LoadUnlocks(Player const* player) const;
    std::string DescribeProgress(Player const* player, Housing::PieceDefinition const& piece) const;
    void CollectionCounts(Player const* player, int32 category, uint32& unlocked, uint32& total, std::set<uint32> const* known = nullptr) const;
    // Copies of an unlocked piece, bought into the Collection (free in FreeMode).
    bool GetCopies(Player* player, uint32 itemEntry, uint32 count, std::string& reason);
    // Unlocked since the player last saw them listed.
    std::set<uint32> LoadNewUnlocks(Player const* player) const;
    void MarkSeen(Player const* player, std::vector<uint32> const& itemEntries) const;
    bool GetOneOfEverything(Player* player, std::string& reason);
    void EvaluateUnlocks(Player* player, uint8 ruleType, uint32 param, bool announce, uint32 value = 0);
    uint32 CreditPastProgress(Player* player);
    bool GmUnlock(Player* target, std::string const& what, bool unlock, std::string& reason);
    void OnCreatureKilled(Player* killer, uint32 creatureEntry);

    // ---- people (HousingPeople.cpp)
    bool GetHouseRecord(ObjectGuid::LowType ownerGuid, Housing::HouseRecord& outRecord) const;
    bool EnsureHouse(ObjectGuid::LowType ownerGuid) const;
    // Who has the visitor on a guest list or a friends list, looked up once for a whole list.
    struct VisitContext
    {
        std::unordered_set<ObjectGuid::LowType> guestOf;
        std::unordered_set<ObjectGuid::LowType> friendOf;
    };
    bool CanVisit(Player const* visitor, Housing::HouseRecord const& house, std::string& reason, VisitContext const* context = nullptr) const;
    bool SetPrivacy(Player* player, uint8 privacy, std::string& reason);
    bool CyclePrivacy(Player* player, std::string& reason);
    bool InviteGuest(Player* player, ObjectGuid::LowType guestGuid, std::string& reason);
    bool InviteGuestByName(Player* player, std::string const& name, std::string& reason);
    bool InviteTarget(Player* player, std::string& reason);
    bool InviteParty(Player* player, std::string& reason);
    bool RemoveGuest(Player* player, ObjectGuid::LowType guestGuid, std::string& reason);
    bool RemoveGuestByName(Player* player, std::string const& name, std::string& reason);
    std::vector<Housing::VisitEntry> GetGuests(ObjectGuid::LowType ownerGuid) const;
    std::vector<Housing::VisitEntry> GetVisitList(Player const* player, uint8 list) const;
    // Likes: one per account per island.
    uint32 CountLikes(ObjectGuid::LowType ownerGuid) const;
    bool LikesIsland(Player const* player, ObjectGuid::LowType ownerGuid) const;
    bool ToggleLike(Player* player, std::string& reason);
    // Who visited: the last 50 arrivals.
    void LogVisit(ObjectGuid::LowType ownerGuid, Player* visitor) const;
    std::vector<std::pair<std::string, std::string>> GetVisitorLog(ObjectGuid::LowType ownerGuid, uint32 limit) const;
    uint32 CountVisitorsThisWeek(ObjectGuid::LowType ownerGuid) const;
    bool SetGreeting(Player* player, std::string const& greeting, std::string& reason);
    static char const* PrivacyName(uint8 privacy);
    // The guestbook: visitors leave notes; the owner reads them and can throw them out.
    bool SignGuestbook(Player* visitor, std::string const& text, std::string& reason);
    std::vector<Housing::GuestbookNote> GetGuestbook(ObjectGuid::LowType ownerGuid, uint32 limit) const;
    uint32 CountNewNotes(ObjectGuid::LowType ownerGuid) const;
    void MarkGuestbookRead(ObjectGuid::LowType ownerGuid) const;
    bool DeleteNote(Player* owner, uint32 noteId, std::string& reason);
    // Where visitors arrive: where the owner stands now, or (reset) the landing spot.
    bool SetDoor(Player* player, bool reset, std::string& reason);

    // ---- misc helpers shared by the scripts
    void Say(Player* player, std::string const& text) const;
    void Tip(Player* player, uint32 tip, std::string const& text);
    // A tour quest's step done, if the player is on that quest.
    static void QuestEvent(Player* player, uint32 questId);
    void SendAddonState(Player* player) const;
    // The figurines' bosses, told to the client as if it had met them, so the Collection's
    // previews can draw them (a model frame only draws creatures the client knows).
    void SendFigurineCreatures(Player* player) const;
    bool ResolvePlayerGuid(std::string const& playerName, ObjectGuid::LowType& guidLow, std::string& normalizedName) const;
    std::string NameOf(ObjectGuid::LowType guid) const;
    static std::string FormatMoney(uint64 copper);
    static std::string FormatYards(float yards);
    // Cuts text to at most maxBytes without splitting a UTF-8 character.
    static void TruncateUtf8(std::string& text, size_t maxBytes);
    // Heavy or spammable actions wait between uses: true (and a reason) while waiting.
    enum Cooldown : uint8
    {
        COOLDOWN_HEAVY = 0,  // pack up, set out a layout, get missing pieces, one of everything
        COOLDOWN_LIKE,
        COOLDOWN_UNDO,       // undo or redo of a step with many pieces
        COOLDOWN_AMBIENCE,   // weather, time and music: sent to everyone on the island
        COOLDOWN_COUNT
    };
    bool OnCooldown(Player* player, uint8 kind, uint32 ms, std::string& reason);
    // .house commands: a burst is fine, a flood isn't.
    bool CommandFlood(Player* player);
    // Edit mode's moves come in quick runs and have a window of their own.
    bool ShiftFlood(Player* player);
    bool PointFlood(Player* player);  // where the mouse points: up to 25 a second
    // Messages to another player (invites, likes, roommate news): each kind not over and over.
    enum Notice : uint8
    {
        NOTICE_INVITE = 0,
        NOTICE_ROOMMATE,
        NOTICE_LIKE,
        NOTICE_GUESTBOOK,
    };
    bool MayNotify(Player const* sender, ObjectGuid::LowType target, uint8 kind);
    static char const* CategoryName(uint8 category);

private:
    ObjectGuid::LowType HomeOfAccount(uint32 account, ObjectGuid::LowType fallback) const;
    // Everything kept under one character moves to another: storage, layouts, sets, guests,
    // likes, notes; with packUp the island's pieces are put away, else the island moves too.
    void MoveHousing(ObjectGuid::LowType from, ObjectGuid::LowType to, bool packUp);
    void MergeAccountHomes();
    bool RehomeAccountOf(ObjectGuid::LowType deleted);
    mutable std::unordered_map<uint32, ObjectGuid::LowType> _homeByAccount;  // account -> home character

    struct SpawnedPiece
    {
        ObjectGuid guid;
        bool editCopy{false};
    };

    // The green ring under a piece someone chose in the Placed list.
    struct Highlight
    {
        uint32 placementId{0};
        ObjectGuid ring;
    };

    // One per occupied island. Islands share the spot on an open-world map and are kept apart
    // by giving each owner an exact phase of their own (see IsHousingPhase).
    struct Session
    {
        ObjectGuid::LowType ownerGuid{0};
        uint32 phaseMask{0};
        uint32 mapId{0};
        bool initialized{false};
        bool decorating{false};
        uint32 nextPlacementId{1};
        std::unordered_map<ObjectGuid::LowType, uint32> selected;  // per player decorating
        std::unordered_map<ObjectGuid::LowType, std::vector<uint32>> groups;  // selected with it (Ctrl-click)
        std::unordered_set<ObjectGuid::LowType> roommates;         // guests who may decorate
        std::unordered_set<ObjectGuid> occupants;
        std::map<uint32, Housing::Placement> placements;
        std::unordered_map<uint32, SpawnedPiece> spawned;
        std::unordered_map<ObjectGuid::LowType, Highlight> highlights;  // per player
        ObjectGuid photoGuid;  // the photo tour's building
        ObjectGuid stewardGuid;
    };

    struct Journal
    {
        ObjectGuid::LowType island{0};  // the island its steps were made on
        std::deque<Housing::JournalEntry> undo;
        std::deque<Housing::JournalEntry> redo;
    };

    // What the last batch of changes did with pieces and gear, for the message that follows.
    struct ApplyReport
    {
        uint32 toCollection{0};  // back in the player's own Collection
        uint32 placed{0};
        uint32 gearToBags{0};
        uint32 gearMailed{0};
        uint32 toOthers{0};   // pieces someone else placed, back in their Collection
        std::vector<std::string> gearMissing;  // couldn't go back on a stand: no longer in the bags
    };

    struct PendingTrip
    {
        std::time_t at{0};
        float x{0.0f};
        float y{0.0f};
        float z{0.0f};
    };

    PlayerHousingMgr() = default;

    // PlayerHousingMgr.cpp
    void LoadConfig();
    bool LoadDefinitions();
    void ConvertLegacyData();
    // force: a GM inspecting, past the island's privacy.
    // With at, the player arrives there (back where they logged out) and keeps the way out
    // they already have.
    bool EnterHouse(Player* player, ObjectGuid::LowType ownerGuid, std::string& reason, bool force = false, Position const* at = nullptr);
    bool EnsureSession(ObjectGuid::LowType ownerGuid);
    bool InitializeSession(ObjectGuid::LowType ownerGuid, std::string& reason);
    void DespawnSessionObjects(Session& session, Map* map);
    // Rings under chosen pieces: one player's, or every one under a piece.
    void RemoveHighlight(Session& session, Map* map, ObjectGuid::LowType player);
    void RemoveHighlightsOf(Session& session, Map* map, uint32 placementId);
    void EndSessionIfEmpty(ObjectGuid::LowType ownerGuid);
    ObjectGuid::LowType RemovePlayerTracking(ObjectGuid playerGuid, bool eraseReturnLocation);
    bool TryAdmitGroupBot(Player* bot);
    void ApplyHousePhase(Player* player, uint32 phaseMask) const;
    void RestoreNormalPhase(Player* player);
    bool IsInHousingArea(WorldObject const* object) const;
    bool IsOnIslandGround(float x, float y) const;
    Map* GetHousingMap() const;
    void OnArrived(Player* player, ObjectGuid::LowType ownerGuid);
    uint32 GetCharacterFlags(ObjectGuid::LowType guid, uint32* tips = nullptr) const;
    void SetCharacterFlag(ObjectGuid::LowType guid, uint32 flag, bool tip) const;
    void ClearCharacterFlag(ObjectGuid::LowType guid, uint32 flag) const;
    void UpdatePendingTrip(Player* player);

    // HousingPieces.cpp
    // The island the player may change: their own, or one they're a roommate on (unless
    // ownerOnly).
    Session* GetOwnerSession(Player* player, std::string& reason, bool ownerOnly = false);
    // Whose Collection a piece goes back to.
    static ObjectGuid::LowType ItemOwnerOf(Session const& session, Housing::Placement const& placement)
    {
        return placement.placedBy ? placement.placedBy : session.ownerGuid;
    }
    // One of a piece out of its owner's Collection, for a piece coming back (redo, undo).
    bool TakePieceFor(Player* player, ObjectGuid::LowType itemOwner, uint32 itemEntry);
    Session const* FindSessionOf(Player const* player) const;
    bool SpawnPlacement(Session& session, Map* map, Housing::Placement const& placement);
    bool SpawnStand(Session& session, Map* map, Housing::Placement const& placement);
    bool SpawnFigure(Session& session, Map* map, Housing::PieceDefinition const& piece, Housing::Placement const& placement);
    void RemoveSpawned(Map* map, ObjectGuid const& guid);
    void DespawnPlacement(Session& session, Map* map, uint32 placementId);
    void RespawnPlacement(Session& session, Map* map, uint32 placementId);
    void SpawnSteward(Session& session, Map* map);
    void SpawnStewardAt(Session& session, Map* map, Position const& position);
    // Krook's welcome tour done by any character of the island's account: he stays away.
    bool TourDone(ObjectGuid::LowType ownerGuid) const;
    void PlaceStarterWreckage(Session& session, Map* map);
    bool CheckLimit(Session const& session, Housing::PieceDefinition const& piece, std::string& reason) const;
    bool IsSpotOnIsland(float x, float y, float z) const;
    // A piece back into the player's Collection, or one out of it (false: they have none).
    void ReturnPiece(Player* player, uint32 itemEntry);
    bool TakePiece(Player* player, uint32 itemEntry);
    void AddToStorage(ObjectGuid::LowType ownerGuid, uint32 itemEntry, int32 delta) const;
    bool ApplyChanges(Player* player, Session& session, std::vector<Housing::Change> const& changes, bool towardsAfter, std::string& reason);
    bool ApplyState(Player* player, Session& session, Map* map, uint32 placementId, std::optional<Housing::Placement> const& target, std::string& reason);
    void Record(Player* player, std::string const& label, std::vector<Housing::Change> changes, bool merge = false);
    // A journal step still fits the island: every piece it touches is the one it was written for.
    bool JournalStillApplies(Session const& session, Housing::JournalEntry const& entry, bool towardsAfter) const;
    // Undo lists that point at an island's pieces, when those pieces go some other way.
    void ForgetJournals(ObjectGuid::LowType ownerGuid);
    // What roommates put on an island goes back to them: pieces to their Collection, gear by mail.
    void ReturnRoommatePieces(ObjectGuid::LowType ownerGuid);
    // Housing of characters that no longer exist, at startup.
    void PurgeLeftovers();
    void RemoveHousingOf(ObjectGuid::LowType guid);
    bool Transform(Player* player, uint32 placementId, std::string const& label, float dx, float dy, float dz, float dO, bool absoluteO, float o,
        std::string& reason, bool merge = false);
    // Checks the island, applies the changes and records them as one step; the first change
    // is the piece the label names.
    // labelIsFinal: the label is used as it is, instead of naming the first piece in it.
    bool Commit(Player* player, Session& session, std::string const& label, std::vector<Housing::Change> changes, std::string& reason,
        bool merge = false, bool labelIsFinal = false);
    void SnapToGrid(ObjectGuid::LowType guid, float& x, float& y) const;
    // Pieces that go wherever this one goes: what stands on it, and for a building (when
    // includeInside) what's inside it; each with what stands on them in turn.
    std::vector<uint32> CarriedBy(Session const& session, uint32 placementId, bool includeInside) const;
    uint32 FindSurfaceUnder(Session const& session, float x, float y, float z, std::set<uint32> const& exclude = {}) const;
    // Whether x, y is over that surface's top (within its outline, or its footprint).
    bool IsOverSurface(Housing::PieceDefinition const& piece, Housing::Placement const& surface, float x, float y) const;
    bool ChangeStand(Player* player, uint32 placementId, Housing::Placement const& after, std::string const& label, std::string& reason);
    bool MoveGearToStand(Player* player, ObjectGuid::LowType ownerGuid, uint32 placementId, uint8 slot, uint32 itemGuid, std::string& reason);
    // Gear back to its owner: into the player's bags when it's theirs, otherwise by mail.
    void ReturnGear(Player* player, ObjectGuid::LowType islandOwner, ObjectGuid::LowType gearOwner, uint32 placementId, uint8 slot,
        Housing::GearItem const& gear);
    void LoadGear(ObjectGuid::LowType ownerGuid, std::map<uint32, Housing::Placement>& placements) const;
    // A stand's item, loaded from item_instance (not in anyone's inventory), or null.
    Item* LoadGearItem(ObjectGuid::LowType ownerGuid, Housing::GearItem const& gear) const;
    void SavePlacement(ObjectGuid::LowType ownerGuid, Housing::Placement const& placement, uint32 mapId) const;
    void DeletePlacement(ObjectGuid::LowType ownerGuid, uint32 placementId) const;
    std::string PieceName(uint32 itemEntry) const;
    // Where things went back to. collectionToo false: only what the player couldn't guess
    // (someone else's Collection, gear by mail).
    std::string DescribeReturns(bool collectionToo = true) const;
    std::string DescribeItemReturns(bool collectionToo = true) const;


    // HousingGroups.cpp
    // The group's pieces that nothing else in it carries (the rest move with them), and what
    // each of those carries.
    void GroupRoots(Session const& session, std::vector<uint32> const& members, std::vector<uint32>& roots, std::vector<uint32>& carried) const;
    // Moves every root as told (after is filled in from before) and carries the rest along;
    // one undo step.
    bool CommitGroup(Player* player, Session& session, std::vector<uint32> const& members, std::string const& label,
        std::function<void(Housing::Placement const& before, Housing::Placement& after)> const& move, std::string& reason, bool merge = false);
    // Moves the group by dx, dy, dz and turns it about its middle (radians); with snap and the
    // grid on, the first piece lands on the grid. The label reads "<verb> 3 pieces<tail>".
    bool ShiftGroup(Player* player, Session& session, std::vector<uint32> const& members, float dx, float dy, float dz, float turn,
        bool snap, std::string const& verb, std::string const& tail, std::string& reason, bool merge);
    // The group the selection was part of, kept after a step that changed its pieces;
    // after any other step (or with its first piece gone) the group is let go.
    bool KeepGroupAfterStep(Session& session, ObjectGuid::LowType self, uint32 selectedBefore, std::vector<uint32> const& groupBefore,
        std::vector<Housing::Change> const& changes);
    // The selection moves to this piece: a group it doesn't lead is let go.
    void SelectOne(Session& session, ObjectGuid::LowType self, uint32 placementId);
    // A piece standing on a raised floor (a building's, which the server can't see: pieces
    // have no collision) keeps its height; one on the ground follows the ground.
    float FloorHeightNear(Player* player, Session const& session, Housing::Placement const& reference, float x, float y) const;
    bool PickUpGroup(Player* player, Session& session, std::vector<uint32> const& members, std::string& reason);
    bool MoveGroupTo(Player* player, Session& session, std::vector<uint32> const& members, Position const& target, std::string& reason);
    // The pieces new placements need, in the player's Collection: missing copies of unlocked
    // pieces are bought first (free in FreeMode); paid says what they cost.
    bool EnsurePieces(Player* player, std::map<uint32, uint32> const& needed, std::string& reason, uint64* paid = nullptr);
    bool FitsLimits(Session const& session, std::map<uint32, uint32> const& adding, std::string& reason) const;
    // Sets a saved set down with its first piece at target, the whole set turned to facing.
    bool StampSet(Player* player, Session& session, uint32 setId, Position const& target, float facing, std::string& reason);
    float GroundHeightNear(Player* player, float x, float y, float z) const;
    void DespawnPhoto(Session& session, Map* map);

    // HousingGhosts.cpp
    struct GhostPiece
    {
        uint32 placementId{0};  // moving: the piece; new: 0
        uint32 itemEntry{0};
        float dx{0.0f};         // from the lead piece, in its own frame (x: where it faces)
        float dy{0.0f};
        float dz{0.0f};
        float dO{0.0f};
        float scale{1.0f};
        float pitch{0.0f};
        float roll{0.0f};
        uint64 look{0};
        ObjectGuid shown;       // the ghost creature, or the carried building
        float x{0.0f};          // where it's shown
        float y{0.0f};
        float z{0.0f};
        float o{0.0f};
        uint32 drawnMs{0};      // carried objects: when last redrawn
        bool reshaped{false};   // sized or tilted since it was last drawn
    };
    struct Ghost
    {
        bool isNew{true};
        uint32 copyOf{0};
        uint32 setId{0};        // a saved set being set down (new pieces, all of them)
        std::vector<GhostPiece> pieces;  // the lead first
        float forward{3.0f};    // from the player: ahead, to the left, and up from what's under it
        float side{0.0f};
        float lift{0.0f};
        float o{0.0f};          // the lead's turn
        uint32 parent{0};       // what the lead stands on, where it's shown
        bool posed{false};
        // The player, as last heard: their position is guessed ahead from there.
        float seenX{0.0f};
        float seenY{0.0f};
        float seenZ{0.0f};
        float seenO{0.0f};
        uint32 seenMs{0};
        uint32 updatedMs{0};
        float floorZ{0.0f};     // the floor the player stands on, kept while they jump
        bool floorRaised{false};  // above the ground: a building's floor
        // Following the mouse instead: the point it shows at (what the mouse points at).
        bool atPoint{false};
        float pointX{0.0f};
        float pointY{0.0f};
        float pointZ{0.0f};
        float rawZ{0.0f};       // the height the client gave with the point, before the rules
        bool onWall{false};     // the point is on a wall: the piece faces out from it
        bool onCeiling{false};  // the point is on a ceiling: the piece hangs from it
        float wallO{0.0f};
        float wallTurn{0.0f};   // the player's own turn, on top of the wall's
        std::string note;       // why the last point wasn't taken (for the addon), or empty
        bool local{false};      // the player's own client moves it (see SetAddonClient)
        std::string piecesSent; // the ghost's pieces as last told to the addon
        float playerO{0.0f};    // the player's facing as last seen: turning turns the piece too
        bool playerOKnown{false};
    };
    // Which way the lead faces now: its own turn, or out from the wall it's on.
    static float GhostFacing(Ghost const& ghost);
    uint32 GhostDisplayFor(uint32 itemEntry) const;
    uint32 SolidDisplayFor(Housing::PieceDefinition const& piece) const;
    // One in the player's Collection.
    bool HasOneToPlace(Player* player, uint32 itemEntry) const;
    // For a client that moves the ghost itself: its creatures and where each sits from the
    // lead (sent when they change), and where the server has the lead now.
    void SendGhostPieces(Player* player, Ghost& ghost) const;
    void SendGhostPose(Player* player, Ghost const& ghost, float x, float y, float z) const;
    // Shared by new pieces and sets: starts the ghost the player carries from now on.
    void BeginGhost(Player* player, Session& session, Ghost ghost);
    // The player where they are about now: their last movement packet, carried forward.
    void PredictPlayer(Player* player, Ghost& ghost, float& x, float& y, float& z, float& o) const;
    // What x, y would stand on: a table top (parent), else the floor.
    float GhostFloor(Player* player, Session const& session, Ghost const& ghost, float x, float y, float playerZ, bool building,
        uint32& parent) const;
    // A new piece on the island: shown, saved, selected, and one undo step.
    bool AddNewPlacement(Player* player, Session& session, Housing::Placement const& placement, std::string& reason);
    // Where the lead goes now: in front of the player, on the grid, on a table or the floor.
    void PoseGhost(Player* player, Session const& session, Ghost& ghost, float& x, float& y, float& z, uint32& parent);
    void DrawGhost(Player* player, Session& session, Ghost& ghost, bool force);
    void DespawnGhost(Ghost& ghost, Map* map);
    void EndGhost(Player* player);

    // HousingLayouts.cpp
    std::vector<Housing::Placement> LoadSavedPieces(ObjectGuid::LowType ownerGuid, uint32 layoutId) const;
    // fromLayout 0 copies fromOwner's island as it is now.
    void WriteLayout(ObjectGuid::LowType ownerGuid, uint32 layoutId, std::string const& name, std::string const& source,
        ObjectGuid::LowType fromOwner, uint32 fromLayout) const;
    uint32 NextLayoutId(ObjectGuid::LowType ownerGuid) const;

    // HousingAmbience.cpp
    void SendAmbience(Player* player, Housing::HouseRecord const& house, bool withMusic);
    void RestoreAmbience(Player* player);
    void UpdateAmbience(Player* player, uint32 diffMs);
    void ApplyAmbienceToIsland(ObjectGuid::LowType ownerGuid, bool withMusic);

    // HousingCollection.cpp
    bool RuleMet(Player const* player, Housing::PieceRule const& rule) const;
    // Whether any rule group of the piece is complete; `triggered` marks rules the current
    // event satisfies by itself.
    template <typename Triggered>
    bool AnyGroupMet(Player const* player, Housing::PieceDefinition const& piece, Triggered triggered) const;
    bool Unlock(Player* player, Housing::PieceDefinition const& piece, bool announce);
    static bool IsAreaExplored(Player const* player, uint32 areaId);

    mutable std::recursive_mutex _lock;

    bool _enabled{true};
    bool _ready{false};  // started up: definitions loaded, leftovers purged
    bool _freeMode{false};
    bool _unlockAll{false};
    bool _gmVisitBypass{false};
    uint8 _defaultPrivacy{Housing::PRIVACY_PRIVATE};
    uint32 _stewardEntry{900200};
    uint32 _stewardDisplayId{25384};
    uint32 _maxFurnishings{200};
    uint32 _maxBuildings{10};
    uint32 _keyDelaySeconds{5};
    float _sizeMin{0.5f};   // times the piece's normal size
    float _sizeMax{2.0f};
    float _tiltMax{180.0f};  // degrees either way; 180: all the way round
    uint32 _maxSavedLayouts{5};
    bool _catalogEverything{false};
    bool _ghosts{true};     // see-through ghosts for furniture (the client patch has their models)
    std::string _layoutCode{"cleared"};

    Housing::LayoutDefinition _layout;
    std::map<uint32, Housing::PieceDefinition> _pieces;
    std::map<std::pair<uint8, uint32>, std::vector<uint32>> _piecesByRule;  // (rule type, param1) -> pieces

    std::unordered_map<ObjectGuid::LowType, Session> _sessionsByOwner;
    std::unordered_map<ObjectGuid, ObjectGuid::LowType> _playerOwnerByGuid;
    std::unordered_map<ObjectGuid, WorldLocation> _returnLocations;
    std::unordered_map<ObjectGuid::LowType, Journal> _journals;
    std::unordered_map<ObjectGuid, PendingTrip> _pendingTrips;
    std::atomic<uint32> _pendingTripCount{0};  // _pendingTrips.size(), read without the lock
    std::unordered_map<ObjectGuid, std::array<uint64, COOLDOWN_COUNT>> _cooldowns;  // ready again at (ms)
    struct CommandWindow
    {
        uint64 start{0};
        uint32 count{0};
    };
    std::unordered_map<ObjectGuid, CommandWindow> _commandWindows;
    std::unordered_map<ObjectGuid, CommandWindow> _shiftWindows;
    std::unordered_map<ObjectGuid, CommandWindow> _pointWindows;
    std::unordered_set<ObjectGuid> _editMode;  // asked for edit mode; it holds while they decorate here
    std::unordered_set<ObjectGuid> _groupHold;  // the addon says Ctrl is down: clicks add to the group
    std::unordered_set<ObjectGuid> _addonClients;        // has the addon: the House Key opens its window
    std::unordered_set<ObjectGuid> _mouseClients;        // has PlayerHousing.dll: ghosts follow the mouse
    std::unordered_set<ObjectGuid> _localGhostClients;   // and the DLL moves the ghost itself
    mutable std::unordered_set<ObjectGuid::LowType> _knownHouses;  // have a house row (EnsureHouse); until logout
    mutable std::unordered_map<ObjectGuid::LowType, float> _gridSizes;  // read once a login: the addon's state has it
    void SendAddonRows(Player* player, std::string const& kind, std::string const& label, std::vector<std::string> const& parts) const;
    std::unordered_map<ObjectGuid::LowType, std::unordered_map<uint64, uint64>> _notified;  // sender -> target << 8 | kind -> sent at (ms)
    std::unordered_set<ObjectGuid> _arrivals;  // teleported onto an island, greeting not shown yet
    std::unordered_map<ObjectGuid, MannequinLook> _mannequins;
    std::unordered_map<ObjectGuid, ObjectGuid> _chestBankers;  // player -> the banker their chest brought

    // Players on an island: when their clock is resent, and their music replayed.
    struct AmbienceTimers
    {
        uint32 clockMs{0};
        uint32 musicMs{0};
    };
    std::unordered_map<ObjectGuid, AmbienceTimers> _ambienceTimers;

    std::unordered_map<ObjectGuid, uint32> _photoTours;  // GM -> the building on show
    std::unordered_map<ObjectGuid, Ghost> _carrying;     // player -> what follows them
    ApplyReport _report;
    static constexpr uint32 MAX_SAVED_SETS = 20;
};

#define sPlayerHousingMgr PlayerHousingMgr::instance()

#endif
