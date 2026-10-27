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
    log("resource_start player=" .. getPlayerName(localPlayer))
    triggerServerEvent("mobilefixture:ready", localPlayer)
end)

addEventHandler("onClientResourceStop", resourceRoot, function()
    log("resource_stop")
end)
