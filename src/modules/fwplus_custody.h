#pragma once

#include <stdint.h>
#include <string.h>

//fw+ DTN-like custody signaling helpers shared across files
namespace fwplus_custody
{
    // Only CA is used in CA-only design
    static const uint32_t RR_ROUTER_CUSTODY_ACK = 10; // router → source
}


