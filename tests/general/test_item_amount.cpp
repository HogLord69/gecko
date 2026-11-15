#include <catch2/catch_test_macros.hpp>

#include "format/map/ItemDefaults.h"

// An ammo stack's amount is entered in rounds; the map keeps whole boxes and
// the rounds in the top box, which the game shows as box * (boxes - 1) + top.

using geck::MapObject;
using geck::Pro;

namespace {
Pro ammoPro(uint32_t box) {
    Pro pro{ std::filesystem::path("test.pro") };
    pro.header.PID = 0x00000023; // an item
    pro.setObjectSubtypeId(static_cast<unsigned int>(Pro::ITEM_TYPE::AMMO));
    pro.ammoData.quantity = box;
    return pro;
}

uint32_t shown(const MapObject& o, uint32_t box) {
    return box * (o.amount - 1) + o.ammo;
}
} // namespace

TEST_CASE("Ammo amounts are rounds", "[item][ammo]")
{
    const Pro pro = ammoPro(50);
    MapObject o;

    geck::setItemAmount(o, pro, 1);
    REQUIRE(o.amount == 1u);
    REQUIRE(o.ammo == 1u);

    geck::setItemAmount(o, pro, 25);
    REQUIRE(o.amount == 1u);
    REQUIRE(o.ammo == 25u);

    geck::setItemAmount(o, pro, 50);
    REQUIRE(o.amount == 1u);
    REQUIRE(o.ammo == 50u);

    geck::setItemAmount(o, pro, 60);
    REQUIRE(o.amount == 2u);
    REQUIRE(o.ammo == 10u);
    REQUIRE(shown(o, 50) == 60u);

    geck::setItemAmount(o, pro, 1000);
    REQUIRE(o.amount == 20u);
    REQUIRE(o.ammo == 50u);
    REQUIRE(shown(o, 50) == 1000u);
}

TEST_CASE("Other items count items", "[item]")
{
    Pro pro{ std::filesystem::path("test.pro") };
    pro.header.PID = 0x00000028;
    pro.setObjectSubtypeId(static_cast<unsigned int>(Pro::ITEM_TYPE::DRUG));
    MapObject o;
    geck::setItemAmount(o, pro, 7);
    REQUIRE(o.amount == 7u);
    REQUIRE(o.ammo == 0u);
}
