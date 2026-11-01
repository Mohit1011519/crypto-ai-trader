#pragma once

#include <string>

namespace crypto_trader::common {

// Logs to the console and, when log_file is non-empty, to a rotating file
// (5 MB x 3) so the bot's history can be reviewed later.
void initialize_logging(const std::string& log_level, const std::string& log_file = "");

}  // namespace crypto_trader::common
