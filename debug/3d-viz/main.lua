local points = {}

local WINDOW_SIZE = 32 -- 32 samples sliding window
local SIZE = 100
local BOTTOM = -SIZE

local stdin_chan, kill_chan
local thread

local function dupePoint(pt)
    return {
        assert(pt[1]), -- 
        assert(pt[2]), -- 
        assert(pt[3]) -- 
    }
end

local function modifyInplace(inval, val, app)
    assert(inval)
    assert(val)
    assert(app)
    for i = 1, 3 do
        inval[i] = app(inval[i], val[i]) --
    end

end

local function computeStats()
    local stats = {
        min = {0, 0, 0}, --
        max = {0, 0, 0}, -- 
        avg = {0, 0, 0}, --
        stddev = 0, --
        nil
    }

    for i = 1, #points do
        local pt = points[i]

        if i == 1 then
            stats.min = dupePoint(pt)
            stats.max = dupePoint(pt)
            stats.avg = dupePoint(pt)
        else
            modifyInplace(stats.min, pt,
                          function(old, new)
                return math.min(old, new)
            end)
            modifyInplace(stats.max, pt,
                          function(old, new)
                return math.max(old, new)
            end)
            modifyInplace(stats.avg, pt, function(old, new)
                return old + new
            end)
        end

    end

    modifyInplace(stats.avg, {0, 0, 0},
                  function(old, new) return old / #points end)

    for i = 1, #points do
        local pt = points[i]

        local dx = pt[1] - stats.avg[1]
        local dy = pt[2] - stats.avg[2]
        local dz = pt[3] - stats.avg[3]

        local delta = dx * dx + dy * dy + dz * dz;

        stats.stddev = stats.stddev + delta / #points
    end

    stats.stddev = math.sqrt(stats.stddev)

    return stats
end

function love.load()

    love.graphics.setFont(love.graphics.newFont(24))

    stdin_chan = love.thread.newChannel()
    kill_chan = love.thread.newChannel()

    thread = love.thread.newThread [[
      local stdin_chan,kill_chan = ...

      print(stdin_chan)

        while true do
          if kill_chan:pop() then
            return
          end
          local line = io.read("*line")
          if line then
            local sx,sy,sz = line:match("debug/sensor/magnetometer%s+(-?%d+%.%d+)%s+(-?%d+%.%d+)%s+(-?%d+%.%d+)")
            local x,y,z = tonumber(sx),tonumber(sy),tonumber(sz)

            if x ~= nil and y ~= nil  and z ~= nil then
              stdin_chan:push({x,y,z})
            end
          else
            return
          end
        end
    ]]

    thread:start(stdin_chan, kill_chan)

end

function love.quit() kill_chan:push(true) end

function love.update(dt)

    if not thread:isRunning() then love.event.quit() end

    while true do
        local point = stdin_chan:pop()

        if point then
            point.alpha = 1.0
            points[#points + 1] = point

            if #points > WINDOW_SIZE then table.remove(points, 1) end
        else
            break
        end
    end

    for i = 1, #points do
        local p = points[i];
        p.alpha = i / #points
    end

end

function love.keypressed(key)
    if key == "space" then

        local stats = computeStats()

        print(string.format("Min = (%.2f,%.2f,%.2f)", stats.min[1],
                            stats.min[2], stats.min[3]))

        print(string.format("Max = (%.2f,%.2f,%.2f)", stats.max[1],
                            stats.max[2], stats.max[3]))

        print(string.format("Avg = (%.2f,%.2f,%.2f)", stats.avg[1],
                            stats.avg[2], stats.avg[3]))
    end
end

function project(x, y, z)
    local w, h = love.graphics.getDimensions()

    local cx, cy = w / 2, h / 2

    local zoom = 100

    local angle = 30 * math.pi / 180

    local x_dx = math.cos(angle)
    local x_dy = math.sin(angle)
    local y_dx = math.cos(angle + math.pi / 2)
    local y_dy = math.sin(angle + math.pi / 2)

    return cx + x_dx * zoom * x + y_dx * zoom * y,
           cy + x_dy * zoom * x + y_dy * zoom * y - zoom * z

end

function love.draw()
    love.graphics.clear(0, 0, 0)

    local corners = {
        {-1, -1, -1}, -- 
        {-1, -1, 1}, -- 
        {-1, 1, -1}, -- 
        {-1, 1, 1}, -- 
        {1, -1, -1}, -- 
        {1, -1, 1}, -- 
        {1, 1, -1}, -- 
        {1, 1, 1} -- 
    }

    local edges = {
        {0, 1}, -- 
        {1, 3}, -- 
        {3, 2}, -- 
        {2, 0}, -- 
        {4, 5}, -- 
        {5, 7}, -- 
        {7, 6}, --  
        {6, 4}, -- 
        {0, 4}, -- 
        {1, 5}, -- 
        {2, 6}, -- 
        {3, 7} --
    }

    for i = 1, #points do
        local point = points[i]

        local px, py =
            project(point[1] / SIZE, point[2] / SIZE, point[3] / SIZE)
        local bx, by = project(point[1] / SIZE, point[2] / SIZE, BOTTOM / SIZE)

        love.graphics.setColor(1, 1, 1, 0.2 * point.alpha)
        love.graphics.line(px, py, bx, by)

        love.graphics.setColor(0.4, 0.4, 0.4, point.alpha)
        love.graphics.setPointSize(2)
        love.graphics.points(bx, by)

        love.graphics.setColor(1, 0, 0, point.alpha)
        love.graphics.setPointSize(4)
        love.graphics.points(px, py)

    end

    love.graphics.setColor(0.3, 0.3, 0.3)

    for i = 1, #edges do
        local edge = edges[i]

        local p_start = corners[edge[1] + 1]
        local p_end = corners[edge[2] + 1]

        local px1, py1 = project(p_start[1], p_start[2], p_start[3])
        local px2, py2 = project(p_end[1], p_end[2], p_end[3])
        love.graphics.line(px1, py1, px2, py2)
    end

    local w, h = love.graphics.getDimensions()

    local slots = {
        {h - 300, h - 200, 1, 0, 0}, --
        {h - 200, h - 100, 0, 1, 0}, --
        {h - 100, h - 000, 0, 0, 1} --
    }

    for i = 1, #points - 1 do

        local p0 = points[i + 0]
        local p1 = points[i + 1]

        local x0 = (i - 1) * w / #points
        local x1 = (i - 0) * w / #points

        for j = 1, 3 do
            local slot = slots[j]

            local v0 = p0[j]
            local v1 = p1[j]

            local mid = (slot[1] + slot[2]) / 2
            local sh = (slot[2] - slot[1]) / 2

            local y0 = mid + sh * v0 / SIZE
            local y1 = mid + sh * v1 / SIZE

            love.graphics.setColor(slot[3], slot[4], slot[5])
            love.graphics.line(x0, y0, x1, y1)
        end

    end

    love.graphics.setColor(1, 1, 1)
    local stats = computeStats()

    love.graphics.print(string.format("Min = (%.2f,%.2f,%.2f)", stats.min[1],
                                      stats.min[2], stats.min[3]), 10, 10)

    love.graphics.print(string.format("Max = (%.2f,%.2f,%.2f)", stats.max[1],
                                      stats.max[2], stats.max[3]), 10, 30)

    love.graphics.print(string.format("Avg = (%.2f,%.2f,%.2f)", stats.avg[1],
                                      stats.avg[2], stats.avg[3]), 10, 50)

    love.graphics.print(string.format("Siz = (%.2f,%.2f,%.2f)",
                                      stats.max[1] - stats.min[1],
                                      stats.max[2] - stats.min[2],
                                      stats.max[3] - stats.min[3]), 10, 70)

    love.graphics.print(string.format("Dev = %.3f", stats.stddev), 10, 90)
end

