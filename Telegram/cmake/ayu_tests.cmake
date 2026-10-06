# This is the source code of AyuGram for Desktop.
#
# AyuGram: unit tests, configure with -D AYU_TESTS=ON.

add_executable(test_ayu_folder_lock)
init_target(test_ayu_folder_lock "(tests)")

target_include_directories(test_ayu_folder_lock PRIVATE ${src_loc})

nice_target_sources(test_ayu_folder_lock ${src_loc}
PRIVATE
    ayu/features/folder_lock/folder_lock_crypto.cpp
    ayu/features/folder_lock/folder_lock_crypto.h
    ayu/features/folder_lock/tests/folder_lock_crypto_tests.cpp
)

target_link_libraries(test_ayu_folder_lock
PRIVATE
    desktop-app::external_openssl
    desktop-app::external_qt
)

set_target_properties(test_ayu_folder_lock PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
)
