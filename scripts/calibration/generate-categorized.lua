-- Usage:
-- cat point-categorization.gltf | lua ./generate-categorized.lua
--
-- Setup:
-- sudo luarocks install lunajson
-- 
-- Filters the GLTF file and extracts all objects with names that start with 'open', 'closed' and 'locked'.
-- The translation and scale of these objects will then be translated into a C structure which can be compiled
-- into the door-control firmware.
--
luna = require "lunajson"

local json_raw = io.read("*all")

local data = assert(luna.decode(json_raw))

local out = io.stdout

out:write("static const struct SensorClassification well_known_vectors[] = {\n")
out:write("    // generated data:\n")

for i = 1, #data.nodes do
    local node = data.nodes[i]

    local group =
        node.name:match("(open).*") or node.name:match("(closed).*") or
            node.name:match("(locked).*")

    if group then
        local x, y, z = node.translation[1] or 0, node.translation[2] or 0,
                        node.translation[3] or 0
        local sx, sy, sz = node.translation[1] or 0, node.translation[2] or 0,
                           node.translation[3] or 0

        local scale = (sx + sy + sz) / 3

        local function ok(val)
            return math.abs(val - scale) >= scale * 0.1
        end

        if not ok(sx) or not ok(sy) or not ok(sz) then
            io.stderr:write("scale of object ", node.name,
                            " is not uniform. please check!\n")
        end

        out:write(
            ("    (struct SensorClassification) { .door_state = DOOR_%s, .location = { .x = %f, .y = %f, .z = %f }, .radius2 = %f },\n"):format(
                group:upper(), x, y, z, scale * scale))

        -- print(group, node.name, x, y, z, node.scale[1])
    end
end

out:write("};\n")
out:write("\n")
