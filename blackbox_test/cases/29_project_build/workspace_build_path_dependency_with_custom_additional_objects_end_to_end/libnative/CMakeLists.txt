add_library(native_shim OBJECT src/native_value.cpp)
scpp_add_package(NAME libnative SOURCES src/libnative.scpp NATIVE_OBJECTS native_shim)
