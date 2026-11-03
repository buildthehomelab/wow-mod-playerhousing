# Server setup

For server admins: from nothing to players building on their islands. It takes about an
hour, most of it compiling. Each step says how to check it worked.

Just want to try it? [tools/test-server](../tools/test-server/README.md) runs a complete
server with the module in a container, no setup needed.

## What you need

- An AzerothCore WotLK (3.3.5a) server you build yourself. Stock
  [AzerothCore](https://github.com/azerothcore/azerothcore-wotlk) should work: the module
  needs no core patches, and every hook and core function it uses is part of AzerothCore
  itself (the script hooks it relies on came with AzerothCore's 2024 script rework). It is
  developed and tested on
  [azerothcore-wotlk-playerbots-custom](https://github.com/dudemcbro/azerothcore-wotlk-playerbots-custom),
  a playerbots fork, and works alongside mod-playerbots (bots never get housing). It
  hasn't been built against stock AzerothCore yet; if it doesn't build there, please open
  an issue.
- The core's map tools (`mmaps_generator`), built with `-DTOOLS_BUILD=maps-only` or `all`.
- A 3.3.5a client, to build the client patch your players install. Use a client like
  theirs: plain 3.3.5a, plus any custom patches your server hands out.
- `smpq` and Python 3 for the patch. On Ubuntu or Debian: `sudo apt install smpq python3`
  (on an immutable desktop such as Bazzite or Bluefin, inside a distrobox).

## 1. Build the module into the server

```bash
cd <azerothcore>/modules
git clone https://github.com/buildthehomelab/wow-mod-playerhousing.git mod-playerhousing
cd <azerothcore>/build
cmake .. <your usual options, with -DTOOLS_BUILD=maps-only if the map tools aren't built yet>
cmake --build . -j"$(nproc)"
cmake --install .
```

Check: `etc/modules/mod_playerhousing.conf.dist` exists in the server's install folder.

## 2. Configure

With the worldserver stopped:

```bash
cd <server>/etc/modules
cp mod_playerhousing.conf.dist mod_playerhousing.conf
```

The defaults suit a live server; [Knobs and dials](#knobs-and-dials), below, lists everything
you can change. Also check that `worldserver.conf` doesn't set `AddonChannel = 0`: the addon
talks to the server over it.

## 3. Apply the SQL

With the worldserver stopped, from the module folder:

```bash
MYSQL="mysql -u <user> -p" tools/release/apply_sql.sh
```

It applies the five files in the right order (every one is safe to apply again) and says how
many pieces are defined (about 2,400, counting the optional catalog). The worldserver's own updater doesn't pick these up,
because they live in `sql/`, not `data/sql/`.

## 4. Clear the island in the server data

The default island is GM Island without its guild hall. The server's collision and pathing
have to match, or players walk into walls they can't see. With the worldserver stopped:

```bash
SERVER_DIR=<server> tools/gm-island-cleared/server_data.sh
```

`SERVER_DIR` is the server's install folder (the one with `bin/` and `data/`). It keeps the
originals as `.orig`; `--restore` puts the hall back. Details:
[tools/gm-island-cleared](../tools/gm-island-cleared/README.md).

## 5. Start and check

Start the worldserver. Its log should say:

```
mod-playerhousing: Loaded layout 'cleared' and 340 pieces.
```

(About 340 with the default curated Collection; around 2,400 with
`PlayerHousing.Catalog = "everything"`.)

If it says definitions failed to load, step 3 was incomplete: run `apply_sql.sh` again.

## 6. Make the player download

One zip with everything a player needs, built from your 3.3.5a client's `Data` folder:

```bash
tools/release/make_player_bundle.sh /path/to/WoW-3.3.5a/Data
```

It writes `PlayerHousing-client.zip`: the addon, PlayerHousing.dll and its launcher, the
client patch (`Data/patch-H.MPQ`) and [PLAYER_SETUP.md](PLAYER_SETUP.md). Host it next to your
client download or patches, and point players to it. Options:

- `--patch-name patch-Z.MPQ` if your server already ships a `patch-H.MPQ`. The patch
  should load after any other patch that carries `Item.dbc`, `ItemDisplayInfo.dbc`,
  `CreatureDisplayInfo.dbc` or `CreatureModelData.dbc`, so give it a later letter.
- `--icons-only` for the `guildhouse` layout.
- `--patch <file>` to pack a patch you built already.
- `--patch-name enUS/patch-enUS-z.MPQ` for clients with HD packs, whose own patches sit in
  `Data/enUS` and load after anything in `Data`.

The addon, DLL and launcher in the zip come from the module folder you built. Each
[release](https://github.com/dudemcbro/mod-playerhousing/releases) also has them on their own (`PlayerHousing-<version>-client.zip`), without the
patch; the patch is the one part only you can provide, since it's built from game files.

## 7. Try it as a player

Install the zip on your own client ([PLAYER_SETUP.md](PLAYER_SETUP.md)), log in, and:

1. Talk to Krook beside any capital's innkeeper and ask for a house: you get a House Key.
2. Right-click the key, **Go home**: you land on the cleared island.
3. Click a piece in the Collection: it follows the mouse, a click sets it down.

## Knobs and dials

Everything in `mod_playerhousing.conf` is read when the worldserver starts: change it, then
restart (`.reload config` doesn't pick it up). Any setting can also come from an environment
variable, the core's usual way: `PlayerHousing.MaxFurnishings` is
`AC_PLAYER_HOUSING_MAX_FURNISHINGS`, `PlayerHousing.FreeMode` is
`AC_PLAYER_HOUSING_FREE_MODE`, and so on. Values outside a setting's range are pulled to the
nearest end of it.

### The island

| Setting | Default | Range | What it does |
| --- | --- | --- | --- |
| `PlayerHousing.Enable` | 1 | 0, 1 | The whole module. Off: housing stops working, and every island, piece and unlock stays in the database for when it's back on. |
| `PlayerHousing.Layout` | cleared | cleared, guildhouse | `cleared`: removes GM Island's big guild hall from every housing island, so islands start as open ground where the hall stood (needs step 4's server data and the client patch). `guildhouse`: keeps the guild hall, and players land inside it. The patch takes the hall out of the map tile, and the module puts it back on the regular GM Island as an object only players outside housing see. Switching to `cleared` sends anything placed inside the old hall back to its owner's Collection. |
| `PlayerHousing.MaxFurnishings` | 200 | 1 to 5000 | Furnishings one island can hold. |
| `PlayerHousing.MaxBuildings` | 10 | 0 to 200 | Buildings one island can hold. 0: no buildings at all. |
| `PlayerHousing.SavedLayouts` | 5 | 0 to 20 | Layouts each island can keep. 0 turns layouts off. |

Every open island shares the same map cells on Kalimdor, so the piece limits are also how you
keep a busy server quick: about 6,000 pieces out across 100 occupied islands is comfortable;
past a few hundred busy islands, lower `MaxFurnishings` (see
[Thousands of players](HOW_IT_WORKS.md#thousands-of-players)).

### Placing

| Setting | Default | Range | What it does |
| --- | --- | --- | --- |
| `PlayerHousing.Size.Min` | 0.5 | 0.1 to 1 | How small a piece can be made, as a multiple of its normal size. |
| `PlayerHousing.Size.Max` | 2 | 1 to 10 | How big a piece can be made. `Size.Min = 1` and `Size.Max = 1` turn resizing off. |
| `PlayerHousing.Tilt.Max` | 180 | 0 to 180 | How far a piece tilts each way, in degrees. 180: all the way round. 0 turns tilting off. Mannequins and figurines always stand upright. |
| `PlayerHousing.Ghosts` | 1 | 0, 1 | 1: a held piece is a see-through copy of itself (needs this version's client patch). 0: pieces are carried as they are, for players on an older patch. |

### Getting there and visitors

| Setting | Default | Range | What it does |
| --- | --- | --- | --- |
| `PlayerHousing.HouseKey.DelaySeconds` | 10 | 0 to 60 | How long Go home takes, like a hearthstone cast: moving or combat cancels it. 0 is instant. GMs and FreeMode always go at once. |
| `PlayerHousing.DefaultPrivacy` | private | private, friends, public | Who may visit a new island: its owner's guest list, friends and guild too, or anyone. Players change their own afterwards. |
| `PlayerHousing.GmBypassPrivate` | 0 | 0, 1 | GMs in GM mode can visit any island. (`.house inspect <player>` goes to any island regardless.) |

### What players own

| Setting | Default | Range | What it does |
| --- | --- | --- | --- |
| `PlayerHousing.Catalog` | curated | curated, everything | `curated`: the ~340 pieces earned by playing. `everything`: also about 2,000 more, every other object model in the game, which everyone has from the start (a sandbox; needs the catalog SQL, which `apply_sql.sh` applies). |
| `PlayerHousing.FreeMode` | 0 | 0, 1 | Test servers: extra copies cost nothing and Go home is instant. |
| `PlayerHousing.UnlockAll` | 0 | 0, 1 | Test servers: every piece is unlocked for everyone, with "one of everything". |

### Krook

| Setting | Default | What it does |
| --- | --- | --- |
| `PlayerHousing.StewardEntry` | 900200 | Krook's creature entry. Change it only if 900200 clashes with another module. |
| `PlayerHousing.StewardDisplayId` | 25384 | Krook's model (a wolvar orphan). Any creature display id works. |

### Ready-made setups

- **A test or showcase server:** `FreeMode = 1`, `UnlockAll = 1`, `GmBypassPrivate = 1`;
  `Catalog = "everything"` for a sandbox with every model in the game.
- **A busy live server:** `MaxFurnishings = 100`, `MaxBuildings = 5`.
- **Grand islands on a small server:** `MaxFurnishings = 500`, `MaxBuildings = 25`,
  `Size.Max = 4`, `SavedLayouts = 10`.
- **No tilting or resizing:** `Tilt.Max = 0`, `Size.Min = 1`, `Size.Max = 1`.
- **Housing paused:** `Enable = 0` keeps every island, piece and unlock in the database.

### Beyond the config file

- **Prices and unlocks:** what each piece costs as an extra copy, and what unlocks it, come
  from `tools/content/pieces.py`. Edit it, then run `tools/content/build_content.py` (see
  [Content](CONTENT.md)) and apply the SQL again. Editing the tables
  directly works until the next `apply_sql.sh`, which rewrites them.
- **Where players land and where Krook stands** on the island: the
  `mod_playerhousing_layout` table (landing point, Krook's offset from it, the island's
  center and radius), set in `sql/db_world/base/mod_playerhousing_world.sql`.
- **Fixed limits**, the same on every server: 15 housing commands in any 3 seconds per player
  (GMs exempt), 3 seconds between heavy actions (pack up, set out a layout), 30 undo steps,
  the last 100 guestbook notes, five reports an hour per account.
- **GM tools** for running it: `.house unlock <item|name|all> [player]` and `relock`,
  `.house unlocks [player]`, `.house key` (a House Key for yourself), `.house add` (Krook
  beside you for ten minutes), and moderation: `.house reports`, `close`, `inspect`, `hide`,
  `unhide`, `cleargreeting`, `gmpackup` (all in
  [COMMANDS.md](COMMANDS.md#moderation)).

## Updating

```bash
cd <azerothcore>/modules/mod-playerhousing
git pull
cd <azerothcore>/build && cmake --build . -j"$(nproc)" && cmake --install .
```

Then, with the worldserver stopped, run `tools/release/apply_sql.sh` again and start it.
When the update changed the addon, the DLL or the content (`tools/content/`,
`client-addon/`, `client-dll/bin/`), run `make_player_bundle.sh` again and tell players to
install the new zip over the old one (and clear their `Cache` folder once).

### From the house levels version

Older versions had house styles, stages, a vendor catalog and furniture unlocks. Applying
the world SQL removes those tables. At the next startup the module converts what players
had: placed furniture and catalog unlocks become Collection unlocks and counts, and with the
cleared island, anything that stood inside the old guild hall goes back to its owner's
Collection with a message on their next visit. Gold spent on stages isn't refunded.

The repository's history was rewritten in September 2026. A clone from before that cannot
pull: clone it again, or run `git fetch && git reset --hard origin/main` (this discards local
commits).

## Uninstall

With the worldserver stopped (the characters rollback hands out mail ids), from the
module directory:

```sh
cd <azerothcore>/modules/mod-playerhousing
mysql -u <user> -p acore_world < sql/db_world/base/mod_playerhousing_world_rollback.sql
mysql -u <user> -p acore_characters < sql/db_characters/base/mod_playerhousing_characters_rollback.sql
```

They remove everything the module added; gear still on mannequins is mailed back to its
owners. On the cleared layout, put the guild hall back in the server data with
`tools/gm-island-cleared/server_data.sh --restore` (see its
[README](../tools/gm-island-cleared/README.md)). Then delete the module directory and your
server's `etc/modules/mod_playerhousing.conf`, re-run CMake and rebuild. Players can
remove the client patch and the addon.

## When something's wrong

| What you see | Why, and the fix |
| --- | --- |
| Log: definitions failed to load, housing disabled | Only part of the world SQL went in. Run `apply_sql.sh` again, restart. |
| Players walk into an invisible wall where the hall stood | The server data step was skipped (step 4), or the player has no client patch. |
| Players see the hall but walk through it | The player is missing the client patch, or another patch loads after it and wins. |
| Housing items are red question marks | Same: the client patch is missing or loads too early. |
| `.house` says the window needs the addon | The player hasn't installed the addon, or it's disabled on the character screen's AddOns button. |
| Pieces follow the player instead of the mouse | The DLL isn't loaded: the game wasn't started with `PlayerHousingLauncher.exe`, or `AddonChannel = 0`. |
| Bots have no islands | On purpose: playerbots never get housing. |

Uninstalling: see [Uninstall](#uninstall), above.
