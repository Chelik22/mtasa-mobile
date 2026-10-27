-- Observe real server elements/events; never create fake players to satisfy H1.
local enabled = false
local vehicle
local correlation
local sequence = 0
local probeSequence = 0
local connectionSequence = 0
local connections, pending, acknowledged = {}, {}, {}
local joins, quits, created, destroyed, acks, expired, rejected = 0, 0, 0, 0, 0, 0, 0
local resourceName = getResourceName(getThisResource())
local run = resourceName .. ":" .. tostring(getRealTime().timestamp) .. ":" .. tostring(getTickCount())
local lifetime = 10000

local function isConsoleInvocation(source)
    -- CRegisteredCommands passes CConsoleClient as an element in this MTA tree.
    -- Retain legacy false/nil calls while rejecting player and other elements.
    return source == false or source == nil or (isElement(source) and getElementType(source) == "console")
end

local function emit(event, fields)
    fields = fields or {}
    fields.event = event
    fields.run = run
    -- MTA's own JSON encoder safely escapes player names; consumers parse after this prefix.
    outputServerLog("[headlesswitness] " .. toJSON(fields, true))
end

local function identify(player)
    if not connections[player] then
        connectionSequence = connectionSequence + 1
        connections[player] = connectionSequence
    end
    return connections[player]
end

local function invalidate(player)
    local request = pending[player]
    if request and isTimer(request.timer) then
        killTimer(request.timer)
    end
    pending[player] = nil
    acknowledged[player] = nil
end

local function cleanup(reason)
    local previous = correlation
    for player in pairs(pending) do
        invalidate(player)
    end
    acknowledged = {}
    if isElement(vehicle) then
        if not destroyElement(vehicle) or isElement(vehicle) then
            emit("cleanup_failed", { reason = reason, correlation = previous or false })
            return
        end
        destroyed = destroyed + 1
    end
    vehicle, correlation = nil, nil
    emit("cleanup", { reason = reason, correlation = previous or false, created = created, destroyed = destroyed })
end

local function probe(player)
    if not enabled or not isElement(vehicle) or not isElement(player) then
        return
    end
    invalidate(player)
    probeSequence = probeSequence + 1
    local token = run .. ":probe:" .. tostring(probeSequence)
    local request = { token = token, correlation = correlation, issuedAt = getTickCount() }
    pending[player] = request
    request.timer = setTimer(function()
        if pending[player] == request then
            pending[player] = nil
            expired = expired + 1
            emit("probe_expired", { connection = connections[player], correlation = request.correlation, token = token })
        end
    end, lifetime, 1)
    -- root exists without client.lua; vehicle appears as an actual element argument.
    local sent = triggerClientEvent(player, "headlesswitness:probe", root, token, correlation, vehicle)
    emit("probe_sent", { connection = identify(player), correlation = correlation, token = token, dispatched = sent })
end

local function ensureFixture()
    local players = getElementsByType("player")
    if not enabled or #players < 2 then
        return
    end
    if not isElement(vehicle) then
        for player in pairs(pending) do
            invalidate(player)
        end
        acknowledged = {}
        vehicle = createVehicle(411, 1480, -1730, 13.6, 0, 0, 90, "H1")
        if not vehicle then
            emit("vehicle_create_failed")
            return
        end
        sequence = sequence + 1
        created = created + 1
        correlation = run .. ":vehicle:" .. tostring(sequence)
        setElementData(vehicle, "headlesswitness:correlation", correlation)
        emit("vehicle_created", { correlation = correlation, model = getElementModel(vehicle), players = #players, created = created })
    end
    for _, player in ipairs(players) do
        if not pending[player] and not acknowledged[player] then
            probe(player)
        end
    end
end

local function status()
    local participants = {}
    for _, player in ipairs(getElementsByType("player")) do
        participants[#participants + 1] = { connection = identify(player), name = getPlayerName(player), ack = acknowledged[player] == true }
    end
    emit("status", { enabled = enabled, players = participants, active = isElement(vehicle), correlation = correlation or false,
        joins = joins, quits = quits, connections_seen = connectionSequence, created = created, destroyed = destroyed,
        acks = acks, expired = expired, rejected = rejected })
end

addCommandHandler("witness", function(player, _, action)
    if not isConsoleInvocation(player) then
        emit("command_denied", { command = "witness", connection = identify(player) })
        return
    end
    if action == "start" then
        enabled = true
        ensureFixture()
        status()
    elseif action == "reset" then
        enabled = false
        cleanup("console_reset")
        status()
    elseif action == "status" or not action then
        status()
    else
        emit("usage", { command = "witness start|status|reset" })
    end
end, true, false)

addEvent("headlesswitness:ack", true)
addEventHandler("headlesswitness:ack", root, function(token, receivedCorrelation, receivedVehicle)
    -- Only the actual connection is authoritative; the remote source argument is not.
    if not client or source ~= client or getElementType(client) ~= "player" then
        rejected = rejected + 1
        emit("ack_rejected", { reason = "source" })
        return
    end
    local request = pending[client]
    if not enabled or not request or token ~= request.token or receivedCorrelation ~= request.correlation
        or receivedVehicle ~= vehicle or not isElement(vehicle) or getTickCount() - request.issuedAt >= lifetime then
        rejected = rejected + 1
        emit("ack_rejected", { reason = "token_or_element", connection = identify(client) })
        return
    end
    invalidate(client)
    acknowledged[client] = true
    acks = acks + 1
    emit("ack", { connection = identify(client), correlation = correlation, token = token, acks = acks })
end)

addEventHandler("onPlayerJoin", root, function()
    joins = joins + 1
    emit("player_join", { connection = identify(source), name = getPlayerName(source), joins = joins })
    ensureFixture()
end)

addEventHandler("onPlayerQuit", root, function()
    quits = quits + 1
    emit("player_quit", { connection = identify(source), name = getPlayerName(source), quits = quits })
    invalidate(source)
    connections[source] = nil
    -- Keep the vehicle for the other player; a reconnect must reference this same element.
end)

addCommandHandler("witnessb0", function(player)
    if not isConsoleInvocation(player) or enabled or isElement(vehicle) or #getElementsByType("player") ~= 0 then
        emit("b0_rejected", { reason = "console_only_disabled_zero_players" })
        return
    end
    local sample = createVehicle(411, 1480, -1730, 13.6)
    local checks = { created = isElement(sample) }
    if checks.created then
        local x, y, z = getElementPosition(sample)
        checks.model = getElementModel(sample) == 411
        checks.position = math.abs(x - 1480) < 0.01 and math.abs(y + 1730) < 0.01 and math.abs(z - 13.6) < 0.01
        checks.destroy_call = destroyElement(sample)
        checks.destroyed = not isElement(sample)
    end
    local passed = checks.created and checks.model and checks.position and checks.destroy_call and checks.destroyed
    emit("b0_result", { passed = passed == true, checks = checks, version = getVersion(), players = #getElementsByType("player"),
        command_source = isElement(player) and getElementType(player) or "legacy_console" })
end, true, false)

addEventHandler("onResourceStart", resourceRoot, function()
    emit("resource_start", { version = getVersion(), existing_players = #getElementsByType("player"), enabled = false })
end)

addEventHandler("onResourceStop", resourceRoot, function()
    enabled = false
    cleanup("resource_stop")
    emit("resource_stop", { joins = joins, quits = quits, acks = acks })
end)
