cmake_minimum_required(VERSION 3.20)

project(femtoFS LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(CTest)

option(FEMTOFS_SANITIZERS "Build with AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
option(FEMTOFS_FREEBSD_TESTS "Register privileged disposable-provider integration" OFF)
option(FEMTOFS_FUZZING "Build bounded builder libFuzzer entry points" OFF)
if (FEMTOFS_SANITIZERS)
    if (NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
        message(FATAL_ERROR "Sanitizers require Clang or GCC")
    endif ()
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer
                        -fno-sanitize-recover=undefined)
    add_link_options(-fsanitize=address,undefined)
endif ()

if (APPLE)
    list(PREPEND CMAKE_PREFIX_PATH
        "/opt/homebrew/opt/libarchive"
        "/usr/local/opt/libarchive"
    )
endif ()

find_package(LibArchive REQUIRED)

add_executable(femtofsSim
    programs/femtofsSim.cpp
)

target_include_directories(femtofsSim PRIVATE
    ${CMAKE_SOURCE_DIR}/include
)

target_compile_options(femtofsSim PRIVATE
    -Wall -Wextra -Wpedantic
)

add_executable(makefemtofs
    programs/makefemtofs.cpp
)

target_include_directories(makefemtofs PRIVATE
    ${CMAKE_SOURCE_DIR}/include
)

target_link_libraries(makefemtofs PRIVATE
    LibArchive::LibArchive
)

target_compile_options(makefemtofs PRIVATE
    -Wall -Wextra -Wpedantic
)

add_custom_target(run-femtofsSim
    COMMAND $<TARGET_FILE:femtofsSim> ${CMAKE_SOURCE_DIR}/misc/list
    DEPENDS femtofsSim
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    USES_TERMINAL
)

if (BUILD_TESTING)
    add_subdirectory(units)
    if (FEMTOFS_FREEBSD_TESTS)
        add_test(NAME makefemtofs-freebsd-provider
            COMMAND /bin/sh ${CMAKE_SOURCE_DIR}/units/test-makefemtofs-freebsd.sh
                    $<TARGET_FILE:makefemtofs> ${CMAKE_BINARY_DIR})
        set_tests_properties(makefemtofs-freebsd-provider PROPERTIES
            LABELS "freebsd;privileged" SKIP_RETURN_CODE 77 RUN_SERIAL TRUE)
    endif ()
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    add_test(NAME makefemtofs-cli-boundaries
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/units/test-builder-cli.py
                $<TARGET_FILE:makefemtofs> ${CMAKE_BINARY_DIR})
    add_test(
        NAME femtofsSim-format-accounting
        COMMAND /bin/sh
            ${CMAKE_SOURCE_DIR}/units/test-femtofsSim.sh
            $<TARGET_FILE:femtofsSim>
            ${CMAKE_BINARY_DIR}
    )
    add_test(
        NAME makefemtofs-directory-archive-equivalence
        COMMAND /bin/sh
            ${CMAKE_SOURCE_DIR}/units/test-makefemtofs.sh
            $<TARGET_FILE:makefemtofs>
            ${CMAKE_BINARY_DIR}
    )
endif ()

if (FEMTOFS_FUZZING)
    if (NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        message(FATAL_ERROR "libFuzzer targets require Clang")
    endif ()
    include(CheckCXXSourceCompiles)
    set(femtofs_saved_required_flags "${CMAKE_REQUIRED_FLAGS}")
    set(CMAKE_REQUIRED_FLAGS "${CMAKE_REQUIRED_FLAGS} -fsanitize=fuzzer")
    check_cxx_source_compiles("#include <cstddef>
        extern \"C\" int LLVMFuzzerTestOneInput(const unsigned char*, std::size_t) { return 0; }"
        FEMTOFS_HAS_LIBFUZZER)
    set(CMAKE_REQUIRED_FLAGS "${femtofs_saved_required_flags}")
    if (NOT FEMTOFS_HAS_LIBFUZZER)
        message(FATAL_ERROR "The selected Clang has no usable libFuzzer runtime; select a complete LLVM toolchain")
    endif ()
    foreach(kind paths plan)
        string(TOUPPER "${kind}" kind_upper)
        add_executable(fuzz-${kind} units/fuzz-makefemtofs.cpp)
        target_include_directories(fuzz-${kind} PRIVATE ${CMAKE_SOURCE_DIR}/include)
        target_link_libraries(fuzz-${kind} PRIVATE LibArchive::LibArchive)
        target_compile_definitions(fuzz-${kind} PRIVATE
            FEMTOFS_FUZZ_${kind_upper}=1 FEMTOFS_TEST_ROOT="${CMAKE_BINARY_DIR}")
        target_compile_options(fuzz-${kind} PRIVATE -fsanitize=fuzzer,address,undefined
            -fno-sanitize-recover=undefined -fno-omit-frame-pointer)
        target_link_options(fuzz-${kind} PRIVATE -fsanitize=fuzzer,address,undefined)
    endforeach()
endif ()
