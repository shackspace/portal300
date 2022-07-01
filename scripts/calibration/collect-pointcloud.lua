-- Usage:
-- ./mosquitto-log.sh | lua ./collect-pointcloud.lua ${COUNT} > data.ply
-- will collect ${COUNT} number of points into data.ply
-- This file can then be imported into blender with the "Point Cloud Visualizer" plugin.
-- This plugin is also shipped in this folder and has a separate licence.
--
local collection = {}

local point_count = assert(tonumber(arg[1]), "arg[1] must be a number!")

local out = io.stdout

out:write(([[ply
format ascii 1.0
element vertex %d
property float x
property float y
property float z
property uint8 red
property uint8 green
property uint8 blue
end_header
]]):format(point_count))

while true do
    local line = io.read("*line")
    if line then
        local sx, sy, sz = line:match(
                               "debug/sensor/magnetometer/raw%s+(-?%d+%.%d+)%s+(-?%d+%.%d+)%s+(-?%d+%.%d+)")
        local x, y, z = tonumber(sx), tonumber(sy), tonumber(sz)

        if x ~= nil and y ~= nil and z ~= nil then

            collection[#collection + 1] = {x = x, y = y, z = z}

            out:write(("%f %f %f 128 128 128\n"):format(x, y, z))

            io.stderr:write("          \rprogress: " .. tostring(#collection) ..
                                "/" .. tostring(point_count))
            io.stderr:flush()

            if #collection >= point_count then break end

        end
    else
        return
    end
end

io.stderr:write("\n")

io.stderr:write("done. press ctrl-c now!\n");
