#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace geck {

class Object;

/// The order the game draws an elevation's objects in, which gecko draws and picks by.
///
/// The engine (fallout2-ce object.cc, _obj_render_pre_roof) draws every flat object first, then
/// everything else, each pass hex by hex in ascending hex number. Objects on the same hex are
/// drawn in the order they were loaded, which is map-file order (_obj_insert appends). So the
/// file order only decides between objects that share a hex: two overlapping objects on
/// different hexes are stacked by their hexes, whatever their places in the file.
namespace game_draw_order {

    /// What decides where an object is drawn: its hex and whether it is flat (object flag 0x08).
    struct DrawKey {
        int hex = -1;
        bool flat = false;
    };

    constexpr std::uint32_t kFlatFlag = 0x08;

    /// True when the game draws `a` (at file index ia) before `b` (at file index ib).
    inline bool drawnBefore(const DrawKey& a, std::size_t ia, const DrawKey& b, std::size_t ib) {
        if (a.flat != b.flat) {
            return a.flat;
        }
        if (a.hex != b.hex) {
            return a.hex < b.hex;
        }
        return ia < ib;
    }

    /// Indices into `keys` (given in file order) in the order the game draws them.
    std::vector<std::size_t> drawOrder(const std::vector<DrawKey>& keys);

    /// The keys of an elevation's objects (EditorSession::objects(), in file order). An object
    /// without map data sorts first, as if on no hex.
    std::vector<DrawKey> drawKeys(const std::vector<std::shared_ptr<Object>>& objects);

} // namespace game_draw_order

} // namespace geck
