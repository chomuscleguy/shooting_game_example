cmake_minimum_required(VERSION 3.20)
project(shooting_game_server CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Threads REQUIRED)

find_package(boost_asio CONFIG QUIET)
if(boost_asio_FOUND)
    set(BOOST_ASIO_TARGET Boost::asio)
else()
    find_package(Boost REQUIRED)
    set(BOOST_ASIO_TARGET Boost::boost)
endif()

add_executable(game_server src/main.cpp)
target_link_libraries(game_server PRIVATE ${BOOST_ASIO_TARGET} Threads::Threads)