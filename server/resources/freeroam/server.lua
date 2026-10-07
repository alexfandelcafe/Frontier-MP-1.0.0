-- FrontierMP Freeroam Server Script

event.add_handler("core:on_player_joined", function(client_id, name)
    print("[Freeroam] Welcome " .. name .. " (" .. client_id .. ")")
    event.trigger_on_all_clients("chat:add_message", "^2* " .. name .. " joined the frontier.")
end)

event.add_handler("core:on_player_left", function(client_id, name)
    print("[Freeroam] Player left: " .. name .. " (" .. client_id .. ")")
    event.trigger_on_all_clients("chat:add_message", "^1* " .. name .. " left the frontier.")
end)
