# Building stamps

Ready-made gecko stamps (Edit → Stamp Pattern) of whole buildings, cut from the maps of Fallout 1, Fallout 2 and four total conversions. Each stamp is one complete building: walls, doors, floor and roof, with its furniture and containers. There are no critters, no loose items and no ground around it.

| Folder | Stamps | Stamp ids are | Open this game in gecko |
|---|---|---|---|
| `Fallout Buildings` | 222 | Fallout 2's (Fallout 1 buildings converted by matching art) | Fallout 2, with or without the Restoration Project |
| `Sonora Buildings` | 257 | Fallout: Sonora's own | Fallout: Sonora |
| `Nevada Buildings` | 160 | Fallout: Nevada's own | Fallout: Nevada |
| `Resurrection Buildings` | 91 | Fallout 1.5: Resurrection's own | Fallout 1.5: Resurrection |
| `Olympus Buildings` | 27 | Olympus 2207's own | Olympus 2207 |

A stamp stores the game's proto and art ids, not images. It only draws correctly in a gecko session with the game data listed above; stamped into another game, the same ids point at different pieces.

## Installing

Copy the folders you want into gecko's pattern library:

- Windows: `%LOCALAPPDATA%\gecko\gecko\patterns\`
- Linux: `~/.config/gecko/patterns/`
- macOS: `~/Library/Preferences/gecko/patterns/`

The library reads subfolders, so each set keeps its layout in the stamp browser.

## Layout

`<construction>/<size>/<floor>/<Game> <MAP> <n> <floor>.json`

- **Construction:** the building's main wall type. The folders are Concrete Block, Adobe, Wood & Scrap, Tents, Corrugated Metal, Military & Metal, Stone & Plaster, Vault, Caves & Rock, Fences & Stockades, and Ruins & Overgrown.
- **Size:** by roofed floor area. Small is under 60 squares, Medium 60–149, Large 150–399, and Huge 400 or more.
- **Floor:** the map elevation the building came from: 1st, 2nd or 3rd Floor. To build a multi-storey building, stamp a 2nd or 3rd floor stamp on that elevation, over the 1st floor stamp with the same map name and number. Some maps use an upper elevation as a separate area, so not every "2nd Floor" stamp is an upper storey.

Every stamp carries a `credit` line and a `source` block with its game, map and elevation. Gecko ignores both.

## What counts as complete

A building is kept only if it has walls all the way round its roof, with gaps no longer than a doorway. It is left out if its roof no longer covers the area inside its walls (a ruin), and fence runs attached to a building are dropped. Identical buildings that recur across maps appear once. Fallout 1 buildings that use art Fallout 2 does not have are left out.

## Credits

The layouts are the work of the people who made these games and mods. Please keep these credits with the stamps.

- **Fallout (1997):** Interplay Productions / Black Isle Studios.
- **Fallout 2 (1998):** Black Isle Studios / Interplay. The maps are taken as they stand in the Restoration Project by killap, now maintained as RPU by BGforge.
- **Fallout: Sonora (2020–2021):** Alexander "BLACK DESIGNER" Poshelyuzhin and Alexander "Red888guns" Berezin, with Osman "Wolfram" Adzhiusmanov, Grigory Sokolov, Denis "MASTER" Voloshin, Dmitry Kuznetsov, Foxx and Lexx.
- **Fallout: Nevada (2009–2015):** Alexander "BLACK DESIGNER" Poshelyuzhin and the Nevada team: Denis "Master" Voloshin, Anton "SAMIRAKUS" Smirnov, Osman "Wolfram" Ajiusmanov, Alexey Osadchiy, X'IL, Pixote, Mr.Stalin, Dmitriy Yermak, Pyran and Foxx.
- **Fallout 1.5: Resurrection:** the Resurrection team ([resurrection.cz](http://resurrection.cz/en)).
- **Olympus 2207:** Nebesa Games. Artem "Rainman" Samoilov, Alexander "Saur" Berezin, Sergey "Zoomer" Bokarev and Elena Samoilova; English translation by keyboard gecko.

Fallout is a trademark of Bethesda Softworks LLC. These stamps contain object ids and positions only, no game art.

Stamps cut by HogLord's `building_stamps.py`.
