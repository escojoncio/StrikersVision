# StrikersVision: the merge step of strikers_xros (cmake/VisionOSBundle.cmake), run with -P.
#   LIST          file with one archive path per line, the game's own first
#   OUT           the game's archive (a copy of the first)
#   OUT_DEPS      the others, merged
#   SDL_INCLUDE   SDL's include/ (SDL3/*.h)
#   DAWN_INCLUDE  Dawn's include/ (webgpu/, dawn/)
#   PORT_INCLUDE  the game's include/port
#   INCLUDE_OUT   where the headers go

file(STRINGS "${LIST}" _inputs)
set(_archives "")
foreach(_f IN LISTS _inputs)
    # Only archives: a dependency found in the SDK (a .tbd) is the app's to link (project.yml).
    if(_f MATCHES "\\.a$" AND EXISTS "${_f}")
        list(APPEND _archives "${_f}")
    elseif(_f)
        message(STATUS "strikers_xros: not merged (not a built archive): ${_f}")
    endif()
endforeach()
list(POP_FRONT _archives _game)
list(LENGTH _archives _n)
message(STATUS "strikers_xros: ${_game} -> ${OUT}; merging ${_n} archives into ${OUT_DEPS}")

get_filename_component(_dir "${OUT}" DIRECTORY)
file(MAKE_DIRECTORY "${_dir}")
file(COPY_FILE "${_game}" "${OUT}")
file(REMOVE "${OUT_DEPS}")
# libtool warns about members with the same name in different archives (abseil's and Dawn's
# hash.cc.o, say); they stay distinct members, which is what a static link needs.
execute_process(
    COMMAND xcrun libtool -static -no_warning_for_no_symbols -o "${OUT_DEPS}" ${_archives}
    RESULT_VARIABLE _rc
    ERROR_VARIABLE _err)
string(REGEX REPLACE "[^\n]*same member name[^\n]*\n?" "" _err "${_err}")
if(_err)
    message(STATUS "libtool: ${_err}")
endif()
if(NOT _rc EQUAL 0)
    message(FATAL_ERROR "libtool failed (${_rc})")
endif()

file(REMOVE_RECURSE "${INCLUDE_OUT}")
file(MAKE_DIRECTORY "${INCLUDE_OUT}")
file(COPY "${SDL_INCLUDE}/SDL3" DESTINATION "${INCLUDE_OUT}")
file(COPY "${DAWN_INCLUDE}/webgpu" "${DAWN_INCLUDE}/dawn" DESTINATION "${INCLUDE_OUT}")
# Not the whole of include/port: most of it needs the port prelude and the decomp's headers.
file(COPY "${PORT_INCLUDE}/xr_quest.h" "${PORT_INCLUDE}/xr_net.h" DESTINATION "${INCLUDE_OUT}/port")
