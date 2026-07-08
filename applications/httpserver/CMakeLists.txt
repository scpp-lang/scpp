set(SCPP_HTTPSERVER_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/httpserver.scpp")
set(SCPP_HTTPSERVER_DEFAULT_BINARY_PATH "${CMAKE_CURRENT_BINARY_DIR}/httpserver_app_bin")
set(SCPP_HTTPSERVER_PLATFORM_LIB_PATH "${CMAKE_CURRENT_BINARY_DIR}/libscpp_httpserver_platform.a")

add_library(scpp_httpserver_platform STATIC platform/scpp_httpserver_platform.cpp)
target_compile_features(scpp_httpserver_platform PRIVATE cxx_std_23)
set_target_properties(scpp_httpserver_platform PROPERTIES OUTPUT_NAME scpp_httpserver_platform)

add_custom_target(httpserver_app ALL
    COMMAND $<TARGET_FILE:scpp> build ${CMAKE_CURRENT_SOURCE_DIR}/main.scpp -o ${SCPP_HTTPSERVER_DEFAULT_BINARY_PATH}
        --import httpserver=${SCPP_HTTPSERVER_MODULE_PATH}
        --link "${SCPP_HTTPSERVER_PLATFORM_LIB_PATH}"
    BYPRODUCTS "${SCPP_HTTPSERVER_DEFAULT_BINARY_PATH}"
    DEPENDS scpp scpp_httpserver_platform
        "${CMAKE_CURRENT_SOURCE_DIR}/main.scpp"
        "${SCPP_HTTPSERVER_MODULE_PATH}"
    VERBATIM)

add_executable(httpserver_test tests/httpserver_test.cpp)
add_dependencies(httpserver_test scpp_httpserver_platform)
target_compile_features(httpserver_test PRIVATE cxx_std_23)
target_compile_definitions(httpserver_test PRIVATE
    SCPP_BINARY_PATH="$<TARGET_FILE:scpp>"
    SCPP_HTTPSERVER_MODULE_PATH="${SCPP_HTTPSERVER_MODULE_PATH}"
    SCPP_HTTPSERVER_PLATFORM_LIB_PATH="${SCPP_HTTPSERVER_PLATFORM_LIB_PATH}"
    SCPP_HTTPSERVER_TESTDATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/testdata"
    SCPP_HTTPSERVER_WORK_DIR="${CMAKE_CURRENT_BINARY_DIR}/test-work")
add_test(NAME httpserver_test COMMAND httpserver_test)
