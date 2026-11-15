#include "editor/helper/GameDrawOrder.h"

#include "editor/Object.h"
#include "format/map/MapObject.h"

#include <algorithm>
#include <numeric>

namespace geck::game_draw_order {

std::vector<DrawKey> drawKeys(const std::vector<std::shared_ptr<Object>>& objects) {
    std::vector<DrawKey> keys;
    keys.reserve(objects.size());
    for (const auto& object : objects) {
        if (object && object->hasMapObject()) {
            const auto& o = object->getMapObject();
            keys.push_back({ static_cast<int>(o.position), (o.flags & kFlatFlag) != 0 });
        } else {
            keys.push_back({ -1, true });
        }
    }
    return keys;
}

std::vector<std::size_t> drawOrder(const std::vector<DrawKey>& keys) {
    std::vector<std::size_t> order(keys.size());
    std::iota(order.begin(), order.end(), std::size_t{ 0 });
    std::ranges::sort(order, [&keys](std::size_t a, std::size_t b) {
        return drawnBefore(keys[a], a, keys[b], b);
    });
    return order;
}

} // namespace geck::game_draw_order
