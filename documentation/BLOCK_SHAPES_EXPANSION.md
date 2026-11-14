# Expanded block shapes, variants and crafting

This branch extends OptiCraft's existing data-driven block registry rather than replacing
its renderer, player physics or crafting UI. It remains a voxel game: a non-cubic block
uses up to 16 axis-aligned boxes with coordinates in the 16-pixel block grid. The CPU
collision/raycast and GPU mesher build their geometry from the same `shape_boxes` function.

## Shapes

The block shape registry (`Block_Shape` in `src/world/blocks/Block_Types.h`) contains
30 selectable shapes: slab, stairs, plate, wire, button, fence, wall, fence gate, door,
trapdoor, pressure plate, pane, ladder, carpet, snow layer, standing sign, hanging sign,
rail, chain, lantern, cauldron, hopper, bed, candle, rod, flower pot, bell, grindstone,
lectern and upright torch.

Data-backed examples live in `assets/blocks.json`. To add a material variant, register a
unique numeric ID, `mesh_type: "Shaped"`, `shape: "fence"` (or another supported shape),
face textures and the other usual block properties. Recipes are stored in
`assets/recipe.json`, in the same format as the original items. The corresponding `.ocb`
files have been regenerated and must be updated alongside the JSON files; the conversion
tool is `tools/json2bin/json2bin.cpp`.

## Material families

All 16 wood species already supported by the project have stairs and slabs, and now
also get a fence, fence gate, door, trapdoor, pressure plate, button, sign and hanging
sign. Other additions include stone and cobblestone variants, brick, glass/panes,
metal and weighted pressure plates, rails, lights, thin decor and functional/redstone
items. Basic missing crafting inputs (wool, glass, brick) have been added to the block
catalog; glass is smelted from sand, and white wool drops from sheep.

The texture atlas has extra procedural 16x16 tiles for the newly introduced materials.
Textures deliberately match the pixel-art style of the existing game; they are not
exact Minecraft texture copies.

## Placement, interaction and metadata

- Stairs rotate and form inside/outside corners based on neighbors.
- Fences, walls and panes connect to compatible neighbors and solid cubes; connections
  are computed when meshing, colliding and raycasting, not stored in chunk metadata.
- Doors place both their halves from a single item, swing open/closed on right click,
  and remove the paired half when destroyed, including explosions.
- Fence gates and trapdoors open/close on right click.
- All wood/stone/metal button variants can be placed on a supporting block; they pulse
  the existing logic network for a short time when clicked.
- Ladders require a vertical supporting face; carpet, rails, snow and pressure plates
  require a supporting floor.
- New shape variants have selectable raycast hitboxes, including nonsolid decorations.

Bit layout: stairs and facing blocks use bits 0-1, gate/door/trapdoor bit 2 is open,
door bit 3 is upper-half; for buttons bits 0-2 encode the support face and bit 3 encodes
the pressed state. Fence/wall/pane use a *derived* 4-bit horizontal connection mask.

## Scope and limitations

This expansion prioritizes meshes, block types, material variants and crafting—the four
requested features. It does **not** recreate all vanilla Minecraft gameplay systems:
pressure plates do not yet detect entities; beds do not implement sleeping; rails lack
minecart physics; signs do not have editable text; hoppers lack inventory transfer;
cauldrons lack filling mechanics. Their shapes and recipes are nevertheless available.
Beds occupy one voxel in this implementation. It also does not include every rare,
entity-driven or custom-animated block model from the latest Minecraft versions.

## Validation

Run from the project root:

```bash
python3 tools/validate_block_catalog.py
```

It verifies unique IDs, shape names, sprite references and crafting ingredient/output
names. The geometry routine was also tested as standalone C++ for all 30 shapes and all
256 metadata states, validating box coordinates and the 16-box limit. A full in-game
build requires the project's OpenGL/GLFW/GLM development dependencies and a working GPU
context; those dependencies were unavailable in the build environment used for this
change, so a successful end-to-end launch is **not** claimed.
