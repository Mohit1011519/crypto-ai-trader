#include "common/notifier.hpp"

#include <curl/curl.h>

#include <cstdlib>
#include <memory>

#include <spdlog/spdlog.h>

namespace crypto_trader::common {
namespace {

size_t discard_response(char*, const size_t size, const size_t count, void*) {
    return size * count;
}

}  // namespace

Notifier Notifier::from_environment() {
    Notifier notifier;
    const char* token = std::getenv("TELEGRAM_BOT_TOKEN");
    const char* chat_id = std::getenv("TELEGRAM_CHAT_ID");
    if (token != nullptr && chat_id != nullptr && *token != '\0' && *chat_id != '\0') {
        notifier.token_ = token;
        notifier.chat_id_ = chat_id;
    }
    return notifier;
}

void Notifier::send(const std::string& message) const {
    if (!enabled()) {
        return;
    }
    std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> handle(
        curl_easy_init(), &curl_easy_cleanup);
    if (!handle) {
        spdlog::warn("Alert not sent: curl_easy_init failed");
        return;
    }
    std::unique_ptr<char, decltype(&curl_free)> text(
        curl_easy_escape(handle.get(), message.c_str(), static_cast<int>(message.size())),
        &curl_free);
    const std::string url = "https://api.telegram.org/bot" + token_ + "/sendMessage";
    const std::string body = "chat_id=" + chat_id_ + "&text=" + (text ? text.get() : "");
    curl_easy_setopt(handle.get(), CURLOPT_URL, url.c_str());
    curl_easy_setopt(handle.get(), CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(handle.get(), CURLOPT_WRITEFUNCTION, discard_response);
    curl_easy_setopt(handle.get(), CURLOPT_TIMEOUT, 10L);
    curl_easy_setopt(handle.get(), CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(handle.get(), CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(handle.get(), CURLOPT_SSL_VERIFYHOST, 2L);
    const auto result = curl_easy_perform(handle.get());
    long status = 0;
    curl_easy_getinfo(handle.get(), CURLINFO_RESPONSE_CODE, &status);
    if (result != CURLE_OK || status != 200) {
        // The URL contains the bot token, so it is never logged.
        spdlog::warn("Telegram alert not delivered (curl={}, HTTP {})",
                     curl_easy_strerror(result), status);
    }
}

}  // namespace crypto_trader::common
