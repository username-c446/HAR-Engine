#include "ENetInit.h"

namespace net
{
    // Process-wide ENet lifetime reference count. Declared static in the header,
    // defined here so linking is self-contained regardless of which translation
    // units embed ENetInit. The count starts at zero; each ENetInit ctor/dtor
    // increments/decrements it, and ENet is (de)initialized at 0<->1 transitions.
    int ENetInit::s_refCount = 0;
}