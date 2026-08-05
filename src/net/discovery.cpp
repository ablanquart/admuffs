// SPDX-License-Identifier: MIT
#include "net/discovery.h"
#include "common.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <cstring>
#include <set>

namespace admuffs {

namespace {
std::string header_value(const std::string& resp, const std::string& key) {
    // Case-insensitive header search.
    std::string low = to_lower(resp);
    std::string k = to_lower(key) + ":";
    size_t pos = low.find(k);
    if (pos == std::string::npos) return "";
    pos += k.size();
    size_t end = resp.find("\r\n", pos);
    if (end == std::string::npos) end = resp.size();
    return trim(resp.substr(pos, end - pos));
}
}  // namespace

std::vector<DiscoveredDevice> ssdp_search(const std::string& search_target, int timeout_ms) {
    std::vector<DiscoveredDevice> out;
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) { LOG_WARN("ssdp: socket() failed"); return out; }

    struct timeval tv{timeout_ms / 1000, (timeout_ms % 1000) * 1000};
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    int ttl = 2;
    setsockopt(sock, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl));

    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(1900);
    inet_pton(AF_INET, "239.255.255.250", &addr.sin_addr);

    std::string msg =
        "M-SEARCH * HTTP/1.1\r\n"
        "HOST: 239.255.255.250:1900\r\n"
        "MAN: \"ssdp:discover\"\r\n"
        "MX: 2\r\n"
        "ST: " + search_target + "\r\n\r\n";

    // Send the query a couple of times; UDP multicast can be lossy.
    for (int i = 0; i < 2; ++i)
        sendto(sock, msg.data(), msg.size(), 0, (sockaddr*)&addr, sizeof(addr));

    std::set<std::string> seen_ips;
    uint64_t deadline = now_ms() + timeout_ms;
    char buf[4096];
    while (now_ms() < deadline) {
        struct sockaddr_in from{};
        socklen_t flen = sizeof(from);
        ssize_t n = recvfrom(sock, buf, sizeof(buf) - 1, 0, (sockaddr*)&from, &flen);
        if (n <= 0) continue;
        buf[n] = 0;
        std::string resp(buf, n);

        char ips[INET_ADDRSTRLEN] = {0};
        inet_ntop(AF_INET, &from.sin_addr, ips, sizeof(ips));
        std::string ip = ips;
        if (seen_ips.count(ip)) continue;
        seen_ips.insert(ip);

        DiscoveredDevice d;
        d.ip = ip;
        d.location = header_value(resp, "LOCATION");
        d.server = header_value(resp, "SERVER");
        d.usn = header_value(resp, "USN");
        d.st = header_value(resp, "ST");
        out.push_back(d);
    }

    ::close(sock);
    LOG_DEBUG("ssdp: %zu device(s) for ST=%s", out.size(), search_target.c_str());
    return out;
}

}  // namespace admuffs
