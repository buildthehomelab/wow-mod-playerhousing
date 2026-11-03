# The home screen (src/HomeScreen.cpp) needs a core that has it: stock AzerothCore builds the rest of
# the module without it.
file(READ "${CMAKE_SOURCE_DIR}/src/server/game/Scripting/ScriptDefines/PlayerScript.h" _playerhousing_script_h)
string(FIND "${_playerhousing_script_h}" "PLAYERHOOK_ON_HOME_VIEWER_LOGIN" _playerhousing_pos)
if (NOT _playerhousing_pos EQUAL -1)
    foreach (_playerhousing_target modules mod_mod-playerhousing)
        if (TARGET ${_playerhousing_target})
            target_compile_definitions(${_playerhousing_target} PRIVATE PLAYERHOUSING_CORE_HAS_HOME_SCREEN=1)
        endif()
    endforeach()
endif()
