# StrikersVision: everything the visionOS app links from the game side, as one static archive.
#
#   strikers_visionos_bundle(<target> <out dir>)
#
# adds the target strikers_xros, which builds <target> and every static library it reaches (Aurora,
# SDL3, Dawn, abseil, fmt, imgui, freetype, libpng, zlib, sqlite, zstd, the disc codecs...) and
# leaves two archives in <out dir>, so the app (built by Xcode from project.yml) does not have to know
# the dependency graph CMake already knows:
#   libstrikers.a       the game's own objects. The app force-loads it: elsewhere these are linked as
#                       objects into the executable, constructors and all, and dead-stripped there.
#   libstrikers_deps.a  every library under it, merged with libtool, linked as usual.
# The headers the app's native code includes are copied beside it, under <out dir>/include: SDL3/,
# webgpu/ and dawn/ (Dawn's), and port/xr_quest.h + port/xr_net.h (the game's side of the XR layer).

function(_strikers_collect_static item out_files out_targets seen)
    set(_files "${${out_files}}")
    set(_targets "${${out_targets}}")
    set(_seen "${${seen}}")

    # $<LINK_ONLY:x> is how a static library's private dependency reaches its consumers, and
    # $<BUILD_INTERFACE:x> how a fetched project names its own targets.
    if(item MATCHES "^\\$<(LINK_ONLY|BUILD_INTERFACE|BUILD_LOCAL_INTERFACE):(.+)>$")
        set(item "${CMAKE_MATCH_2}")
    endif()
    # Other generator expressions are platform switches (-lrt, -ldbghelp...) or compile-only.
    if(item MATCHES "^\\$<" OR item MATCHES "^::@" OR item MATCHES "^-" OR NOT TARGET "${item}")
        return()
    endif()

    get_target_property(_aliased "${item}" ALIASED_TARGET)
    if(_aliased)
        set(item "${_aliased}")
    endif()
    if("${item}" IN_LIST _seen)
        return()
    endif()
    list(APPEND _seen "${item}")

    get_target_property(_type "${item}" TYPE)
    get_target_property(_imported "${item}" IMPORTED)
    if(_type STREQUAL "STATIC_LIBRARY")
        list(APPEND _files "$<TARGET_FILE:${item}>")
        if(NOT _imported)
            list(APPEND _targets "${item}")
        endif()
    endif()

    set(_deps "")
    if(NOT _imported AND NOT _type STREQUAL "INTERFACE_LIBRARY")
        get_target_property(_link "${item}" LINK_LIBRARIES)
        if(_link)
            list(APPEND _deps ${_link})
        endif()
    endif()
    get_target_property(_ilink "${item}" INTERFACE_LINK_LIBRARIES)
    if(_ilink)
        list(APPEND _deps ${_ilink})
    endif()

    set(${out_files} "${_files}" PARENT_SCOPE)
    set(${out_targets} "${_targets}" PARENT_SCOPE)
    set(${seen} "${_seen}" PARENT_SCOPE)
    foreach(_dep IN LISTS _deps)
        _strikers_collect_static("${_dep}" _files _targets _seen)
        set(${out_files} "${_files}" PARENT_SCOPE)
        set(${out_targets} "${_targets}" PARENT_SCOPE)
        set(${seen} "${_seen}" PARENT_SCOPE)
    endforeach()
endfunction()

function(strikers_visionos_bundle target out_dir)
    set(_files "")
    set(_targets "")
    set(_seen "")
    _strikers_collect_static("${target}" _files _targets _seen)
    list(REMOVE_DUPLICATES _files)

    # Generator expressions resolve at generate time; the merge script reads the list from here.
    set(_list "${CMAKE_BINARY_DIR}/strikers_xros_inputs.txt")
    string(REPLACE ";" "\n" _lines "${_files}")
    file(GENERATE OUTPUT "${_list}" CONTENT "${_lines}\n")

    # SDL's public headers: aurora/event.h includes <SDL3/SDL_events.h>.
    FetchContent_GetProperties(SDL SOURCE_DIR _sdl_src)
    get_target_property(_dawn_inc dawn::dawn_public_config INTERFACE_INCLUDE_DIRECTORIES)

    add_custom_target(strikers_xros
        COMMAND "${CMAKE_COMMAND}"
                -DLIST=${_list}
                -DOUT=${out_dir}/libstrikers.a
                -DOUT_DEPS=${out_dir}/libstrikers_deps.a
                -DSDL_INCLUDE=${_sdl_src}/include
                -DDAWN_INCLUDE=${_dawn_inc}
                -DPORT_INCLUDE=${PROJECT_SOURCE_DIR}/include/port
                -DINCLUDE_OUT=${out_dir}/include
                -P "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/VisionOSMerge.cmake"
        COMMENT "libstrikers.a for visionOS"
        VERBATIM)
    add_dependencies(strikers_xros ${_targets})

    list(LENGTH _files _n)
    message(STATUS "StrikersVision: libstrikers.a merges ${_n} static libraries")
endfunction()
