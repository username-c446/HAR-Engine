-- Test server mod for ServerModManager unit tests.
-- on_join rejects clientId 9999, on_validate rejects anything with z far from 0.

function on_join(clientId)
    if clientId == 9999 then
        return false
    end
    return true
end

function on_validate(clientId, x, y, z, speed)
    if z > 100.0 then
        return false
    end
    return true
end

function on_leave(clientId)
end