set(sources
    ${CMAKE_CURRENT_SOURCE_DIR}/src/fs.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/detail/file_lock.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/detail/hash.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/detail/json.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/store.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/view.cpp
)

set(private_headers
    ${CMAKE_CURRENT_SOURCE_DIR}/src/detail/error.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/detail/file_lock.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/detail/hash.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/detail/json.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/detail/scope_exit.hpp
)

file(GLOB bin_sources CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/src/bin/*.cpp
)
set(cli_sources
    ${CMAKE_CURRENT_SOURCE_DIR}/src/cli/agent.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/cli/skill.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/cli/dispatch.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/cli/store.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/cli/view.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/cli/tui.cpp
)
set(cli_headers
    ${CMAKE_CURRENT_SOURCE_DIR}/src/cli/commands.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/cli/cli_common.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/cli/tui_model.hpp
)

set(headers
    ${CMAKE_CURRENT_SOURCE_DIR}/include/memix/error.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/include/memix/fs.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/include/memix/store.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/include/memix/view.hpp
)

set(test_sources
    ${CMAKE_CURRENT_SOURCE_DIR}/test/src/cli_test.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/test/src/skill_test.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/test/src/activation_test.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/test/src/hash_test.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/test/src/json_test.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/test/src/tui_model_test.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/test/src/quest_test.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/test/src/store_test.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/test/src/view_test.cpp
)
