#pragma once

#include <map>

#include "reader/FileParser.h"
#include "format/map/MapObject.h"
#include "format/map/MapScript.h"
#include "format/lst/Lst.h"

namespace geck {

class Map;
class Pro;
class Tile;

class MapReader : public FileParser<Map> {
public:
    MapReader(std::function<Pro*(uint32_t PID)> proLoadCallback);

private:
    std::unique_ptr<MapObject> readMapObject();
    void readInventory(MapObject& object);
    MapScript::ScriptType fromPid(uint32_t val);

    std::function<Pro*(uint32_t PID)> _proLoadCallback;
    // 19 = Fallout 1, 20 = Fallout 2. Ladders differ between them.
    uint32_t _mapVersion = 20;

public:
    std::unique_ptr<Map> read() override;
};

} // namespace geck
