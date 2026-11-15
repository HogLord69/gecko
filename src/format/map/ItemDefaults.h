#pragma once

#include "format/map/MapObject.h"
#include "format/pro/Pro.h"

namespace geck {

// A new item's own data, from its proto, as the engine sets it when it makes
// one (fallout2-ce proto.cc _proto_update_init): a weapon loaded to capacity
// with its ammo type, a full box of ammo, a misc item's charges, a key's code.
// Without this every item gecko made held nothing: ammo boxes with 0 rounds
// (shown as an amount of 0, and an empty box in game) and unloaded weapons.
inline void applyItemDefaults(MapObject& object, const Pro& pro) {
    if (pro.type() != Pro::OBJECT_TYPE::ITEM) {
        return;
    }
    switch (pro.itemType()) {
        case Pro::ITEM_TYPE::WEAPON:
            object.ammo = pro.weaponData.ammoCapacity;
            object.ammo_pid = static_cast<uint32_t>(pro.weaponData.ammoPID);
            break;
        case Pro::ITEM_TYPE::AMMO:
            object.ammo = pro.ammoData.quantity;
            break;
        case Pro::ITEM_TYPE::MISC:
            object.ammo = pro.miscData.charges;
            break;
        case Pro::ITEM_TYPE::KEY:
            object.keycode = pro.keyData.keyId;
            break;
        default:
            break;
    }
}

inline bool isAmmoProto(const Pro& pro) {
    return pro.type() == Pro::OBJECT_TYPE::ITEM && pro.itemType() == Pro::ITEM_TYPE::AMMO;
}

// An ammo stack's amount is counted in rounds, as the game shows it: the
// engine keeps whole boxes (amount) and the rounds in the top box (ammo), and
// shows box size * (boxes - 1) + top box (fallout2-ce inventory.cc). So 1 is
// one box with 1 round in it, 50 of a 50-round box is one full box, and 60
// is a full box and a box of 10.
inline void setAmmoRounds(MapObject& object, const Pro& pro, uint32_t rounds) {
    const uint32_t box = pro.ammoData.quantity > 0 ? static_cast<uint32_t>(pro.ammoData.quantity) : 1;
    if (rounds < 1) {
        rounds = 1;
    }
    const uint32_t boxes = (rounds + box - 1) / box;
    object.amount = boxes;
    object.ammo = rounds - box * (boxes - 1);
}

// Sets how many of an item a stack holds: rounds for ammo, items for the rest.
inline void setItemAmount(MapObject& object, const Pro& pro, uint32_t amount) {
    if (isAmmoProto(pro)) {
        setAmmoRounds(object, pro, amount);
    } else {
        object.amount = amount < 1 ? 1 : amount;
    }
}

} // namespace geck
