cmake_minimum_required(VERSION 3.20)

project(macast_bluetooth_audio LANGUAGES CXX)

add_executable(macast-bluetooth-audio WIN32 main.cpp)
target_compile_features(macast-bluetooth-audio PRIVATE cxx_std_17)

if(MSVC)
    target_compile_options(macast-bluetooth-audio PRIVATE /await /EHsc /utf-8)
endif()

target_link_libraries(macast-bluetooth-audio PRIVATE
    windowsapp
    ole32
    shell32
)

