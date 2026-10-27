-- One server element lets both platforms prove identity without changing RP resources.
local vehicle
local correlation
local sequence = 0
local probeSequence = 0
local pending = {}
local readyAt = {}
local resourceName = getResourceName(getThisResource())
local runId = tostring(getTickCount())
local probeLifetime = 10000

local function log(message)
    outputServerLog("[mobilefixture] " .. message)
end

local function playerLabel(player)
    if not isElement(player) then
        return "console"
    end
    return getPlayerName(player):gsub("[%c]", "?")
end

local function reply(player, message)
    log(message)
    if isElement(player) then
        outputChatBox("[mobilefixture] " .. message, player, 220, 220, 220)
    end
end

local function sendProbe(player)
    if not isElement(vehicle) or not isElement(player) then
        return
    end
    probeSequence = probeSequence + 1
    local token = runId .. ":" .. tostring(probeSequence)
    pending[player] = { token = token, correlation = correlation, sentAt = getTickCount() }
    triggerClientEvent(player, "mobilefixture:probe", resourceRoot, token, correlation, vehicle)
    log("probe_sent caller=" .. playerLabel(player) .. " correlation=" .. correlation .. " token=" .. token)
    setTimer(function()
        local expected = pending[player]
        if expected and expected.token == token then
            log("probe_timeout caller=" .. playerLabel(player) .. " correlation=" .. expected.correlation .. " token=" .. token)
            pending[player] = nil
        end
    end, probeLifetime, 1)
end

local function cleanup(reason)
    local previous = correlation or "none"
    if isElement(vehicle) then
        destroyElement(vehicle)
    end
    vehicle = nil
    correlation = nil
    pending = {}
    log("cleanup reason=" .. reason .. " correlation=" .. previous)
end

addCommandHandler("mobilefixture", function(player, _, action)
    -- Console is local administration; players must have this explicit ACL right.
    if isElement(player) and not hasObjectPermissionTo(player, "command.mobilefixture", false) then
        reply(player, "denied caller=" .. playerLabel(player))
        return
    end

    action = action or "status"
    local caller = playerLabel(player)
    if action == "cleanup" then
        cleanup("command caller=" .. caller)
        return
    end
    if action == "status" then
        reply(player, "status caller=" .. caller .. " active=" .. tostring(isElement(vehicle)) .. " correlation=" .. (correlation or "none"))
        return
    end
    if action ~= "create" then
        reply(player, "usage: mobilefixture create|cleanup|status")
        return
    end
    if isElement(vehicle) then
        reply(player, "already_active caller=" .. caller .. " correlation=" .. correlation)
        return
    end

    -- A fixed stock location/model removes custom assets from the transport experiment.
    vehicle = createVehicle(411, 1480, -1730, 13.6, 0, 0, 90, "MOBILE")
    if not vehicle then
        reply(player, "create_failed caller=" .. caller)
        return
    end
    sequence = sequence + 1
    correlation = resourceName .. ":" .. runId .. ":" .. tostring(sequence)
    setElementData(vehicle, "mobilefixture:correlation", correlation)
    reply(player, "created caller=" .. caller .. " correlation=" .. correlation .. " model=411 position=1480,-1730,13.6")
    for _, target in ipairs(getElementsByType("player")) do
        sendProbe(target)
    end
end, true, false)

addEvent("mobilefixture:ready", true)
addEventHandler("mobilefixture:ready", root, function()
    -- Remote event source is controlled by its sender; only client identifies the sender.
    if not client or source ~= client or getElementType(client) ~= "player" then
        return
    end
    local now = getTickCount()
    if readyAt[client] and now - readyAt[client] < 5000 then
        return
    end
    readyAt[client] = now
    sendProbe(client)
end)

addEvent("mobilefixture:probeReply", true)
addEventHandler("mobilefixture:probeReply", root, function(token, receivedCorrelation, receivedVehicle)
    if not client or source ~= client or getElementType(client) ~= "player" then
        return
    end
    local expected = pending[client]
    if not expected or token ~= expected.token or receivedCorrelation ~= expected.correlation then
        return
    end
    if getTickCount() - expected.sentAt > probeLifetime or receivedVehicle ~= vehicle or not isElement(vehicle) then
        pending[client] = nil
        return
    end
    pending[client] = nil
    -- This proves an application roundtrip and element reference, not truthful client state.
    log("probe_ack caller=" .. playerLabel(client) .. " correlation=" .. correlation .. " token=" .. token)
end)

addEventHandler("onPlayerJoin", root, function()
    log("join caller=" .. playerLabel(source))
end)

addEventHandler("onPlayerQuit", root, function()
    log("quit caller=" .. playerLabel(source))
    pending[source] = nil
    readyAt[source] = nil
end)

addEventHandler("onResourceStart", resourceRoot, function()
    log("resource_start resource=" .. resourceName .. " run=" .. runId)
end)

addEventHandler("onResourceStop", resourceRoot, function()
    cleanup("resource_stop")
    log("resource_stop resource=" .. resourceName .. " run=" .. runId)
end)
