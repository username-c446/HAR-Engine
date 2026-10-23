#pragma once

// Standalone build: includes the network engine layer directly. The game normally
// wires net::Session + GamePlayerBridge through NetworkMatchManager into the
// gameplay context; here we use the engine classes directly since there is no
// game. Kept as a single header so the standalone .cpp stays tiny.

#include "../game/netengine/Session.h"
#include "../game/netengine/GamePlayerBridge.h"
#include "../game/netengine/ServerEndpoint.h"
#include "../game/netengine/ClientEndpoint.h"
#include "../game/netengine/WorldInterfaces.h"
#include "../game/netengine/Packet.h"
#include "PlayerNetworkState.h"
#include "NetworkPacket.h"

#include <cstdlib>