-- FrontierMP Client Chat Resource
-- Bridges network events with CEF In-Game UI

event.register("chat:add_message")
event.add_handler("chat:add_message", function(msg)
    cef.invoke("add_message", msg)
end)

event.register("chat:teleport")
event.add_handler("chat:teleport", function(x, y, z)
    local player_actor = natives.actor.get_player_actor(-1)
    if player_actor then
        natives.actor.teleport_actor(player_actor, vector3(tonumber(x), tonumber(y), tonumber(z)), false, false, false)
        natives.hud.print_small_b("Teleported!", 2.0, true)
    end
end)

event.register("player:kill")
event.add_handler("player:kill", function()
    local player_actor = natives.actor.get_player_actor(-1)
    if player_actor then
        natives.health.kill_actor(player_actor)
    end
end)
