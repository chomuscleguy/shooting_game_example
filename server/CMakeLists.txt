cmake_minimum_required(VERSION 3.20)
project(shooting_game_server CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Threads REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)

find_package(boost_asio CONFIG QUIET)
if(boost_asio_FOUND)
    set(BOOST_ASIO_TARGET Boost::asio)
else()
    find_package(Boost REQUIRED)
    set(BOOST_ASIO_TARGET Boost::boost)
endif()

add_executable(game_server src/main.cpp)    

target_link_libraries(game_server PRIVATE
    ${BOOST_ASIO_TARGET}
    Threads::Threads
    nlohmann_json::nlohmann_json
)

if(MSVC)
    target_compile_definitions(game_server PRIVATE _WIN32_WINNT=0x0A00)
    # 소스는 UTF-8인데 MSVC는 BOM이 없으면 시스템 코드페이지(한국어 환경이면 949)로 읽는다.
    # 한글 주석의 특정 바이트가 뒤따르는 문자를 삼켜서 코드가 사라지는 사고를 막기 위함.
    target_compile_options(game_server PRIVATE "/utf-8")
endif()