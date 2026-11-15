# HogLord Build of Gecko

![The welcome screen: Vault Boy with a hog's head, giving a thumbs up](docs/hoglord/welcome.png)

My build of [gecko](https://github.com/JanSimek/gecko), Jan Simek's Fallout 2 map editor, with the tools I use to make maps for my Fallout 2 mod. This page covers only what this build adds. For the editor itself (its controls, its tools, how it is built) see [gecko's own README](https://github.com/JanSimek/gecko#readme).

It keeps gecko's own files and folders: the program is still `gecko.exe`, and its settings and stamps live where gecko keeps them (`%LOCALAPPDATA%\gecko\gecko`).

## Download

Windows builds are on the [releases page](https://github.com/HogLord69/gecko/releases). Unzip anywhere and run `gecko.exe`. On first launch Preferences opens: press **Add Path...**, then **Add Folder...**, and choose your Fallout 2 folder (the one with `master.dat` in it). No game data is included, so you need your own copy of the game.

List one game at a time. Each game numbers its art and objects in its own lists, so with two games listed the top one's lists win and the other's maps show the wrong pieces. To use another game's pieces, see the next section.

## What this build adds

### Pieces from other games

![The object palette showing Fallout: Sonora's scenery that Fallout 2 doesn't have](docs/hoglord/game-list.png)

The object palette has a **Game** list. Pick another game you have installed and the palette shows its pieces, read from your own install and kept apart from your game's data. **Only pieces Fallout 2 doesn't have** (on at first) leaves out the Fallout 2 pieces every total conversion carries, judged by the picture: of Fallout: Sonora's 2,377 scenery pieces it shows the 1,675 that are its own.

Drop one on the map and it is brought into your mod first, with new numbers, so it works in your game and shows at once; **Tools > Apply New Art** puts it in the game. Dropping it again uses the same copy. Scenery, walls, misc pieces and items that have no inventory picture (desks, lockers, chests, keys) come across. Critters, weapons, armor, ammo and drugs are refused with the reason: their other numbers (AI packet, talking head, ammo, inventory picture) would point at the wrong things. A piece's script stays behind, since its number means another script in your game.

It works with the [map workflow](#the-map-workflow) that comes with this build: run `python setup.py` in the `workflow` folder once and give it the folders of the other games you have. gecko finds that folder by itself when it sits beside `gecko.exe`, as it does in the release zip (or set it in **File > Preferences > Mod Tools**).

### Stamps that come with it

![The Pattern Library with "Came with gecko": Fallout 2's medium adobe buildings](docs/hoglord/pattern-library.png)

**Edit > Stamp Pattern** lists the stamps that come with this build under your own, as **Came with gecko**: 1,567 buildings, graveyards, junkyards and ruins cut from Fallout 1, Fallout 2, Sonora, Nevada, Resurrection and Olympus, and **HogLord Props**, 21 props of my own. Every game's folder is laid out the same way: Buildings by what they are built from and then by size (Small, Medium, Large, Huge), Graveyards, Junkyards, and Ruins by size; stamps are named after the map they came from ("Arroyo Village #2"). Nothing has to be copied anywhere. **Fallout 2** and **HogLord Props** stamp as they are. The credits are in [`resources/stamps/README.md`](resources/stamps/README.md).

![Fallout: Sonora's medium brick and concrete buildings, drawn with Sonora's own data](docs/hoglord/stamps-other-game.png)

A stamp holds its game's object numbers, not pictures, so a stamp cut from another game draws the wrong pieces in yours. With the [map workflow](#the-map-workflow) set up for that game, its folder is drawn with the game's own data, and picking a stamp converts it first: a copy goes into your stamps (**Custom > From Fallout Sonora**), each piece is matched to one your game has or brought into your mod with new numbers, and each floor tile is matched or brought into a spare tile slot (when those run out, the closest-looking tile is used). It then stamps the same as in its own game; **Tools > Apply New Art** puts the new pieces in the game, and until then gecko shows them anyway, after a restart too.

### New scenery from a picture

The **Tools** menu makes new art for your mod with the [map workflow](#the-map-workflow)'s tools, which gecko finds beside `gecko.exe`.

![Add Scenery from Picture: a film projector render, previewed beside the player on the hex grid](docs/hoglord/add-scenery.png)

**Tools > Add Scenery from Picture** turns a render or photo into a new scenery piece: cut out, shaded like Fallout 2's pre-rendered art and put in the game's palette, with its own art, proto, name and examine text. The preview stands it on the hex grid beside the player, for scale; a click on the picture picks the point it stands on. On the map's right-click menu, **Mirrored Copy**, **Bigger or Smaller Copy** and **Name and Examine Text** work on the one selected piece.

### Buildings cut into walls

![Cut Building into Walls: a house render with the foot of its walls marked, and where it is cut](docs/hoglord/cut-building.png)

**Tools > Cut Building into Walls** cuts a building rendered with the Fallout camera into wall pieces, one per hex, on the same screen columns as Fallout 2's own walls (32 and 16 pixels on a 14 degree wall, 16 on a 37 degree one). Click the foot of the walls; Shift-click the top of a wall, and the roof above it becomes roof tiles. A stamp lays the whole building down.

### Apply New Art

**Tools > Apply New Art** saves the map, closes gecko, deploys the mod in a window that numbers the steps, says FINISHED and closes itself, and opens gecko again on the map. The steps run on their own, so closing the window early stops nothing. The window, with each step's own output left out:

```
Apply New Art
============================================
Waiting to go in:
  0x020009C4  sdaygl19  from Fallout: Sonora (0x02000944)

Waiting for gecko (process 12345) to close...

--- Step 1 of 6: gen_ids.py

--- Step 2 of 6: deploy.py

--- Step 3 of 6: scenery_text.py --no-deploy

--- Step 4 of 6: deploy.py

--- Step 5 of 6: repack_art.py   (the slow one: it reads every map, about a minute)

--- Step 6 of 6: validate.py

============================================
  FINISHED
  The new pieces are in the palette (Scenery tab, or search for their names).
============================================

This window closes in 5 seconds.
```

### First-run setup that finds your game

![Preferences on first run, saying which buttons to press](docs/hoglord/first-run.png)

A Fallout 2 folder named only in the **Game Location** (the Play button's fields, under the data paths) is read as the game's data too. **Auto-Detect** finds Fallout 2 in every Steam library on any drive (`D:\SteamLibrary`) and reads the registry keys GOG and Steam write from their 32-bit installers. While no game is listed, Preferences says which buttons to press.

### Fallout 1 maps

![Fallout 1's Junktown entrance, drawn by this build from the game's own archives](docs/hoglord/fallout1-junktown.png)

Point it at a Fallout 1 install and open its maps: Fallout 1's archives (`MASTER.DAT`, `CRITTER.DAT`, with their LZSS packing) and its version-19 maps, ladders included, are read and saved. Above, Junktown's entrance drawn by `gecko-cli map render`.

### Copy, cut and paste

![A copied part of Arroyo's village following the cursor, ready to drop](docs/hoglord/copy-paste.png)

**Ctrl+C / Ctrl+X / Ctrl+V** copy, cut and paste map areas: floor and roof tiles and objects, onto another elevation or into another map. A ghost follows the cursor and each click drops a copy, until right-click or Esc.

### Right-click menu

![The map's right-click menu in Arroyo's village: Cut, Copy, the draw order, Put on Top and the copy entries](docs/hoglord/right-click-menu.png)

Right-click on the map for Cut / Copy / Paste and the draw order (Bring to Front, Bring Forward, Send Backward, Send to Back; also Home / PgUp / PgDn / End). **Put on Top** makes a piece on a table or shelf show on top of it in the game the way it does in the editor. With the map workflow's tools there are also **Mirrored Copy**, **Bigger or Smaller Copy** and **Name and Examine Text**.

### Black Edge

![The Black Edge tool laying a black edge across Arroyo](docs/hoglord/black-edge.png)

The **Black Edge** tool lays Fallout 2's black map edge: click to start and at each corner, Space puts the black on the other side, and the last point, Enter or right-click finishes it.

### Add and Remove Blocker

![Wall blockers shown in green, with a row just dragged across the village](docs/hoglord/blockers.png)

**Add / Remove Blocker** place or remove wall blockers and scroll blockers: click a hex for one, or drag to stamp a path. Each object is also drawn at its own pixel offset, as the game draws it.

### Smaller changes

- New items hold what the game gives them: an ammo box has its rounds, a weapon its ammo, and a container is saved with room for its items. Ammo amounts are entered in rounds, as they are shown.
- A bag inside a critter's inventory loads instead of failing the map; an object whose art can't be found is skipped instead of stopping the map.
- The file browser's **Only the mod's own files** switch hides what the .dat archives provide, so the list is your mod's maps.
- A data path that can't be loaded is reported and skipped instead of closing gecko.
- `gecko-cli map strip-exit-grids --map maps\arvillag.map --out out.map --data "C:\Games\Fallout 2\master.dat"`.
- Only one copy runs at a time, and the settings are sturdier.

## The map workflow

[`workflow/`](workflow/README.md) holds the whole toolkit I use to make Fallout 2 maps with this build: browsing the original games and the total conversions and saving their buildings as stamps, converted to your mod's numbers; planning a site to scale; building and finishing a map the way Fallout 2 does; putting it in the game. It needs Python 3.10 or newer and Fallout 2 with the Restoration Project Unofficial: open a terminal in `workflow`, run `python setup.py`, then `python foc.py`.

## Building

It builds exactly as gecko does: see [gecko's README](https://github.com/JanSimek/gecko#building-from-source). The pictures on this page are drawn by `qt_tests "[.readme]"` (see `tests/qt/test_readme_shots.cpp`).

## Credits

gecko is Jan Simek's, under the Apache 2.0 licence ([LICENSE](LICENSE)); this build's changes are under the same licence. Fallout is a trademark of Bethesda Softworks LLC. The stamps carry the credits of the games they were cut from.
