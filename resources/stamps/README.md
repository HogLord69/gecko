# Stamps

Ready-made gecko stamps (Edit → Stamp Pattern) cut from the maps of Fallout 1, Fallout 2 and four total conversions: whole buildings, graveyards, junkyards and ruins. There are no critters and no loose items.

| Folder | Stamps | Stamp ids are |
|---|---|---|
| `Fallout 2` | 424 | Fallout 2's (its Fallout 1 buildings were converted by matching art) |
| `Fallout 1` | 124 | Fallout 1's own |
| `Sonora` | 437 | Fallout: Sonora's own |
| `Nevada` | 310 | Fallout: Nevada's own |
| `Resurrection` | 193 | Fallout 1.5: Resurrection's own |
| `Olympus` | 79 | Olympus 2207's own |
| `HogLord Props` | 21 | Fallout 2's. My own compositions: barrels, car junk, a reactor, relay walls, tents, a trash house, a warehouse, adobe huts, a clinic, crops, a farm, a pen, ruins, a tavern and a well |

A stamp stores its game's proto and art ids, not images: stamped into another game, the same ids point at different pieces. The HogLord Build of Gecko lists these folders by itself (Edit → Stamp Pattern, "Came with gecko"). `Fallout 2` and `HogLord Props` stamp as they are in Fallout 2; another game's folder is drawn with that game's own data, and a stamp picked from it is converted into your mod first, once the map workflow knows where that game is installed (`python setup.py` in `workflow`). In another build of gecko, copy the folders you want into its pattern library (`%LOCALAPPDATA%\gecko\gecko\patterns\` on Windows) and open the game each folder is in.

## Layout

The same for every game; a folder with no stamps for a game is left out:

```
<Game>/Buildings/<construction>/<Small|Medium|Large|Huge>/<place> #<n>.json
<Game>/Graveyards/<place> #<n>.json
<Game>/Junkyards/<place> #<n>.json
<Game>/Ruins/<Small|Medium|Large>/<place> #<n>.json
```

`<place>` is the map's name in its game's own map list (`maps.txt`), so "Arroyo Village #2" is the second stamp cut from Arroyo's village map. Fallout 1 has no such list, so its stamps are named after the map file ("HUBDWNTN #1"). Each game's `INDEX.md` lists every stamp with the map it came from and its size.

**Buildings** are sorted by what they are built from: Adobe, Brick & Concrete, Caves & Rock, Corrugated Metal, Military & Metal, Stone & Plaster, Tents, Vault, and Wood & Scrap. The folder is decided by the wall art the building really uses, and every building was checked by eye. Then by size, in roofed squares: Small under 60, Medium under 150, Large under 400, Huge. A building comes with its walls, doors, floor and roof tiles, furniture and containers. It is kept only if its walls go all the way round its roof, with gaps no longer than a doorway, and its roof still covers the ground inside its walls. A stamp named with a 2nd or 3rd floor is a building from that elevation of its map: stamp it on that elevation. It may be an upper storey or an area of its own; a map's buildings are numbered straight through its elevations, so it is not tied to one 1st floor stamp.

**Graveyards** are clusters of graves, headstones, crosses and coffins, with the fence round them when there is one, and the gates, posts, benches and dead trees inside it.

**Junkyards** are clusters of junk piles, tires, wrecked cars and crates, with their fence if they have one.

**Ruins** are broken walls: corners, L and U shapes, buildings with walls missing, with the debris and rubble beside them. Intact roofless buildings, tents, vault and cave walls, railings and compound fences are left out, and so are underground maps.

Graveyards, junkyards and ruins are objects only, with no ground tiles, so they drop onto whatever ground the map already has. Identical stamps that recur across maps appear once. Hidden script markers and "UNUSED ART" placeholder pieces are left out.

Ruins are sized by the hexes their pieces stand on: Small under 30, Medium under 50, Large.

Every stamp carries a `credit` line, a `source` block with its game, map and elevation, and a `library` block with where it belongs in this layout. Gecko ignores all three. The layout is made by `organize_stamps.py`, which can be run again after stamps are added.

## Credits

The layouts are the work of the people who made these games and mods. Please keep these credits with the stamps.

- **Fallout (1997):** Interplay Productions / Black Isle Studios.
- **Fallout 2 (1998):** Black Isle Studios / Interplay. The maps are taken as they stand in the Restoration Project by killap, now maintained as RPU by BGforge.
- **Fallout: Sonora (2020–2021):** Alexander "BLACK DESIGNER" Poshelyuzhin and Alexander "Red888guns" Berezin, with Osman "Wolfram" Adzhiusmanov, Grigory Sokolov, Denis "MASTER" Voloshin, Dmitry Kuznetsov, Foxx and Lexx.
- **Fallout: Nevada (2009–2015):** Alexander "BLACK DESIGNER" Poshelyuzhin and the Nevada team: Denis "Master" Voloshin, Anton "SAMIRAKUS" Smirnov, Osman "Wolfram" Ajiusmanov, Alexey Osadchiy, X'IL, Pixote, Mr.Stalin, Dmitriy Yermak, Pyran and Foxx.
- **Fallout 1.5: Resurrection:** the Resurrection team ([resurrection.cz](http://resurrection.cz/en)).
- **Olympus 2207:** Nebesa Games. Artem "Rainman" Samoilov, Alexander "Saur" Berezin, Sergey "Zoomer" Bokarev and Elena Samoilova; English translation by keyboard gecko.

Fallout is a trademark of Bethesda Softworks LLC. These stamps contain object ids and positions only, no game art.

Stamps cut by HogLord's `building_stamps.py` and `area_stamps.py`.
