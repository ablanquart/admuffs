// SPDX-License-Identifier: MIT
#include "net/http.h"
#include "common.h"

#include <curl/curl.h>

namespace admuffs {

namespace {
size_t write_cb(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* s = static_cast<std::string*>(userdata);
    s->append(ptr, size * nmemb);
    return size * nmemb;
}
}  // namespace

void Http::global_init() { curl_global_init(CURL_GLOBAL_DEFAULT); }
void Http::global_cleanup() { curl_global_cleanup(); }

HttpResponse Http::request(const HttpRequest& req) {
    HttpResponse resp;
    CURL* curl = curl_easy_init();
    if (!curl) { resp.error = "curl init failed"; return resp; }

    curl_easy_setopt(curl, CURLOPT_URL, req.url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &resp.body);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, (long)req.timeout_ms);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, (long)req.timeout_ms);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);

    if (req.insecure) {
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
    }

    if (req.method == "POST") {
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, req.body.c_str());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)req.body.size());
    } else if (req.method != "GET") {
        curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, req.method.c_str());
        if (!req.body.empty()) {
            curl_easy_setopt(curl, CURLOPT_POSTFIELDS, req.body.c_str());
            curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)req.body.size());
        }
    }

    struct curl_slist* hdrs = nullptr;
    for (const auto& h : req.headers) hdrs = curl_slist_append(hdrs, h.c_str());
    if (hdrs) curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdrs);

    CURLcode rc = curl_easy_perform(curl);
    if (rc == CURLE_OK) {
        resp.ok = true;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &resp.status);
    } else {
        resp.error = curl_easy_strerror(rc);
        LOG_DEBUG("http %s %s failed: %s", req.method.c_str(), req.url.c_str(),
                  resp.error.c_str());
    }

    if (hdrs) curl_slist_free_all(hdrs);
    curl_easy_cleanup(curl);
    return resp;
}

HttpResponse Http::get(const std::string& url, int timeout_ms) {
    HttpRequest r; r.method = "GET"; r.url = url; r.timeout_ms = timeout_ms;
    return request(r);
}

HttpResponse Http::post(const std::string& url, const std::string& body,
                        const std::vector<std::string>& headers, int timeout_ms) {
    HttpRequest r; r.method = "POST"; r.url = url; r.body = body;
    r.headers = headers; r.timeout_ms = timeout_ms;
    return request(r);
}

HttpResponse Http::post_multipart(
    const std::string& url,
    const std::vector<std::pair<std::string, std::string>>& fields,
    const std::string& file_field, const std::string& file_name,
    const std::string& file_data, int timeout_ms) {
    HttpResponse resp;
    CURL* curl = curl_easy_init();
    if (!curl) { resp.error = "curl init failed"; return resp; }

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &resp.body);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, (long)timeout_ms);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, (long)timeout_ms);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);

    curl_mime* mime = curl_mime_init(curl);
    for (const auto& [k, v] : fields) {
        curl_mimepart* part = curl_mime_addpart(mime);
        curl_mime_name(part, k.c_str());
        curl_mime_data(part, v.c_str(), v.size());
    }
    curl_mimepart* fp = curl_mime_addpart(mime);
    curl_mime_name(fp, file_field.c_str());
    curl_mime_filename(fp, file_name.c_str());
    curl_mime_data(fp, file_data.data(), file_data.size());
    curl_easy_setopt(curl, CURLOPT_MIMEPOST, mime);

    CURLcode rc = curl_easy_perform(curl);
    if (rc == CURLE_OK) {
        resp.ok = true;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &resp.status);
    } else {
        resp.error = curl_easy_strerror(rc);
        LOG_DEBUG("http multipart POST %s failed: %s", url.c_str(), resp.error.c_str());
    }

    curl_mime_free(mime);
    curl_easy_cleanup(curl);
    return resp;
}

}  // namespace admuffs
