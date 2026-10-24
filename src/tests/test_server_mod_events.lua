-- Test script for the ServerModManager event bus (net.on / Emit).
-- Registers handlers for "player.spawn" and "player.kick". The counters are
-- globals so the C++ test can read them back via ServerModManager::GetGlobalInt.

spawns = 0
kicks = 0

net.on("player.spawn", function(id)
    spawns = spawns + 1
end)

net.on("player.kick", function(id, reason)
    kicks = kicks + 1
end)