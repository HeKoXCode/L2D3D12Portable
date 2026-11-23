cmake_minimum_required(VERSION 3.24)

project(L2D3D12Portable VERSION 0.2.0 LANGUAGES CXX)

if(NOT WIN32)
    message(FATAL_ERROR "L2D3D12Portable targets Windows only.")
endif()

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
    message(FATAL_ERROR "Classic L2 clients are x86. Configure a Win32 toolchain.")
endif()

add_library(l2_d3d9_proxy SHARED
    src/common/log.cpp
    src/common/log.h
    src/proxy/d3d9_hooks.cpp
    src/proxy/d3d9_hooks.h
    src/proxy/d3d9_proxy.cpp
    src/proxy/d3d9_proxy.def
)

target_include_directories(l2_d3d9_proxy PRIVATE src)
target_compile_features(l2_d3d9_proxy PRIVATE cxx_std_17)
target_compile_definitions(l2_d3d9_proxy PRIVATE
    UNICODE
    _UNICODE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)

if(MSVC)
    target_compile_options(l2_d3d9_proxy PRIVATE
        /W4
        /WX
        /permissive-
        /EHsc
        /guard:cf
    )
    target_link_options(l2_d3d9_proxy PRIVATE
        /DYNAMICBASE
        /NXCOMPAT
    )
endif()

set_target_properties(l2_d3d9_proxy PROPERTIES
    OUTPUT_NAME d3d9
    PREFIX ""
)

add_executable(proxy_smoke_test tests/proxy_smoke_test.cpp)
target_compile_features(proxy_smoke_test PRIVATE cxx_std_17)
target_compile_definitions(proxy_smoke_test PRIVATE
    UNICODE
    _UNICODE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)
if(MSVC)
    target_compile_options(proxy_smoke_test PRIVATE
        /W4
        /WX
        /permissive-
        /EHsc
    )
endif()
target_link_libraries(proxy_smoke_test PRIVATE user32)

enable_testing()

add_test(
    NAME proxy_native9_smoke_test
    COMMAND proxy_smoke_test "$<TARGET_FILE:l2_d3d9_proxy>" 3 native9
)
set_tests_properties(proxy_native9_smoke_test PROPERTIES
    ENVIRONMENT "INTERLUDE_D3D_BACKEND=native9;L2D3D12_SMOKE_HIDDEN=1;L2D3D12_TELEMETRY=0"
)

add_test(
    NAME proxy_9on12_smoke_test
    COMMAND proxy_smoke_test "$<TARGET_FILE:l2_d3d9_proxy>" 3 9on12
)
set_tests_properties(proxy_9on12_smoke_test PROPERTIES
    ENVIRONMENT "INTERLUDE_D3D_BACKEND=9on12;L2D3D12_SMOKE_HIDDEN=1;L2D3D12_TELEMETRY=0"
)

add_test(
    NAME proxy_invalid_backend_fallback_test
    COMMAND proxy_smoke_test "$<TARGET_FILE:l2_d3d9_proxy>" 3 native9
)
set_tests_properties(proxy_invalid_backend_fallback_test PROPERTIES
    ENVIRONMENT "INTERLUDE_D3D_BACKEND=invalid;L2D3D12_SMOKE_HIDDEN=1;L2D3D12_TELEMETRY=0"
)
