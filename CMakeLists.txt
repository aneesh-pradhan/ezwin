cmake_minimum_required(VERSION 3.30)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_BINARY_DIR)
    message(FATAL_ERROR "Use an out-of-source build: cmake --preset llvm")
endif()
set(required_toolchain "${CMAKE_CURRENT_LIST_DIR}/cmake/llvm-toolchain.cmake")
if(DEFINED CMAKE_TOOLCHAIN_FILE)
    get_filename_component(selected_toolchain "${CMAKE_TOOLCHAIN_FILE}" REALPATH BASE_DIR "${CMAKE_CURRENT_LIST_DIR}")
    if(NOT selected_toolchain STREQUAL required_toolchain)
        message(FATAL_ERROR "ezwin requires cmake/llvm-toolchain.cmake")
    endif()
endif()
set(CMAKE_TOOLCHAIN_FILE "${required_toolchain}" CACHE FILEPATH "Required LLVM toolchain" FORCE)

if(DEFINED CMAKE_CXX_STANDARD AND NOT CMAKE_CXX_STANDARD STREQUAL "26")
    message(FATAL_ERROR "ezwin requires C++26; CMAKE_CXX_STANDARD cannot be overridden.")
endif()
if(CMAKE_CXX_EXTENSIONS)
    message(FATAL_ERROR "ezwin requires ISO C++26 mode, without GNU C++ extensions.")
endif()
set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Toolchain-selection flags belong to the policy, not user optimization flags.
foreach(user_flags IN ITEMS "$ENV{CXXFLAGS}" "$ENV{CPPFLAGS}" "$ENV{LDFLAGS}")
    if(user_flags MATCHES "(^|[ ;])(-std=|--std=|-stdlib=|--stdlib=|--?rtlib=|--?unwindlib=|-fuse-ld=|--ld-path=|--config[= ])")
        message(FATAL_ERROR "Compiler, C++26, standard library and linker selections are managed by ezwin; remove toolchain-selection flags from the environment.")
    endif()
endforeach()

# Cache flags are also user-facing entry points. Accept only the policy's choices.
foreach(flag_variable IN ITEMS
    CMAKE_CXX_FLAGS CMAKE_CXX_FLAGS_DEBUG CMAKE_CXX_FLAGS_RELEASE
    CMAKE_CXX_FLAGS_RELWITHDEBINFO CMAKE_CXX_FLAGS_MINSIZEREL
    CMAKE_EXE_LINKER_FLAGS CMAKE_EXE_LINKER_FLAGS_DEBUG CMAKE_EXE_LINKER_FLAGS_RELEASE
    CMAKE_EXE_LINKER_FLAGS_RELWITHDEBINFO CMAKE_EXE_LINKER_FLAGS_MINSIZEREL)
    string(REGEX MATCHALL "--?(std|stdlib|rtlib|unwindlib|fuse-ld)=[^ \t]+" selections "${${flag_variable}}")
    foreach(selection IN LISTS selections)
        if(NOT selection MATCHES "^--?(std=c\\+\\+(26|2c)|stdlib=libc\\+\\+|rtlib=compiler-rt|unwindlib=libunwind|fuse-ld=lld)$")
            message(FATAL_ERROR "${flag_variable}: ${selection} conflicts with the pinned LLVM / C++26 policy.")
        endif()
    endforeach()
endforeach()

project(ezwin VERSION 0.5.0 LANGUAGES CXX)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/.llvm-version")
if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR "ezwin supports Linux only.")
endif()
if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "Clang" OR
   NOT CMAKE_CXX_COMPILER_VERSION VERSION_EQUAL EZWIN_LLVM_VERSION)
    message(FATAL_ERROR "ezwin requires LLVM Clang ${EZWIN_LLVM_VERSION} exactly.")
endif()

string(REPLACE "." ";" llvm_parts "${EZWIN_LLVM_VERSION}")
list(GET llvm_parts 0 EZWIN_LLVM_MAJOR)
list(GET llvm_parts 1 EZWIN_LLVM_MINOR)
list(GET llvm_parts 2 EZWIN_LLVM_PATCH)
math(EXPR EZWIN_LIBCPP_VERSION "${EZWIN_LLVM_MAJOR} * 10000 + ${EZWIN_LLVM_MINOR} * 100 + ${EZWIN_LLVM_PATCH}")
configure_file(cmake/toolchain-check.hpp.in generated/toolchain-check.hpp @ONLY)

# Apply policy at target level as well, after user-provided CMake flags.
add_library(ezwin_toolchain INTERFACE)
target_compile_options(ezwin_toolchain INTERFACE
    --no-default-config -stdlib=libc++
    -include "${CMAKE_CURRENT_BINARY_DIR}/generated/toolchain-check.hpp")
target_link_options(ezwin_toolchain INTERFACE
    --no-default-config -stdlib=libc++ "--ld-path=${CMAKE_LINKER}"
    --rtlib=compiler-rt --unwindlib=libunwind)
target_link_directories(ezwin_toolchain INTERFACE "${EZWIN_LLVM_RUNTIME_DIR}")

execute_process(COMMAND "${CMAKE_CXX_COMPILER}" --no-default-config --rtlib=compiler-rt --print-libgcc-file-name
    OUTPUT_VARIABLE builtins OUTPUT_STRIP_TRAILING_WHITESPACE RESULT_VARIABLE builtins_result)
if(NOT builtins_result EQUAL 0 OR NOT EXISTS "${builtins}" OR NOT builtins MATCHES "clang_rt\\.builtins")
    message(FATAL_ERROR "Pinned LLVM compiler-rt builtins are missing: ${builtins}")
endif()
cmake_path(IS_PREFIX LLVM_ROOT "${builtins}" NORMALIZE builtins_in_toolchain)
if(NOT builtins_in_toolchain)
    message(FATAL_ERROR "compiler-rt must come from ${LLVM_ROOT}; found ${builtins}")
endif()

include(CheckCXXSourceCompiles)
set(CMAKE_REQUIRED_FLAGS "--no-default-config -stdlib=libc++")
set(CMAKE_REQUIRED_INCLUDES "${CMAKE_CURRENT_BINARY_DIR}/generated")
unset(EZWIN_TOOLCHAIN_WORKS CACHE)
check_cxx_source_compiles("#include <toolchain-check.hpp>
    #include <string>
    #include <stdexcept>
    int main() { try { throw std::runtime_error(std::string(\"LLVM\")); }
        catch (const std::exception& e) { return e.what()[0] == 'L' ? 0 : 1; } }"
    EZWIN_TOOLCHAIN_WORKS)
if(NOT EZWIN_TOOLCHAIN_WORKS)
    message(FATAL_ERROR "The pinned LLVM toolchain must compile and link C++26 with libc++, libc++abi, compiler-rt and libunwind.")
endif()

find_package(PkgConfig REQUIRED)
pkg_check_modules(UDEV REQUIRED IMPORTED_TARGET libudev)
pkg_check_modules(MOUNT REQUIRED IMPORTED_TARGET mount)
pkg_check_modules(WIM REQUIRED IMPORTED_TARGET wimlib)
pkg_search_module(FDISK REQUIRED IMPORTED_TARGET fdisk libfdisk)

add_executable(ezwin src/main.cpp src/common.cpp src/devices.cpp src/disk.cpp src/flash.cpp)
target_compile_definitions(ezwin PRIVATE _GNU_SOURCE _FILE_OFFSET_BITS=64)
target_compile_options(ezwin PRIVATE -Wall -Wextra -Wpedantic -fstack-protector-strong)
target_include_directories(ezwin PRIVATE src)
target_link_libraries(ezwin PRIVATE ezwin_toolchain PkgConfig::UDEV PkgConfig::MOUNT PkgConfig::WIM PkgConfig::FDISK)
set_target_properties(ezwin PROPERTIES
    BUILD_RPATH "${EZWIN_LLVM_RUNTIME_DIR}"
    INSTALL_RPATH "${EZWIN_LLVM_RUNTIME_DIR}")

include(GNUInstallDirs)
install(TARGETS ezwin RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
install(FILES res/uefi-ntfs.img DESTINATION "${CMAKE_INSTALL_DATADIR}/ezwin")
enable_testing()
add_test(NAME cli-help COMMAND ezwin --help)
set_tests_properties(cli-help PROPERTIES PASS_REGULAR_EXPRESSION "Usage:")
add_test(NAME cli-version COMMAND ezwin --version)
set_tests_properties(cli-version PROPERTIES PASS_REGULAR_EXPRESSION "0\\.5\\.0")
add_executable(toolchain-smoke tests/toolchain-smoke.cpp)
target_link_libraries(toolchain-smoke PRIVATE ezwin_toolchain)
set_target_properties(toolchain-smoke PROPERTIES BUILD_RPATH "${EZWIN_LLVM_RUNTIME_DIR}")
add_test(NAME llvm-runtime COMMAND toolchain-smoke)
