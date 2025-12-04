root="./"
package.path = package.path .. ";" .. root .. "?.lua"
package.cpath = package.cpath .. ";" .. root .. "?.so"

local profile = require "profile"
local json = require "json"

local g_storage = {}

local function test3()
    local t = {}
    local s = 0
    for i = 1, 10 do
        s = s + i
        t[i] = i
    end
end

local function test2()
    for i = 1, 500000 do
        -- print(i)
        tonumber("123")    
        test3()
    end    
end

local function test22()
    for i = 1, 500000 do
        -- print(i)
        tonumber("123")    
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

local function write_file(path, content)
    local f, err = io.open(path, "w")
    if not f then
        print("write_file open fail:", err)
        return false
    end
    f:write(content or "")
    f:close()
    return true
end

local function test1()
    local opts = { cpu = "sample", mem = "sample", cpu_sample_hz = 250 }
    profile.start(opts)
    test_storage1()
    test_storage2()
    for i = 1, 3000 do
        tonumber("123")    
    end
    print("111")
    tonumber("234")
    print("222")
    test2()
    test22()
    test_vccl()
    local result = profile.stop()
    print("time:",result.time)
    print("nodes:")
    print(result.nodes)
    write_file("cpu-samples.txt", result.nodes)
end

local function test()
    test1()
end

test()