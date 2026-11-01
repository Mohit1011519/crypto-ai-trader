cmake_minimum_required(VERSION 3.24)

if(POLICY CMP0167)
    cmake_policy(SET CMP0167 NEW)
endif()

project(crypto_ai_trader VERSION 0.1.0 LANGUAGES CXX)

include(FetchContent)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(CURL REQUIRED)
find_package(Boost 1.74 REQUIRED COMPONENTS system)
find_package(OpenSSL REQUIRED)

FetchContent_Declare(
    nlohmann_json
    URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
)
FetchContent_Declare(
    spdlog
    URL https://github.com/gabime/spdlog/archive/refs/tags/v1.14.1.tar.gz
)

set(SPDLOG_FMT_EXTERNAL OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(nlohmann_json spdlog)

add_library(crypto_trader
    src/ai/signal_engine.cpp
    src/common/config.cpp
    src/common/logger.cpp
    src/exchange/binance_exchange.cpp
    src/exchange/binance_market_stream.cpp
    src/indicators/technical_indicators.cpp
    src/market/market_scanner.cpp
    src/market/order_book_analysis.cpp
    src/market/trend_analysis.cpp
    src/market/volatility_analysis.cpp
    src/market/volume_analysis.cpp
    src/paper/paper_portfolio.cpp
    src/risk/risk_manager.cpp
)

target_include_directories(crypto_trader PUBLIC include)
if(WIN32)
    target_compile_definitions(crypto_trader PUBLIC _WIN32_WINNT=0x0A00)
endif()
target_link_libraries(crypto_trader
    PUBLIC
        CURL::libcurl
        Boost::system
        OpenSSL::SSL
        OpenSSL::Crypto
        nlohmann_json::nlohmann_json
        spdlog::spdlog
)

if(MSVC)
    target_compile_options(crypto_trader PRIVATE /W4 /permissive- /bigobj)
else()
    target_compile_options(crypto_trader PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_executable(crypto-ai-trader src/main.cpp)
target_link_libraries(crypto-ai-trader PRIVATE crypto_trader)

include(CTest)
if(BUILD_TESTING)
    FetchContent_Declare(
        googletest
        URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.tar.gz
    )
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)

    add_executable(crypto_trader_tests
        tests/config_tests.cpp
        tests/market_research_tests.cpp
        tests/paper_portfolio_tests.cpp
        tests/risk_manager_tests.cpp
        tests/signal_engine_tests.cpp
    )
    target_link_libraries(crypto_trader_tests PRIVATE crypto_trader GTest::gtest_main)
    include(GoogleTest)
    gtest_discover_tests(crypto_trader_tests)
endif()
