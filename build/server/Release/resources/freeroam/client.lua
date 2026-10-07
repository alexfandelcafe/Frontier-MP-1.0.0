-- FrontierMP Freeroam Client Script
-- Spawning, health regeneration, and death handlers

local prev_health = 100
local regen_timer = 0.0

local function check_health_regen()
    local actor = natives.actor.get_player_actor(-1)
    if not actor then return end

    local current_health = natives.health.get_actor_health(actor)

    if current_health <= 0 then
        -- Muerte del jugador: Pantalla de carga y reaparición
        thread.wait(1500)
        natives.hud.hud_fade_to_loading_screen()
        thread.wait(1000)

        -- Respawn
        local layout = natives.object.find_named_layout("PlayerLayout")
        local model = 837 -- John Marston
        local spawn_pos = vector3(-180.0, 60.0, 1950.0)

        natives.streaming.streaming_request_actor(model)
        while not natives.streaming.streaming_is_actor_loaded(model) do
            thread.wait(50)
        end

        natives.actor.create_player_actor_in_layout(layout, model, spawn_pos.x, spawn_pos.y, spawn_pos.z, 0.0)
        natives.hud.hud_fade_from_loading_screen()
    elseif current_health < 100 and current_health > 0 then
        -- Regeneración pasiva
        natives.health.set_actor_health(actor, current_health + 1)
    end

    prev_health = current_health
end

-- Registrar bucle de tick
thread.create_loop(function()
    check_health_regen()
    thread.wait(1000)
end)
