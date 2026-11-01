#pragma once

#include <string>

namespace crypto_trader::common {

// Optional Telegram alerts. Enabled only when TELEGRAM_BOT_TOKEN and
// TELEGRAM_CHAT_ID are set. Delivery failures are logged and never interrupt
// trading.
class Notifier {
public:
    static Notifier from_environment();

    bool enabled() const noexcept { return !token_.empty(); }
    void send(const std::string& message) const;

private:
    std::string token_;
    std::string chat_id_;
};

}  // namespace crypto_trader::common
