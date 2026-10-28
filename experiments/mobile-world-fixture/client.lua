-- Diagnostics are intentionally native MTA calls: a custom client must implement them.
local function log(message)
    outputDebugString("[mobilefixture] " .. message, 3)
end

local function describe(element, reason)
    if not isElement(element) or getElementType(element) ~= "vehicle" then
        return
    end
    local correlation = getElementData(element, "mobilefixture:correlation")
    if type(correlation) ~= "string" then
        return
    end
    local x, y, z = getElementPosition(element)
    log(string.format("%s correlation=%s model=%d position=%.3f,%.3f,%.3f streamed=%s",
        reason, correlation, getElementModel(element), x, y, z, tostring(isElementStreamedIn(element))))
end

addEvent("mobilefixture:probe", true)
addEventHandler("mobilefixture:probe", resourceRoot, function(token, correlation, element)
    if source ~= resourceRoot or type(token) ~= "string" or type(correlation) ~= "string" then
        return
    end
    if not isElement(element) or getElementType(element) ~= "vehicle" then
        log("probe_missing_element correlation=" .. correlation)
        return
    end
    if getElementData(element, "mobilefixture:correlation") ~= correlation then
        log("probe_correlation_mismatch correlation=" .. correlation)
        return
    end
    describe(element, "probe_received")
    triggerServerEvent("mobilefixture:probeReply", localPlayer, token, correlation, element)
end)

addEventHandler("onClientElementStreamIn", root, function()
    describe(source, "stream_in")
end)

addEventHandler("onClientElementStreamOut", root, function()
    describe(source, "stream_out")
end)

addEventHandler("onClientElementDataChange", root, function(key)
    if key == "mobilefixture:correlation" then
        describe(source, "correlation_received")
    end
end)

addEventHandler("onClientResourceStart", resourceRoot, function()
    -- cache=false Lua travels in packets. A separate resource file exercises
    -- file download and file APIs; the HTTP log/hash must prove its origin.
    local file = fileOpen("http-witness.txt", true)
    if not file then
        log("http_asset_failed reason=open")
        return
    end
    local size = fileGetSize(file)
    local contents = size <= 128 and fileRead(file, size) or false
    fileClose(file)
    if type(contents) ~= "string" or not contents:match("^MTA mobile HTTP witness v1[\r\n]*$") then
        log("http_asset_failed reason=contents")
        return
    end
    log("http_asset_read bytes=" .. tostring(size))
    log("resource_start player=" .. getPlayerName(localPlayer))
    triggerServerEvent("mobilefixture:ready", localPlayer)
end)

addEventHandler("onClientResourceStop", resourceRoot, function()
    log("resource_stop")
end)
