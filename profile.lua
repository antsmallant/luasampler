local c = require "luaprofilec"

local M = {
}

-- luacheck: ignore coroutine on_coroutine_destory
local old_co_create = coroutine.create
local old_co_wrap = coroutine.wrap

local function my_coroutine_create(f)
    return old_co_create(function (...)
            c.mark()
            return f(...)
        end)
end

local function my_coroutine_wrap(f)
    return old_co_wrap(function (...)
            c.mark()
            return f(...)
        end)
end

local g_profile_started = false
local g_opts = nil

-- opts = { cpu = "off|profile|sample", mem = "off|profile|sample", cpu_sample_hz = 250 }
function M.start(opts)
    if g_profile_started then
        print("profile start fail, already started")
        return
    end
    g_profile_started = true
    g_opts = opts or { cpu = "profile", mem = "profile", cpu_sample_hz = 250 }
    c.start(g_opts)
    coroutine.create = my_coroutine_create
    coroutine.wrap = my_coroutine_wrap
end

function M.stop()
    if not g_profile_started then
        print("profile stop fail, not started")
        return
    end
    coroutine.create = old_co_create
    coroutine.wrap = old_co_wrap    
    local record_time, nodes = c.dump()
    c.stop()
    g_profile_started = false
    g_opts = nil
    return {time = record_time, nodes = nodes}
end

return M
