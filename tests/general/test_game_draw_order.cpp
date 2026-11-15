#include <catch2/catch_test_macros.hpp>

#include <vector>

#include "editor/helper/GameDrawOrder.h"

using geck::game_draw_order::DrawKey;
using geck::game_draw_order::drawnBefore;
using geck::game_draw_order::drawOrder;

// fallout2-ce object.cc _obj_render_pre_roof: flat objects first, then the rest, each in ascending
// hex order; on one hex, load (file) order.
TEST_CASE("game draw order: flat first, then by hex, then by file order", "[game_draw_order]") {
    const std::vector<DrawKey> keys{
        { 500, false }, // 0: a wall late in the file but on an early hex
        { 900, false }, // 1
        { 700, true },  // 2: a flat rug
        { 500, false }, // 3: same hex as 0, after it in the file
        { 100, true },  // 4: flat
    };
    const std::vector<std::size_t> expected{ 4, 2, 0, 3, 1 };
    CHECK(drawOrder(keys) == expected);
}

TEST_CASE("game draw order: file order matters only on one hex", "[game_draw_order]") {
    const DrawKey beam{ 300, false };
    const DrawKey post{ 301, false };
    // the beam later in the file still goes under the post on a later hex
    CHECK(drawnBefore(beam, 9, post, 1));
    CHECK_FALSE(drawnBefore(post, 1, beam, 9));
    // on one hex, the file decides
    CHECK(drawnBefore(post, 1, DrawKey{ 301, false }, 2));
    CHECK_FALSE(drawnBefore(post, 2, DrawKey{ 301, false }, 1));
    // a flat object never goes over a standing one
    CHECK(drawnBefore(DrawKey{ 900, true }, 5, DrawKey{ 1, false }, 0));
}
