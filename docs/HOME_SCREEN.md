# Home screen

After logging in, an account lands in the world at its plot instead of on character select, and
picks a character from there. Click Play and the game enters that character, back where they last
were. It needs no client exe change beyond the marker described below, and works with
`PlayerHousing.Enable` off: the rest of the module stays asleep and its SQL isn't needed.

This is early. There's one plot for everybody and no furniture or figures on it yet.

## What you need

- A core with `characters.home_viewer`, `WorldSession::SetHomeEnter` and the
  `OnPlayerHomeViewerLogin` hook. Stock AzerothCore doesn't have these, so this feature needs a fork
  that carries them. Its character list has to mark each character for the character select
  screen, in the value that core sends as an extra field.
- A character select screen that reads those marks, hides the viewer and enters whoever is marked.
- The [HomeScreen addon](https://github.com/buildthehomelab/wow-addon-HomeScreen) on the client, which draws the character list.

Without those, the rest of this module works as described in the README.

## Turning it on

Set `CharacterSelect.HomeScreen = 1` in worldserver.conf. Then, as a GM, on your own account:

| Command | What it does |
|---|---|
| `.home enable` | Creates the account's viewer, or wakes it. Log out to see it |
| `.home disable` | Parks the viewer: the account goes back to character select |

`CharacterSelect.HomeScreen = 0` turns it off for everybody, without touching anyone's viewer.

To reach character select with the home screen on, click **Character select** in the window, or hold
Shift while the character list loads.

Still to do before players get this: everything else that lists an account's characters has to leave the
viewer out, including modules that pick the last played character or offer a list to spend things on,
and playerbots must never log one in. So far it's for testing on a GM's own account.

## How it works

- **The viewer** is a character row nobody plays: level 1, invisible, not selectable, rooted, always
  at the plot. Its saved position is the plot, so getting there needs no teleport. `PlayerHousing.Home.*`
  sets where that is, and every viewer moves there when the server starts.
- **The phase** is the same kind a house uses: an exact phase of its own, so the viewer sees nobody else
  and nothing spawned in the normal world. The ground and its scenery stay.
- **Player hooks** don't run for a viewer: the core skips them, so no module treats it as someone playing.
- **The character list** decides who to enter by itself, each time it goes out: the character Play chose
  if there is one, nobody if the player asked for character select, otherwise the viewer.
- **The addon** talks to the server over the addon command channel:

| Command | What it does |
|---|---|
| `.home list` | Answers with addon whispers, prefix `HOMESCREEN`, tab separated: `begin`, then `row`, name, race, class, level, zone for each character, then `end` |
| `.home play <name>` | The next character list enters this character. The addon logs out right after sending it |
| `.home glue` | The next character lists enter nobody, until a character logs in. The addon logs out right after sending it |

All three work for the viewer only, so the addon can ask on any character and stay quiet on real ones.

If a disconnect left a character in the world, the list enters that character instead of the viewer,
so the player gets back to it. The character select screen won't enter the viewer if the addon is
missing or turned off for it, because there would be no way to leave.

A stock core builds this module without any of this: `mod-playerhousing.cmake` checks for the core's hook.
