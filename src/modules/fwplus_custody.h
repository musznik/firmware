#pragma once

#include <stdint.h>
#include <string.h>

//fw+ DTN-like custody signaling helpers shared across files
namespace fwplus_custody
{
    // Only CA is used in CA-only design
    static const uint32_t RR_ROUTER_CUSTODY_ACK = 10; // router → source
    static const uint32_t RR_ROUTER_DELIVERED   = 11; // router → source (final delivery)
    static const uint32_t RR_ROUTER_DELIVERY_FAILED = 12; // router/source → mesh (terminal failure)
    //fw+ Explicit custody claim broadcast (server → mesh) so other S&F back off
    static const uint32_t RR_ROUTER_CUSTODY_CLAIM = 13; // server → mesh (claim custody)
}


