-- Live server-mod for mp_standalone --mod runtime check.
-- Registers a handler on the engine's open event bus; mp_standalone emits
-- "player.joined" through the live session loop when a client connects, and
-- we count invocations in a global the harness can observe via console.

joined_count = 0

net.on("player.joined", function(id)
    joined_count = joined_count + 1
    print("live-mod: player.joined handler fired, id=" .. id .. " total=" .. joined_count)
end)