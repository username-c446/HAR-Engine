#pragma once

// Standalone build: includes the network layer directly. The game normally wires
// NetworkMatchManager + RemotePlayerWorldView into gameplay context; here we use
// the underlying classes directly since there is no game. Kept as a single header
// so the standalone .cpp stays tiny.

#include "NetworkServer.h"
#include "NetworkClient.h"
#include "PlayerNetworkState.h"
#include "NetworkPacket.h"

#include <cstdlib>