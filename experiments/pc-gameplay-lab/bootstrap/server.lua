-- The shared-world fixture intentionally does not own an RP spawn system.
-- This separate ordinary MTA resource makes its stock test location playable.
local function log(message)
    outputServerLog("[mobilelab-spawn] " .. message)
end

local function spawn(player)
    if not isElement(player) or getElementType(player) ~= "player" then
        return
    end
    local ok = spawnPlayer(player, 1480, -1720, 13.6, 180, 0, 0, 0)
    if not ok then
        log("spawn_failed name=" .. getPlayerName(player))
        return
    end
    fadeCamera(player, true)
    setCameraTarget(player, player)
    log("spawned name=" .. getPlayerName(player) .. " position=1480,-1720,13.6")
end

addEventHandler("onPlayerJoin", root, function()
    spawn(source)
end)

addEventHandler("onResourceStart", resourceRoot, function()
    local players = getElementsByType("player")
    for _, player in ipairs(players) do
        spawn(player)
    end
    log("resource_start players=" .. tostring(#players))
end)
