// SPDX-License-Identifier: MIT
// http.h - thin libcurl wrapper for the REST-based TV APIs.
#pragma once

#include <string>
#include <vector>

namespace admuffs {

struct HttpResponse {
    bool ok = false;        // transport succeeded (not necessarily 2xx)
    long status = 0;        // HTTP status code
    std::string body;
    std::string error;      // transport error text, if any

    bool is2xx() const { return status >= 200 && status < 300; }
};

struct HttpRequest {
    std::string method = "GET";
    std::string url;
    std::string body;
    std::vector<std::string> headers;  // "Key: Value"
    int timeout_ms = 4000;
    bool insecure = false;             // accept self-signed certs (Vizio)
};

class Http {
public:
    static void global_init();
    static void global_cleanup();
    static HttpResponse request(const HttpRequest& req);

    static HttpResponse get(const std::string& url, int timeout_ms = 4000);
    static HttpResponse post(const std::string& url, const std::string& body,
                             const std::vector<std::string>& headers = {},
                             int timeout_ms = 4000);

    // multipart/form-data POST: text fields plus one binary file part.
    static HttpResponse post_multipart(
        const std::string& url,
        const std::vector<std::pair<std::string, std::string>>& fields,
        const std::string& file_field, const std::string& file_name,
        const std::string& file_data, int timeout_ms = 10000);
};

}  // namespace admuffs
