-- FrontierMP Admin Resource
-- Provides admin commands: /kick, /ann, /weather

event.add_handler("chat:command", function(source, args)
    local full_cmd = args[1]
    if not full_cmd then return end

    local cmd = string.match(full_cmd, "^(%S+)")
    local param = string.match(full_cmd, "^%S+%s+(.+)$")

    if cmd == "ann" then
        if param then
            event.trigger_on_all_clients("chat:add_message", "^1[ANUNCIO] " .. param)
        end
    elseif cmd == "players" then
        local count = 0
        for _ in pairs(player.list()) do count = count + 1 end
        event.trigger_on_client("chat:add_message", source, "^3Jugadores en linea: " .. count)
    end
end)
