root="./"
package.path = package.path .. ";" .. root .. "?.lua"
package.cpath = package.cpath .. ";" .. root .. "?.so"

local profile = require "profile"
local json = require "json"
local c = require "luaprofilec"

-- 带时间戳的打印：[YYYY-MM-DD HH:MM:SS] 后跟原始内容
local function print_ts(...)
    local ts = os.date("%Y-%m-%d %H:%M:%S")
    local parts = {}
    for i = 1, select("#", ...) do
        parts[i] = tostring(select(i, ...))
    end
    io.stdout:write("[" .. ts .. "] ", table.concat(parts, "\t"), "\n")
end

local g_storage = {}

local function write_profile_result(str)
    local file, err = io.open(root .. "profile_result.txt", "a")
    if not file then
        io.stderr:write("open file failed: " .. tostring(err) .. "\n")
        return false
    end
    file:write(str, "\n")
    file:close()
    return true
end

local function test3()
    local t = {}
    local s = 0
    for i = 1, 10000 do
        s = s + i
        table.insert(t, i)
    end
end

local function test2()
    for i = 1, 100 do
        test3()
    end    
end

-- 触发 LUA_VCCL：string.gmatch 返回 C 闭包迭代器（带 upvalues）
local function test_vccl()
    local acc = 0
    for w in string.gmatch("foo bar baz", "%S+") do
        acc = acc + #w
    end
    return acc
end

local function test_storage1()
    for i = 1, 100 do
        table.insert(g_storage, i)
    end
end

local function test_storage2()
    for i = 1, 10 do
        table.insert(g_storage, i)
    end
    g_storage = {} 
    collectgarbage("collect")
    for i = 1, 100 do
        table.insert(g_storage, i)
    end
end

local function do_test1()
    test_storage1()
    test_storage2()
    tonumber("123")    
    print("111")
    tonumber("234")
    print("222")
    test2()
    test_vccl()    
end

local function test_11()
    local t = {}
    for i = 1, 100000 do
        table.insert(t, i)
    end
    return t
end

local function test_12()
    local t = {}
    for i = 1, 100000 do
        table.insert(t, i)
    end
    return t
end

local function do_test()
    test_11()
    test_12()
end

local function test_with_profile()
    print_ts("test_with_profile start")
    local opts = { cpu = "profile", mem = "profile", cpu_sample_hz = 250 }
    profile.start(opts)
    local t1 = c.getnanosec()
    do_test()
    local t2 = c.getnanosec()
    local result = profile.stop()
    local strResult = json.encode(result)
    print(strResult)
    write_profile_result(strResult)
    print_ts("test_with_profile stop")
    return t2 - t1
end

local function test_without_profile()
    local t1 = c.getnanosec()
    do_test()
    local t2 = c.getnanosec()
    return t2 - t1
end

local cost_without_profile = test_without_profile()
local cost_with_profile = test_with_profile()
print("test_with_profile cost:", cost_with_profile)
print("test_without_profile cost:", cost_without_profile)
print("test_with_profile cost / test_without_profile cost:", cost_with_profile / cost_without_profile)