# Copyright (c) 2026 Herman van Hazendonk <github.com@herrie.org>
#
# SPDX-License-Identifier: Apache-2.0
#
# Unit tests: src/identify_post.c against a fake libluna-service2
# (tests/fake_ls2.cpp) and the real pbnjson and glib.

enable_language(CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

pkg_check_modules(GTEST REQUIRED gtest_main)

add_executable(fingerprint-adapter-tests
    test_identify_post.cpp
    fake_ls2.cpp
    ${CMAKE_SOURCE_DIR}/src/identify_post.c)
target_include_directories(fingerprint-adapter-tests PRIVATE
    ${CMAKE_SOURCE_DIR}/src ${CMAKE_CURRENT_SOURCE_DIR} ${GTEST_INCLUDE_DIRS})
target_compile_options(fingerprint-adapter-tests PRIVATE -Wall -Wextra)
target_link_libraries(fingerprint-adapter-tests
    ${GTEST_LDFLAGS} ${PBNJSON_C_LDFLAGS} ${GLIB2_LDFLAGS} pthread)

add_test(NAME fingerprint-adapter-tests COMMAND fingerprint-adapter-tests)
