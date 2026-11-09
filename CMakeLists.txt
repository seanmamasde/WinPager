cmake_minimum_required(VERSION 3.25)
project(WinPager VERSION 0.1.1 LANGUAGES CXX RC)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)

add_executable(WinPager WIN32
    src/main.cpp
    src/app.cpp
    src/pager.cpp
    src/model.cpp
    src/settings.cpp
    src/theme.cpp
    src/vdapi.cpp
    res/app.rc
    res/app.manifest
)
target_include_directories(WinPager PRIVATE src res)
target_compile_definitions(WinPager PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX
    WINPAGER_VERSION=L"${PROJECT_VERSION}")
target_compile_options(WinPager PRIVATE /W4 /permissive- /utf-8 /GR- /Gy /Gw)
target_link_options(WinPager PRIVATE /OPT:REF /OPT:ICF)
target_link_libraries(WinPager PRIVATE
    d2d1 dwrite windowscodecs dwmapi uxtheme comctl32 shcore shlwapi runtimeobject)

enable_testing()
add_executable(selftest tests/selftest.cpp)
target_include_directories(selftest PRIVATE src)
target_compile_definitions(selftest PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(selftest PRIVATE /W4 /utf-8)
add_test(NAME selftest COMMAND selftest)
