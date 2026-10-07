-- FrontierMP Server Chat Resource
-- Handles chat commands and text routing

local locations = {
    ["valentine"]      = vector3(-180.0, 60.0, 1950.0),
    ["armadillo"]      = vector3(-2175.62, 16.31, 2613.50),
    ["blackwater"]     = vector3(-83.45, 117.68, 1374.10),
    ["thieveslanding"] = vector3(101.91, 73.10, 2322.79),
    ["chuparosa"]      = vector3(-1545.03, 15.03, 3913.46)
}

event.add_handler("chat:command", function(source, args)
    local full_cmd = args[1]
    if not full_cmd then return end

    local cmd = string.match(full_cmd, "^(%S+)")
    local param = string.match(full_cmd, "^%S+%s+(.+)$")

    if cmd == "tp" then
        if param and locations[string.lower(param)] then
            local pos = locations[string.lower(param)]
            event.trigger_on_client("chat:teleport", source, pos.x, pos.y, pos.z)
            event.trigger_on_client("chat:add_message", source, "^2Teleported to " .. param .. "!")
        else
            event.trigger_on_client("chat:add_message", source, "^1Usage: /tp [valentine | armadillo | blackwater | thieveslanding | chuparosa]")
        end
    elseif cmd == "help" then
        event.trigger_on_client("chat:add_message", source, "^3--- FrontierMP Commands ---")
        event.trigger_on_client("chat:add_message", source, "^3/tp [location] - Teleport to town")
        event.trigger_on_client("chat:add_message", source, "^3/me [action] - Roleplay action")
        event.trigger_on_client("chat:add_message", source, "^3/suicide - Respawn character")
    elseif cmd == "suicide" then
        event.trigger_on_client("player:kill", source)
    elseif cmd == "me" then
        if param then
            event.trigger_on_all_clients("chat:add_message", "^5* " .. player.get_name(source) .. " " .. param)
        end
    end
end)
