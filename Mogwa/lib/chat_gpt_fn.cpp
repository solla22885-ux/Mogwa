// chat_gpt_fn.cpp : 구현 파일.
//

#include "pch.h"
#include "chat_gpt_fn.h"
#include "string.h"

#include <curl/curl.h>
#include <boost/json.hpp>

///////////////////////////////////////////////////////////////////////////////

namespace m1 {
    namespace gpt_fn {

    size_t WriteCallback(void* contents, size_t size, size_t nmemb, void* userp)
    {
        size_t totalSize = size * nmemb;
        std::string* s = static_cast<std::string*>(userp);
        s->append(static_cast<char*>(contents), totalSize);
        return totalSize;
    }

///////////////////////////////////////////////////////////////////////////////

    bool gpt_bot::execute(std::string& output)
    {
        if (api_key.empty() || model.empty() || messages.empty()) {
            return false;
        }

        boost::json::object root;
        root["model"] = model;

        boost::json::array query;
        for (auto message : messages) {
            boost::json::object obj;
            obj["role"] = "user";
            obj["content"] = message;

            query.push_back(obj);
        }

        root["messages"] = query;

        std::string request = boost::json::serialize(root);

        CURL* curl = curl_easy_init();
        if (curl) {
            struct curl_slist* headers = nullptr;
            std::string authHeader = "Authorization: Bearer " + api_key;
            std::string response;

            headers = curl_slist_append(headers, "Content-Type: application/json");
            headers = curl_slist_append(headers, authHeader.data());

            curl_easy_setopt(curl, CURLOPT_URL, CHAT_GPT_PATH.data());
            curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
            curl_easy_setopt(curl, CURLOPT_POSTFIELDS, request.data());
            curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
            curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
            curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 5000L);
            curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 20000L);
            curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);

            CURLcode res = curl_easy_perform(curl);
            if (res == CURLE_OK) {
                try {
                    boost::json::value parsed = boost::json::parse(response);
                    boost::json::object obj = parsed.as_object();

                    if (obj.if_contains("error")) {
                        boost::json::object err = obj["error"].as_object();
                        output = err["message"].as_string();

                        messages.clear();
                        curl_slist_free_all(headers);
                        curl_easy_cleanup(curl);
                        return false;
                    }
                    else {
                        output = parsed.as_object()["choices"]
                            .as_array()[0]
                            .as_object()["message"]
                            .as_object()["content"]
                            .as_string();
                    }
                }
                catch (const std::exception&) {
                    messages.clear();
                    curl_slist_free_all(headers);
                    curl_easy_cleanup(curl);
                    return false;
                }
            }
            else {
                messages.clear();
                curl_slist_free_all(headers);
                curl_easy_cleanup(curl);
                return false;
            }

            messages.clear();
            curl_slist_free_all(headers);
            curl_easy_cleanup(curl);
        }
        else {
            return false;
        }

        return true;
    }
    }
}

