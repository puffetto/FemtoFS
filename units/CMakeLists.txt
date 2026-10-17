include(FetchContent)

find_package(Catch2 3 QUIET)
if (NOT Catch2_FOUND)
    FetchContent_Declare(
        catch2
        GIT_REPOSITORY https://github.com/catchorg/Catch2.git
        GIT_TAG v3.6.0
    )
    FetchContent_MakeAvailable(catch2)
endif ()

include(Catch)

file(GLOB UNIT_TEST_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/test-*.cpp")

if (NOT UNIT_TEST_SOURCES)
    message(WARNING "No unit test sources found in ${CMAKE_CURRENT_SOURCE_DIR}")
endif ()

foreach(test_src IN LISTS UNIT_TEST_SOURCES)
    get_filename_component(test_name "${test_src}" NAME_WE)

    add_executable(${test_name} "${test_src}")
    set_target_properties(${test_name}
        PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/tests"
    )

    target_include_directories(${test_name}
        PRIVATE ${PROJECT_SOURCE_DIR}/include
    )
    target_link_libraries(${test_name}
        PRIVATE Catch2::Catch2WithMain
    )

    if (test_name STREQUAL "test-isPrime")
        find_package(OpenSSL REQUIRED)
        target_link_libraries(${test_name}
            PRIVATE OpenSSL::Crypto
        )
    endif ()

    if (test_name MATCHES "^test-(makefemtofs|builder-plan)$")
        target_link_libraries(${test_name} PRIVATE LibArchive::LibArchive)
        target_compile_definitions(${test_name} PRIVATE
            FEMTOFS_TEST_ROOT="${CMAKE_BINARY_DIR}")
        target_compile_options(${test_name} PRIVATE -Wall -Wextra -Wpedantic)
    endif ()

    catch_discover_tests(${test_name})
endforeach()
