-- FrontierMP 3D Nametags Client Script
-- Uses RDR1 multiplayer text billboards above remote players

local player_tags = {}

local function update_nametags()
    local cam = natives.cam.get_game_camera()
    local cam_dir = natives.camera.get_camera_direction(cam)

    for id, tag in pairs(player_tags) do
        local remote = player.get(id)
        if remote and remote.actor and natives.actor.is_actor_valid(remote.actor) then
            local pos = natives.actor.get_position(remote.actor)
            pos.y = pos.y + 2.0 -- Colocar 2 metros sobre la cabeza
            natives.object.set_object_position(tag, pos)
        end
    end
end

event.register("core:on_player_joined")
event.add_handler("core:on_player_joined", function(id, name)
    local remote = player.get(id)
    if remote and remote.actor then
        natives.extended.ui_add_string("NAMETAG_" .. id, name)
        local tag = natives.gravestone.create_mp_text(remote.actor, "", "NAMETAG_" .. id, vector3(0,0,0), vector3(0,0,0), 0xFFFFFF)
        player_tags[id] = tag
    end
end)

event.register("core:on_player_left")
event.add_handler("core:on_player_left", function(id)
    if player_tags[id] then
        natives.object.destroy_object(player_tags[id])
        player_tags[id] = nil
        natives.extended.ui_remove_string("NAMETAG_" .. id)
    end
end)

thread.create_loop(function()
    update_nametags()
    thread.wait(16) -- ~60 FPS
end)
