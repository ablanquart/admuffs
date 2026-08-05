// SPDX-License-Identifier: MIT
// discovery.h - SSDP (UPnP) discovery for finding TVs on the LAN.
#pragma once

#include <string>
#include <vector>

namespace admuffs {

struct DiscoveredDevice {
    std::string ip;
    std::string location;   // LOCATION header (device description URL)
    std::string server;     // SERVER header
    std::string usn;        // USN header
    std::string st;         // search target that matched
};

// Send an SSDP M-SEARCH for `search_target` (e.g. "roku:ecp",
// "urn:dial-multiscreen-org:service:dial:1", "ssdp:all") and collect replies
// for up to timeout_ms. Deduplicated by IP.
std::vector<DiscoveredDevice> ssdp_search(const std::string& search_target,
                                          int timeout_ms = 2500);

}  // namespace admuffs
