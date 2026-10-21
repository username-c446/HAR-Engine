#pragma once

// Registers the "host", "join" and "match" console commands used to drive an
// online match (start a server, connect to a host, leave the current match).
// Safe to call multiple times; registration happens only once.
void RegisterNetworkConsoleCommands();