cmake_minimum_required(VERSION 3.15)
project(OrderBook VERSION 1.0.0 LANGUAGES CXX)

# C++17 with optimizations
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Build type and optimization flags
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# Find dependencies
find_package(Boost 1.70 REQUIRED)
find_package(Threads REQUIRED)
option(WITH_QUICKFIX "Enable QuickFIX integration" ON)
if(WITH_QUICKFIX)
    set(QUICKFIX_ROOT "${CMAKE_SOURCE_DIR}/third_party/quickfix_install")
    find_path(QUICKFIX_INCLUDE_DIR quickfix/Application.h HINTS "${QUICKFIX_ROOT}/include")
    find_library(QUICKFIX_LIBRARY NAMES quickfix libquickfix HINTS "${QUICKFIX_ROOT}/lib")
    if(QUICKFIX_INCLUDE_DIR AND QUICKFIX_LIBRARY)
        message(STATUS "Found QuickFIX: include=${QUICKFIX_INCLUDE_DIR} lib=${QUICKFIX_LIBRARY}")
        include_directories(${QUICKFIX_INCLUDE_DIR})
        add_definitions(-DWITH_QUICKFIX)
    else()
        message(FATAL_ERROR "QuickFIX requested but library not found. Install QuickFIX or disable WITH_QUICKFIX.")
    endif()
endif()

# Core library sources
set(CORE_SOURCES
    src/Core/OrderBook.cpp
    src/Core/OrderManager.cpp
    src/Core/MatchingEngine.cpp
    src/Core/Order.cpp
)

# Network library sources
set(NETWORK_SOURCES
    src/Network/FixParser.cpp
    src/Network/FixSession.cpp
    src/Network/FixMessageHandler.cpp
    src/Network/FixServer.cpp
)

# Utilities library sources
set(UTILITY_SOURCES
    src/Utilities/Logger.cpp
    src/Utilities/Config.cpp
    src/Utilities/PerformanceTimer.cpp
    src/Utilities/PerformanceTest.cpp
)

# Market Data library sources
set(MARKETDATA_SOURCES
    src/MarketData/MarketDataFeed.cpp
)
if(WITH_QUICKFIX)
    list(APPEND MARKETDATA_SOURCES src/MarketData/Adapters/QuickFixConnector.cpp)
endif()

# Risk library sources
set(RISK_SOURCES
    src/Risk/RiskManager.cpp
)

# Create library targets
add_library(OrderBookCore STATIC ${CORE_SOURCES})
target_include_directories(OrderBookCore PUBLIC include)
target_link_libraries(OrderBookCore PUBLIC Threads::Threads Boost::headers)

add_library(OrderBookNetwork STATIC ${NETWORK_SOURCES})
target_include_directories(OrderBookNetwork PUBLIC include)
target_link_libraries(OrderBookNetwork PUBLIC 
    OrderBookCore 
    OrderBookUtilities
    Boost::headers
    Threads::Threads
)

add_library(OrderBookUtilities STATIC ${UTILITY_SOURCES})
target_include_directories(OrderBookUtilities PUBLIC include)
target_link_libraries(OrderBookUtilities PUBLIC Threads::Threads Boost::headers)

add_library(OrderBookMarketData STATIC ${MARKETDATA_SOURCES})
target_include_directories(OrderBookMarketData PUBLIC include)
target_link_libraries(OrderBookMarketData PUBLIC 
    OrderBookCore 
    OrderBookUtilities
    Threads::Threads
)
if(WITH_QUICKFIX)
    target_link_libraries(OrderBookMarketData PUBLIC ${QUICKFIX_LIBRARY})
endif()

add_library(OrderBookRisk STATIC ${RISK_SOURCES})
target_include_directories(OrderBookRisk PUBLIC include)
target_link_libraries(OrderBookRisk PUBLIC 
    OrderBookCore 
    OrderBookUtilities
    Threads::Threads
)

# Main executable
add_executable(OrderBook src/main.cpp)
target_link_libraries(OrderBook PRIVATE 
    OrderBookNetwork 
    OrderBookMarketData
    OrderBookRisk
    Boost::headers 
    Threads::Threads
)

# Performance validation executable
add_executable(OrderBookPerformanceValidation src/performance_validation.cpp)
target_link_libraries(OrderBookPerformanceValidation PRIVATE 
    OrderBookNetwork 
    OrderBookMarketData
    OrderBookRisk
    Boost::headers 
    Threads::Threads
)

if(WITH_QUICKFIX)
    add_executable(MDSimulator src/tests/MDSimulator.cpp)
    target_link_libraries(MDSimulator PRIVATE ${QUICKFIX_LIBRARY})
endif()

# Compiler-specific optimization flags
if(CMAKE_BUILD_TYPE STREQUAL "Release")
    if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" OR CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
        target_compile_options(OrderBookCore PRIVATE -O3 -march=native -DNDEBUG -fsanitize=address)
        target_compile_options(OrderBookNetwork PRIVATE -O3 -march=native -DNDEBUG -fsanitize=address)
        target_compile_options(OrderBookUtilities PRIVATE -O3 -march=native -DNDEBUG -fsanitize=address)
        target_compile_options(OrderBookMarketData PRIVATE -O3 -march=native -DNDEBUG -fsanitize=address)
        target_compile_options(OrderBookRisk PRIVATE -O3 -march=native -DNDEBUG -fsanitize=address)
        target_compile_options(OrderBook PRIVATE -O3 -march=native -DNDEBUG -fsanitize=address)
        target_compile_options(OrderBookPerformanceValidation PRIVATE -O3 -march=native -DNDEBUG -fsanitize=address)
        target_link_options(OrderBookPerformanceValidation PRIVATE -fsanitize=address)
        target_link_options(OrderBook PRIVATE -fsanitize=address)
    elseif(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
        target_compile_options(OrderBookCore PRIVATE /O2 /DNDEBUG)
        target_compile_options(OrderBookNetwork PRIVATE /O2 /DNDEBUG)
        target_compile_options(OrderBookUtilities PRIVATE /O2 /DNDEBUG)
        target_compile_options(OrderBookMarketData PRIVATE /O2 /DNDEBUG)
        target_compile_options(OrderBookRisk PRIVATE /O2 /DNDEBUG)
        target_compile_options(OrderBook PRIVATE /O2 /DNDEBUG)
        target_compile_options(OrderBookPerformanceValidation PRIVATE /O2 /DNDEBUG)
    endif()
endif()

# Threading support
set(THREADS_PREFER_PTHREAD_FLAG ON)

# Install targets
install(TARGETS OrderBook 
    OrderBookPerformanceValidation
    OrderBookCore 
    OrderBookNetwork 
    OrderBookUtilities 
    OrderBookMarketData 
    OrderBookRisk
    RUNTIME DESTINATION bin
    LIBRARY DESTINATION lib
    ARCHIVE DESTINATION lib
)

install(DIRECTORY include/ DESTINATION include)
install(DIRECTORY config/ DESTINATION config)
install(FILES README.md LICENSE DESTINATION .)