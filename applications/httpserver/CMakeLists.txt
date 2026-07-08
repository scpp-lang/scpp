set(SCPP_HTTPSERVER_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/httpserver.scpp")
set(SCPP_HTTPSERVER_BINARY_PATH "${CMAKE_CURRENT_BINARY_DIR}/httpserver_app_bin")

add_library(scpp_httpserver_platform STATIC platform/scpp_httpserver_platform.cpp)
target_compile_features(scpp_httpserver_platform PRIVATE cxx_std_23)

add_custom_target(httpserver_app ALL
    COMMAND $<TARGET_FILE:scpp> build ${CMAKE_CURRENT_SOURCE_DIR}/main.scpp -o ${SCPP_HTTPSERVER_BINARY_PATH}
        --import httpserver=${SCPP_HTTPSERVER_MODULE_PATH}
        --link "$<TARGET_FILE:scpp_httpserver_platform>"
    BYPRODUCTS "${SCPP_HTTPSERVER_BINARY_PATH}"
    DEPENDS scpp scpp_httpserver_platform
        "${CMAKE_CURRENT_SOURCE_DIR}/main.scpp"
        "${SCPP_HTTPSERVER_MODULE_PATH}"
    VERBATIM)

add_executable(httpserver_test tests/httpserver_test.cpp)
add_dependencies(httpserver_test httpserver_app)
target_compile_features(httpserver_test PRIVATE cxx_std_23)
target_compile_definitions(httpserver_test PRIVATE
    SCPP_HTTPSERVER_BINARY_PATH="${SCPP_HTTPSERVER_BINARY_PATH}"
    SCPP_HTTPSERVER_TESTDATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/testdata"
    SCPP_HTTPSERVER_WORK_DIR="${CMAKE_CURRENT_BINARY_DIR}/test-work")
add_test(NAME httpserver_test COMMAND httpserver_test)
