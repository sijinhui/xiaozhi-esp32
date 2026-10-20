#ifndef HTTP_GET_H
#define HTTP_GET_H

#include <memory>
#include <string>

#include "board.h"

// GET that follows redirects. Http does not, and GitHub release downloads answer 302 to a
// file host. Returns the open connection once it answers 200, or nullptr with `error` set.
inline std::unique_ptr<Http> OpenHttpGet(std::string url, std::string& error,
                                         const std::string& user_agent = "") {
    auto network = Board::GetInstance().GetNetwork();
    for (int hop = 0; hop < 5; ++hop) {
        auto http = network->CreateHttp(0);
        if (!user_agent.empty()) {
            http->SetHeader("User-Agent", user_agent);
        }
        if (auto opened = http->Open("GET", url); !opened) {
            error = opened.error().ToString();
            return nullptr;
        }
        auto status_code = http->GetStatusCode();
        if (!status_code) {
            error = status_code.error().ToString();
            return nullptr;
        }
        int code = *status_code;
        if (code == 200) {
            return http;
        }
        std::string location = http->GetResponseHeader("Location");
        http->Close();
        if ((code != 301 && code != 302 && code != 303 && code != 307 && code != 308) ||
            location.empty()) {
            error = "HTTP " + std::to_string(code);
            return nullptr;
        }
        if (location[0] == '/') {
            auto host_start = url.find("://");
            auto path_start = host_start == std::string::npos ? std::string::npos
                                                              : url.find('/', host_start + 3);
            location = url.substr(0, path_start) + location;
        }
        url = location;
    }
    error = "too many redirects";
    return nullptr;
}

#endif  // HTTP_GET_H
