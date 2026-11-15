#pragma once

#include <functional>
#include <memory>
#include <vector>

namespace geck {

struct MapObject;

/// The Step names the right-click "Bring to Front / Send to Back" commands use, and a file-order
/// reorder.
///
/// gecko draws in the game's order (editor/helper/GameDrawOrder.h): hex by hex, the file order
/// only deciding between objects on the SAME hex. So the editor's commands
/// (EditorWidget::reorderSelectedObjects) move an object's hex, not its place in the file;
/// `reordered` below changes only the file order, which shows only between objects on one hex.
namespace object_order {

    enum class Step {
        BringToFront,
        BringForward,
        SendBackward,
        SendToBack,
    };

    /// True when two objects' drawn images cover each other. Forward/Backward hop over the
    /// nearest object this accepts, so a step is always one visible change; without a
    /// predicate they hop over the nearest unselected object.
    using Overlaps = std::function<bool(const MapObject& a, const MapObject& b)>;

    /// The elevation's object list after moving `selected` one step. Selected objects keep
    /// their order relative to each other; everything unselected keeps its order. An object
    /// with nothing to hop over stays where it is.
    std::vector<std::shared_ptr<MapObject>> reordered(
        const std::vector<std::shared_ptr<MapObject>>& order,
        const std::vector<std::shared_ptr<MapObject>>& selected,
        Step step,
        const Overlaps& overlaps = {});

} // namespace object_order

} // namespace geck
