cmake_minimum_required(VERSION 3.24)
project(smstrikers-port CXX C)
include(cmake/StrikersVersion.cmake)

# StrikersVision: Apple Vision Pro (CMAKE_SYSTEM_NAME visionOS, sysroot xros). The process belongs to
# the visionOS app (visionos/), so the game is not an executable there but one static library the app
# links: the game's main() becomes strikers_main(), called on a thread of the app's (see
# visionos/include/sv_bridge.h), and Aurora runs headless, its frames going to Compositor Services.
# SV_VISIONOS is defined for every target from here down, Aurora and its dependencies included.
if(CMAKE_SYSTEM_NAME STREQUAL "visionOS")
    set(STRIKERS_VISIONOS ON)
    # PORT_XR_HEADSET: a headset build (Quest with STRIKERS_XR, or Vision Pro), for code that has to
    # tell one apart from a flat build at compile time.
    add_compile_definitions(SV_VISIONOS=1 PORT_XR_HEADSET=1)
else()
    set(STRIKERS_VISIONOS OFF)
endif()

# Super Mario Strikers, native port Targets macOS (arm64 and x86_64) first, but nothing here is
# Darwin-specific: the platform surface is confined to src/platform/ and to Aurora, both of which
# already cover Windows and Linux.

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 99)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Toolchain contract: clang, on every platform.
if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    message(FATAL_ERROR
        "This port requires clang (found ${CMAKE_CXX_COMPILER_ID}). "
        "See the toolchain contract in CMakeLists.txt.")
endif()

if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/Game/main.cpp")
    message(FATAL_ERROR
        "src/Game/main.cpp is missing. The game sources are committed in this "
        "tree, so this checkout is incomplete.")
endif()

# Build options. Declared ahead of the first line that reads them; the order matters.

# Heap bugs in ported code tend to surface far from their cause, a stray write through a truncated
# pointer shows up as a crash inside malloc much later.
option(STRIKERS_ASAN "Build with AddressSanitizer" OFF)

# Aurora is a drop-in replacement for the Dolphin SDK: it defines the real GX/VI/PAD/DVD/CARD/SI
# symbols over WebGPU and SDL3.
option(STRIKERS_AURORA "Build against Aurora (real GX/PAD/CARD/DVD)" ON)

# include/msl links only the Metrowerks headers whose names do not collide with host ones, so
# <stdio.h>, <math.h> and the rest resolve to the host.
set(PORT_INCLUDES
    # Ahead of Aurora: forwards only dolphin/mtx.h back to the decomp's, which has the paired-single
    # entry points Aurora lacks.
    $<$<BOOL:${STRIKERS_AURORA}>:${CMAKE_CURRENT_SOURCE_DIR}/include/port/aurora-overlay>
    $<$<BOOL:${STRIKERS_AURORA}>:${CMAKE_CURRENT_SOURCE_DIR}/extern/aurora/include>
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/include/msl"
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${CMAKE_CURRENT_SOURCE_DIR}/extern/musyx/include"
)

# The build type, as a string, so a measurement can say which build produced it.
if(CMAKE_BUILD_TYPE)
    set(PORT_BUILD_TYPE_STR "${CMAKE_BUILD_TYPE}")
else()
    set(PORT_BUILD_TYPE_STR "unspecified")
endif()

set(PORT_DEFINES
    TARGET_PC=1
    PORT_BUILD_TYPE="${PORT_BUILD_TYPE_STR}"
    # MSVC's <math.h> defines M_PI and friends only when this is set; every other libc defines them
    # unconditionally, so nlMath.cpp's `M_PI / 2.0f` compiled everywhere except Windows.
    _USE_MATH_DEFINES
    # No VERSION_G4Q* define: the region is resolved at runtime from the disc id.
    dSINGLE                    # ODE single precision
    # MusyX picks its ABI with `#if MUSY_TARGET == MUSY_TARGET_DOLPHIN`, which is evaluated in
    # headers reached before musyx/version.h defines either name, both expand to 0 and the console
    # branch wins by accident.
    MUSY_TARGET_PC=0
    MUSY_TARGET_DOLPHIN=1
    MUSY_TARGET=0
    # The MusyX the game shipped with: the decomp builds game objects at 2.0.3 while musyx/version.h
    # defaults to 2.0.0, so the two would disagree on version-gated layouts and lose the group-stack
    # API.
    MUSY_VERSION_MAJOR=2
    MUSY_VERSION_MINOR=0
    MUSY_VERSION_PATCH=3
    $<$<BOOL:${STRIKERS_AURORA}>:PORT_USE_AURORA=1>
)

if(MSVC AND STRIKERS_AURORA)
    # Aurora's Windows Dawn package contains only a Release archive, so its C++ ABI is /MT with
    # iterator debugging disabled even when this project is a Debug build.
    add_compile_definitions(_ITERATOR_DEBUG_LEVEL=0)
endif()

if(STRIKERS_AURORA)
    # OFF: src/platform/dvd.c provides DVD, reading an extracted disc directory rather than a disc
    # image.
    set(AURORA_ENABLE_DVD   OFF CACHE BOOL "" FORCE)
    set(AURORA_ENABLE_CARD  ON  CACHE BOOL "" FORCE)
    set(AURORA_ENABLE_RMLUI OFF CACHE BOOL "" FORCE)

    if(STRIKERS_VISIONOS)
        # SDL for audio, controllers and events only. Its video subsystem is UIKit, which wants the
        # main thread and a window of its own, and the app already owns both; Aurora creates no
        # window there (extern/aurora/lib/window.cpp).
        set(AURORA_SDL3_PROVIDER vendor CACHE STRING "" FORCE)
        set(AURORA_SDL3_LINKAGE static CACHE STRING "" FORCE)
        set(SDL_VIDEO OFF CACHE BOOL "" FORCE)
        set(SDL_SYSTEM_ICONV OFF CACHE BOOL "" FORCE)
        set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
        # libpng defaults to a framework bundle on Apple; Aurora links the static library.
        set(PNG_FRAMEWORK OFF CACHE BOOL "" FORCE)
        # Dawn: Aurora's prebuilt iOS package, its objects retargeted to visionOS
        # (scripts/fetch-dawn-xros.sh). Same arm64 code against the same Metal API; there is no
        # prebuilt visionOS package.
        if(NOT STRIKERS_DAWN_XROS OR NOT EXISTS "${STRIKERS_DAWN_XROS}/lib/cmake/Dawn/DawnConfig.cmake")
            message(FATAL_ERROR "visionOS: -DSTRIKERS_DAWN_XROS=<dir> must name the Dawn tree "
                                "scripts/fetch-dawn-xros.sh makes")
        endif()
        # Found here, outside the sysroot (NO_CMAKE_FIND_ROOT_PATH: a cross build otherwise looks only
        # inside the SDK), and global, so Aurora's "system" provider takes it as found.
        # The package's interface names Threads::Threads, which only Aurora's package path looks up.
        find_package(Threads REQUIRED)
        set(CMAKE_FIND_PACKAGE_TARGETS_GLOBAL ON)
        find_package(Dawn REQUIRED CONFIG
            PATHS "${STRIKERS_DAWN_XROS}/lib/cmake/Dawn"
            NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH)
        set(CMAKE_FIND_PACKAGE_TARGETS_GLOBAL OFF)
        set(AURORA_DAWN_PROVIDER system CACHE STRING "" FORCE)
    endif()

    # std::jthread is probed rather than version-checked; an older libc++ has it only under
    # -fexperimental-library.
    include(CheckCXXSourceCompiles)
    set(_jthread_src [[
#include <thread>
#include <stop_token>
int main() { std::jthread t([](std::stop_token) {}); }
]])
    set(_saved_cxx_standard "${CMAKE_CXX_STANDARD}")
    set(_saved_required_flags "${CMAKE_REQUIRED_FLAGS}")
    set(CMAKE_CXX_STANDARD 20)
    check_cxx_source_compiles("${_jthread_src}" PORT_JTHREAD_PLAIN)
    if(NOT PORT_JTHREAD_PLAIN)
        set(CMAKE_REQUIRED_FLAGS
            "${_saved_required_flags} -fexperimental-library")
        check_cxx_source_compiles("${_jthread_src}" PORT_JTHREAD_EXPERIMENTAL)
        if(NOT PORT_JTHREAD_EXPERIMENTAL)
            message(FATAL_ERROR
                "Aurora needs std::jthread and std::stop_token, and this "
                "standard library has them neither plainly nor under "
                "-fexperimental-library. A newer libc++ (Apple clang 17, or "
                "Xcode 17) is required.")
        endif()
        message(STATUS "strikers: libc++ needs -fexperimental-library for std::jthread")
        add_compile_options(
            "$<$<COMPILE_LANGUAGE:CXX>:-fexperimental-library>")
        add_link_options(
            "$<$<LINK_LANGUAGE:CXX>:-fexperimental-library>")
    endif()
    set(CMAKE_REQUIRED_FLAGS "${_saved_required_flags}")
    unset(_saved_required_flags)
    set(CMAKE_CXX_STANDARD "${_saved_cxx_standard}")

    include(FetchContent)
    # Freetype from upstream's own mirrors: Aurora fetches it from a private one that now answers
    # 403.
    FetchContent_Declare(Freetype
        URL https://downloads.sourceforge.net/project/freetype/freetype2/2.14.3/freetype-2.14.3.tar.gz
            https://download.savannah.gnu.org/releases/freetype/freetype-2.14.3.tar.gz
        URL_HASH SHA256=e61b31ab26358b946e767ed7eb7f4bb2e507da1cfefeb7a8861ace7fd5c899a1
        DOWNLOAD_EXTRACT_TIMESTAMP FALSE
        EXCLUDE_FROM_ALL
    )
    # Freetype's bzip2 support decompresses bzip2-compressed PCF fonts and nothing else.
    set(FT_DISABLE_BZIP2 ON CACHE BOOL "" FORCE)

    # Overriding an upstream URL is only right while that URL is still the one upstream uses.
    file(READ "${CMAKE_CURRENT_SOURCE_DIR}/extern/aurora/extern/CMakeLists.txt"
         _aurora_extern)
    string(FIND "${_aurora_extern}" "files.twilitrealm.dev/freetype" _ft_anchor)
    unset(_aurora_extern)
    if(_ft_anchor EQUAL -1)
        message(FATAL_ERROR
            "Aurora no longer fetches Freetype from files.twilitrealm.dev, so "
            "the FetchContent_Declare(Freetype) override above is stale; "
            "re-review it. If upstream's own URL works now, delete it.")
    endif()
    add_subdirectory(extern/aurora EXCLUDE_FROM_ALL)
endif()

add_library(port_flags INTERFACE)
target_include_directories(port_flags INTERFACE ${PORT_INCLUDES})
target_compile_definitions(port_flags INTERFACE ${PORT_DEFINES})
# The compile contract, in both of clang's command-line dialects.
if(MSVC)
    target_compile_options(port_flags INTERFACE
        /clang:-fdeclspec
        /clang:-fshort-wchar
        "/FI${CMAKE_CURRENT_SOURCE_DIR}/include/port/prelude.h"
        /clang:-fno-strict-aliasing
        /clang:-fsigned-char
        -Wno-invalid-offsetof
        # A pointer truncated into a smaller integer is a build error here, as it already is
        # everywhere else.
        /clang:-Werror=pointer-to-int-cast
        /clang:-Werror=void-pointer-to-int-cast
        $<$<BOOL:${STRIKERS_ASAN}>:-fsanitize=address>
    )
else()
    target_compile_options(port_flags INTERFACE
        -fdeclspec
        -fshort-wchar
        # The game's 16-bit string loops (nlStrLen<unsigned short> and friends) match clang's wcslen
        # idiom once wchar_t is 16 bits, and get replaced by a call to the C library's wcslen, which
        # counts 32-bit units everywhere but Windows: a garbage length, then a huge alloca in
        # nlFont::DrawString and a stack overflow on the first long text.
        -fno-builtin-wcslen
        -include "${CMAKE_CURRENT_SOURCE_DIR}/include/port/prelude.h"
        -fno-strict-aliasing
        -fsigned-char
        -Wno-invalid-offsetof
        $<$<BOOL:${STRIKERS_ASAN}>:-fsanitize=address>
        $<$<BOOL:${STRIKERS_ASAN}>:-fno-omit-frame-pointer>
    )
endif()
if(STRIKERS_ASAN)
    target_link_options(port_flags INTERFACE -fsanitize=address)
endif()

if(STRIKERS_AURORA)
    # aurora/event.h exposes SDL_Event, so the game needs SDL3 on its include path.
    target_include_directories(port_flags INTERFACE
        $<TARGET_PROPERTY:aurora::core,INTERFACE_INCLUDE_DIRECTORIES>)
endif()

# Source sets src/Dolphin, src/PowerPC_EABI_Support and src/SDK are vendored but not built: Aurora
# supplies GX/PAD/DVD/CARD and the host supplies libc/libc++.
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/build-sources.txt" _built
     REGEX "^[^#]")
function(collect_sources out_var)
    set(_all "")
    foreach(_rel ${_built})
        foreach(_pre ${ARGN})
            if(_rel MATCHES "^${_pre}/")
                list(APPEND _all "${CMAKE_CURRENT_SOURCE_DIR}/src/${_rel}")
            endif()
        endforeach()
    endforeach()
    set(${out_var} ${_all} PARENT_SCOPE)
endfunction()

collect_sources(GAME_SOURCES Game NL)

# src/NL/MemAlloc.cpp is replaced by src/platform/memalloc.cpp: its free-block allocator is 32-bit
# PowerPC in its on-heap layout and has no reason to survive the port.
list(REMOVE_ITEM GAME_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src/NL/MemAlloc.cpp")
collect_sources(ODE_SOURCES  ode)

# ODE pieces the decomp does not build but the port needs to link: matrix.cpp calls dDot/dSolveL1
# and ode.cpp calls dRand, all of which live in files outside configure.py's object list (the retail
# build presumably got them from a prebuilt library).
set(ODE_EXTRA_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/src/ode/fastdot.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/ode/fastlsolve.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/ode/fastltsolve.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/ode/fastldlt.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/ode/misc.cpp"
    # step.cpp holds dInternalStepIsland, the constraint solver the world step calls into.
    "${CMAKE_CURRENT_SOURCE_DIR}/src/ode/step.cpp"
)
set_source_files_properties(${ODE_EXTRA_SOURCES} PROPERTIES LANGUAGE CXX)
list(APPEND ODE_SOURCES ${ODE_EXTRA_SOURCES})
file(GLOB_RECURSE PLATFORM_SOURCES CONFIGURE_DEPENDS
     "${CMAKE_CURRENT_SOURCE_DIR}/src/platform/*.cpp"
     "${CMAKE_CURRENT_SOURCE_DIR}/src/platform/*.c")

# src/platform/vulkan_shim.c is not part of the game at all: it is its own shared library, and
# compiled in here it would export vkGetInstanceProcAddr from libmain.so, where nothing should find
# a Vulkan entry point.
list(REMOVE_ITEM PLATFORM_SOURCES
     "${CMAKE_CURRENT_SOURCE_DIR}/src/platform/vulkan_shim.c")
# strikers-quest r41: its own library without the prelude (port_disc_codecs, below).
list(REMOVE_ITEM PLATFORM_SOURCES
     "${CMAKE_CURRENT_SOURCE_DIR}/src/platform/disc_codecs.c")

# strikers-quest: src/platform/xr_quest.cpp is built on its own (strikers_xr, below): C++17 and
# without the port prelude, which the Vulkan and OpenXR headers do not survive.
list(REMOVE_ITEM PLATFORM_SOURCES
     "${CMAKE_CURRENT_SOURCE_DIR}/src/platform/xr_quest.cpp"
     "${CMAKE_CURRENT_SOURCE_DIR}/src/platform/xr_fx.cpp"
     "${CMAKE_CURRENT_SOURCE_DIR}/src/platform/xr_net.cpp")

# src/platform/operator_new.cpp is the one file in the tree that is not C++11.
list(REMOVE_ITEM PLATFORM_SOURCES
     "${CMAKE_CURRENT_SOURCE_DIR}/src/platform/operator_new.cpp")
add_library(strikers_operators OBJECT
     "${CMAKE_CURRENT_SOURCE_DIR}/src/platform/operator_new.cpp")
set_target_properties(strikers_operators PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON)

# The debug overlay is ImGui, and ImGui arrives with Aurora and only with Aurora: with
# STRIKERS_AURORA=OFF none of overlay.cpp's includes are on the path, so it leaves the source set
# rather than becoming an empty translation unit.
if(NOT STRIKERS_AURORA)
    list(REMOVE_ITEM PLATFORM_SOURCES
        "${CMAKE_CURRENT_SOURCE_DIR}/src/platform/overlay.cpp")
endif()

if(STRIKERS_AURORA)
    # Aurora implements PAD (SDL_Gamepad, GC adapters, rumble, remapping) far better than the
    # placeholder here, so pad.c goes.
    list(REMOVE_ITEM PLATFORM_SOURCES
        "${CMAKE_CURRENT_SOURCE_DIR}/src/platform/pad.c")
endif()

# Portable slices of the Dolphin SDK: MTX, QUAT and VEC are pure computation Aurora does not
# provide,; the paired-single entry points forward to the C_ twins in the same files.
set(SDK_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/src/Dolphin/mtx/mtx.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/Dolphin/mtx/mtx44.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/Dolphin/mtx/quat.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/Dolphin/mtx/vec.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/Dolphin/mtx/mtxvec.c"
)
add_library(strikers_sdk      OBJECT ${SDK_SOURCES})

# MusyX, from the decomp submodule. It is self-contained and must NOT get the port prelude: musyx.h
# typedefs `bool` itself under C99, which collides with the <stdbool.h> the prelude pulls in for C
# translation units.
set(MUSYX_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/extern/musyx/src/musyx/runtime/hw_pc.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/extern/musyx/src/musyx/runtime/CheapReverb/creverb.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/extern/musyx/src/musyx/runtime/CheapReverb/creverb_fx.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/extern/musyx/src/musyx/runtime/Chorus/chorus_fx.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/extern/musyx/src/musyx/runtime/Delay/delay_fx.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/extern/musyx/src/musyx/runtime/StdReverb/reverb.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/extern/musyx/src/musyx/runtime/StdReverb/reverb_fx.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/extern/musyx/src/musyx/runtime/dsp_import.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/extern/musyx/src/musyx/runtime/hardware.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/extern/musyx/src/musyx/runtime/hw_aramdma.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/extern/musyx/src/musyx/runtime/hw_dspctrl.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/extern/musyx/src/musyx/runtime/hw_lib_dolphin.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/extern/musyx/src/musyx/runtime/hw_memory.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/extern/musyx/src/musyx/runtime/hw_volconv.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/extern/musyx/src/musyx/runtime/s_data.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/extern/musyx/src/musyx/runtime/seq.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/extern/musyx/src/musyx/runtime/seq_api.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/extern/musyx/src/musyx/runtime/snd3d.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/extern/musyx/src/musyx/runtime/snd_init.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/extern/musyx/src/musyx/runtime/snd_math.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/extern/musyx/src/musyx/runtime/snd_midictrl.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/extern/musyx/src/musyx/runtime/snd_service.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/extern/musyx/src/musyx/runtime/snd_synthapi.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/extern/musyx/src/musyx/runtime/stream.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/extern/musyx/src/musyx/runtime/synth.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/extern/musyx/src/musyx/runtime/synth_ac.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/extern/musyx/src/musyx/runtime/synth_adsr.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/extern/musyx/src/musyx/runtime/synth_dbtab.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/extern/musyx/src/musyx/runtime/synth_vsamples.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/extern/musyx/src/musyx/runtime/synthdata.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/extern/musyx/src/musyx/runtime/synthmacros.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/extern/musyx/src/musyx/runtime/synthvoice.c"
)
add_library(strikers_musyx OBJECT ${MUSYX_SOURCES})
target_include_directories(strikers_musyx PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/extern/musyx/include")
# Version gates real API: sndStackGetSize/sndStackAdd and friends sit behind `#if MUSY_VERSION >=
# MUSY_VERSION_CHECK(2, 0, 2)`, and the header defaults to 2.0.0.
target_compile_definitions(strikers_musyx PRIVATE
    MUSY_TARGET=0
    _USE_MATH_DEFINES
    MUSY_VERSION_MAJOR=2
    MUSY_VERSION_MINOR=0
    MUSY_VERSION_PATCH=3
    # MusyX does not link port_flags and so does not get PORT_DEFINES, which is where
    # _USE_MATH_DEFINES lives; and hardware.c's DPL2 coefficient uses M_PI.
    _USE_MATH_DEFINES)
target_compile_options(strikers_musyx PRIVATE -fdeclspec -w -std=gnu11)

add_library(strikers_ode      OBJECT ${ODE_SOURCES})
add_library(strikers_game     OBJECT ${GAME_SOURCES})
add_library(strikers_platform OBJECT ${PLATFORM_SOURCES})

foreach(_t strikers_ode strikers_game strikers_platform strikers_sdk
          strikers_operators)
    target_link_libraries(${_t} PRIVATE port_flags)
endforeach()

# strikers_scan is the cheap per-push check: every in-scope translation unit compiled, nothing
# linked.
add_custom_target(strikers_scan
    DEPENDS strikers_ode strikers_game strikers_platform strikers_sdk
            strikers_musyx strikers_operators)

# The link test. Aurora, MusyX and the THP decoder are not wired up, so tools/genstubs.py supplies
# an abort-on-call definition for each symbol they would provide.
set(_strikers_objects
    $<TARGET_OBJECTS:strikers_game>
    $<TARGET_OBJECTS:strikers_ode>
    $<TARGET_OBJECTS:strikers_sdk>
    $<TARGET_OBJECTS:strikers_platform>
    $<TARGET_OBJECTS:strikers_operators>
    $<TARGET_OBJECTS:strikers_musyx>
)

if(ANDROID)
    # An Android application has no executable of its own: the process is started by the runtime and
    # the native side is a shared object it dlopen()s. SDL's activity loads the library named by
    # getMainSharedObject(), "main" by default, and calls SDL_main in it, which SDL_main.h has
    # already #defined main to be. So the same objects, linked as libmain.so.
    add_library(strikers SHARED ${_strikers_objects})
    set_target_properties(strikers PROPERTIES OUTPUT_NAME main)

    # The Vulkan stand-in: its own shared library, named libvulkan.so, so that Dawn's
    # dlopen("libvulkan.so") finds it instead of the system loader. See src/platform/vulkan_shim.c.
    # Deliberately NOT linked to anything here -- it is loaded by name at runtime, by Dawn, and the
    # game has no reference to it.
    add_library(strikers_vulkan_shim SHARED
        "${CMAKE_CURRENT_SOURCE_DIR}/src/platform/vulkan_shim.c")
    set_target_properties(strikers_vulkan_shim PROPERTIES OUTPUT_NAME vulkan)
    target_link_libraries(strikers_vulkan_shim PRIVATE log)
    # Hidden by default, so the one entry point is the only thing that answers.
    target_compile_options(strikers_vulkan_shim PRIVATE -fvisibility=hidden)

    # libadrenotools, which is how a driver the system does not ship gets loaded at all: Android's
    # linker will not dlopen a library outside the app's own namespace, and this sets up a namespace
    # that can. Fetched rather than vendored, like Dawn and SDL, and pinned to a commit.
    include(FetchContent)
    FetchContent_Declare(adrenotools
        GIT_REPOSITORY https://github.com/bylaws/libadrenotools.git
        GIT_TAG 8fae8ce254dfc1344527e05301e43f37dea2df80
        GIT_SUBMODULES_RECURSE TRUE
        EXCLUDE_FROM_ALL
    )
    # C++20 for its directory only. This project is C++11 because the decompiled game is, and that
    # standard leaks into anything added below it: adrenotools has a union with a default member
    # initializer, which C++11 says is not an aggregate, so its brace initialisation does not
    # compile. Saved and put back so nothing else inherits the change.
    set(_saved_cxx_standard_adreno "${CMAKE_CXX_STANDARD}")
    set(CMAKE_CXX_STANDARD 20)
    FetchContent_MakeAvailable(adrenotools)
    set(CMAKE_CXX_STANDARD "${_saved_cxx_standard_adreno}")
    unset(_saved_cxx_standard_adreno)
    target_link_libraries(strikers_vulkan_shim PRIVATE adrenotools)

    # The hooks are separate shared objects that adrenotools loads by name out of the app's library
    # directory, so they have to be packaged even though nothing links against them.
    add_dependencies(strikers_vulkan_shim main_hook file_redirect_hook gsl_alloc_hook hook_impl)
    add_dependencies(strikers strikers_vulkan_shim)

    # SDL_main has no caller. It is what SDLActivity looks up by name once the library is loaded,
    # so nothing inside the library references it -- and aurora::main is a static archive, whose
    # members are pulled in only to satisfy a reference. Without this the link succeeds, the .so
    # loads, and the app dies at "Failed to find function SDL_main". -u is the reference.
    target_link_options(strikers PRIVATE "-Wl,-u,SDL_main")
elseif(STRIKERS_VISIONOS)
    # The objects as an archive; strikers_xros (cmake/VisionOSBundle.cmake, below) merges it with every
    # static library it depends on into the one libstrikers.a the app links.
    add_library(strikers STATIC ${_strikers_objects})
    set_target_properties(strikers PROPERTIES OUTPUT_NAME strikers_game)
else()
    add_executable(strikers ${_strikers_objects})
endif()
target_link_libraries(strikers PRIVATE port_flags)

# The executable's icon on Windows: the game's memory card icon, from assets/icon. The files are
# checked in; the build only compiles the resource.
if(WIN32)
    target_sources(strikers PRIVATE assets/icon/strikers.rc)
    set_source_files_properties(assets/icon/strikers.rc PROPERTIES
        INCLUDE_DIRECTORIES "${CMAKE_CURRENT_BINARY_DIR}/version"
        OBJECT_DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/version/strikers_version.h")

    # /ENTRY:mainCRTStartup keeps main() as the entry point of a GUI-subsystem executable.
    set_target_properties(strikers PROPERTIES WIN32_EXECUTABLE TRUE)
    if(MSVC)
        target_link_options(strikers PRIVATE /ENTRY:mainCRTStartup)
    endif()
endif()

# The name macOS shows in the menu bar, from assets/macos/Info.plist.
if(APPLE AND NOT STRIKERS_VISIONOS)
    configure_file(assets/macos/Info.plist "${CMAKE_CURRENT_BINARY_DIR}/Info.plist" @ONLY)
    target_link_options(strikers PRIVATE
        "-Wl,-sectcreate,__TEXT,__info_plist,${CMAKE_CURRENT_BINARY_DIR}/Info.plist")
    # Relink when the plist changes; a link option is not a dependency.
    set_property(TARGET strikers APPEND PROPERTY
        LINK_DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/Info.plist")
endif()

option(STRIKERS_FFMPEG "Decode THP movies with libavcodec" ON)
set(STRIKERS_FFMPEG_ROOT "" CACHE PATH
    "An unpacked ffmpeg (include/, lib/, and bin/ on Windows), for hosts with no pkg-config")
set(_ff_static "${CMAKE_CURRENT_SOURCE_DIR}/extern/ffmpeg-static")

if(STRIKERS_FFMPEG)
    find_package(PkgConfig QUIET)
    if(PKG_CONFIG_FOUND AND NOT STRIKERS_FFMPEG_ROOT AND NOT DEFINED ENV{STRIKERS_FFMPEG_ROOT}
       AND NOT EXISTS "${_ff_static}/lib/libavcodec.a" AND NOT EXISTS "${_ff_static}/lib/avcodec.lib")
        if(APPLE AND EXISTS "/opt/homebrew/opt/ffmpeg/lib/pkgconfig")
            set(ENV{PKG_CONFIG_PATH}
                "/opt/homebrew/opt/ffmpeg/lib/pkgconfig:$ENV{PKG_CONFIG_PATH}")
        endif()
        pkg_check_modules(FFMPEG QUIET IMPORTED_TARGET libavcodec libavutil)
    endif()
endif()

if(STRIKERS_FFMPEG AND FFMPEG_FOUND)
    target_compile_definitions(strikers_platform PRIVATE STRIKERS_FFMPEG=1)
    target_include_directories(strikers_platform PRIVATE ${FFMPEG_INCLUDE_DIRS})
    target_link_libraries(strikers PRIVATE PkgConfig::FFMPEG)
    message(STATUS "THP movies: libavcodec ${FFMPEG_libavcodec_VERSION} (pkg-config)")
elseif(STRIKERS_FFMPEG)
    # Two passes: the named roots first, taken literally (NO_DEFAULT_PATH, and
    # NO_CMAKE_FIND_ROOT_PATH so a cross build does not search inside the Windows SDK), then an
    # ordinary search that cannot pick up the host's own libavcodec.
    set(_ff_roots "")
    if(STRIKERS_FFMPEG_ROOT)
        list(APPEND _ff_roots "${STRIKERS_FFMPEG_ROOT}")
    endif()
    if(DEFINED ENV{STRIKERS_FFMPEG_ROOT})
        list(APPEND _ff_roots "$ENV{STRIKERS_FFMPEG_ROOT}")
    endif()
    list(APPEND _ff_roots "${_ff_static}")
    list(APPEND _ff_roots "${CMAKE_CURRENT_SOURCE_DIR}/extern/ffmpeg-windows-x86_64")
    if(APPLE)
        list(APPEND _ff_roots "/opt/homebrew/opt/ffmpeg")
    endif()

    find_path(STRIKERS_FFMPEG_INCLUDE_DIR
        NAMES libavcodec/avcodec.h
        PATHS ${_ff_roots} PATH_SUFFIXES include
        NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH)
    find_library(STRIKERS_FFMPEG_AVCODEC
        NAMES avcodec
        PATHS ${_ff_roots} PATH_SUFFIXES lib
        NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH)
    find_library(STRIKERS_FFMPEG_AVUTIL
        NAMES avutil
        PATHS ${_ff_roots} PATH_SUFFIXES lib
        NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH)

    # A no-op when the pass above answered: find_path and find_library return immediately on a cache
    # entry that is set.
    find_path(STRIKERS_FFMPEG_INCLUDE_DIR NAMES libavcodec/avcodec.h)
    find_library(STRIKERS_FFMPEG_AVCODEC NAMES avcodec)
    find_library(STRIKERS_FFMPEG_AVUTIL NAMES avutil)

    if(STRIKERS_FFMPEG_INCLUDE_DIR AND STRIKERS_FFMPEG_AVCODEC AND STRIKERS_FFMPEG_AVUTIL)
        # The version out of the header, not the file name: a rolling asset is called "latest" and
        # says nothing.
        set(_ff_version "unknown")
        if(EXISTS "${STRIKERS_FFMPEG_INCLUDE_DIR}/libavcodec/version_major.h")
            file(STRINGS "${STRIKERS_FFMPEG_INCLUDE_DIR}/libavcodec/version_major.h"
                 _ff_major REGEX "LIBAVCODEC_VERSION_MAJOR +[0-9]+")
        endif()
        if(EXISTS "${STRIKERS_FFMPEG_INCLUDE_DIR}/libavcodec/version.h")
            file(STRINGS "${STRIKERS_FFMPEG_INCLUDE_DIR}/libavcodec/version.h"
                 _ff_minor REGEX "LIBAVCODEC_VERSION_MINOR +[0-9]+")
        endif()
        string(REGEX MATCH "[0-9]+" _ff_major "${_ff_major}")
        string(REGEX MATCH "[0-9]+" _ff_minor "${_ff_minor}")
        if(_ff_major)
            set(_ff_version "${_ff_major}.${_ff_minor}")
        endif()

        target_compile_definitions(strikers_platform PRIVATE STRIKERS_FFMPEG=1)
        target_include_directories(strikers_platform PRIVATE "${STRIKERS_FFMPEG_INCLUDE_DIR}")
        target_link_libraries(strikers PRIVATE
            "${STRIKERS_FFMPEG_AVCODEC}" "${STRIKERS_FFMPEG_AVUTIL}")
        string(FIND "${STRIKERS_FFMPEG_AVCODEC}" "${_ff_static}/" _ff_static_at)
        if(STRIKERS_FFMPEG_AVCODEC MATCHES "\\.a$" OR _ff_static_at EQUAL 0)
            set(_ff_is_static TRUE)
        else()
            set(_ff_is_static FALSE)
        endif()
        if(_ff_is_static AND WIN32)
            target_link_libraries(strikers PRIVATE bcrypt)
        elseif(_ff_is_static)
            # A static libavcodec leaves its libm and thread references for the executable to resolve.
            find_package(Threads REQUIRED)
            target_link_libraries(strikers PRIVATE m Threads::Threads)
        endif()
        message(STATUS "THP movies: libavcodec ${_ff_version} (${STRIKERS_FFMPEG_AVCODEC})")

        if(WIN32 AND NOT _ff_is_static)
            # An import library is not a runtime: linking avcodec.lib without avcodec-NN.dll beside
            # it does not start. Copied at build time, and package.sh takes the same files. These
            # three, from avcodec's import table.
            get_filename_component(_ff_libdir "${STRIKERS_FFMPEG_AVCODEC}" DIRECTORY)
            get_filename_component(_ff_bindir "${_ff_libdir}/../bin" ABSOLUTE)
            file(GLOB _ff_dlls
                "${_ff_bindir}/avcodec-*.dll"
                "${_ff_bindir}/avutil-*.dll"
                "${_ff_bindir}/swresample-*.dll")
            if(_ff_dlls)
                add_custom_command(TARGET strikers POST_BUILD
                    COMMAND ${CMAKE_COMMAND} -E copy_if_different
                            ${_ff_dlls} "$<TARGET_FILE_DIR:strikers>"
                    COMMENT "ffmpeg runtime DLLs -> $<TARGET_FILE_DIR:strikers>"
                    VERBATIM)
            else()
                message(WARNING "THP movies: linking ${STRIKERS_FFMPEG_AVCODEC} but found no "
                                "avcodec/avutil/swresample DLLs in ${_ff_bindir}; the build "
                                "will not start until they are beside strikers.exe.")
            endif()
        endif()
    else()
        # A warning, so a build that drops the movies is seen before a player finds it.
        message(WARNING "THP movies: STRIKERS_FFMPEG is ON but no libavcodec/libavutil was "
                        "found, by pkg-config or otherwise; the game will build without "
                        "movies, and without their audio. Install ffmpeg's development "
                        "files (brew install ffmpeg, apt install libavcodec-dev "
                        "libavutil-dev), or run tools/build-ffmpeg.sh or "
                        "tools/fetch-ffmpeg-windows.sh, or point "
                        "-DSTRIKERS_FFMPEG_ROOT at an unpacked one, or pass "
                        "-DSTRIKERS_FFMPEG=OFF to say this is intended.")
    endif()
endif()

# zlib, for .gcz disc images (src/platform/disc.c).
if(NOT TARGET ZLIB::ZLIB)
    find_package(ZLIB QUIET)
endif()
if(TARGET ZLIB::ZLIB)
    target_compile_definitions(strikers_platform PRIVATE STRIKERS_ZLIB=1)
    target_link_libraries(strikers_platform PRIVATE ZLIB::ZLIB)
    target_link_libraries(strikers PRIVATE ZLIB::ZLIB)
    message(STATUS "Disc images: .iso/.gcm, .ciso and .gcz (zlib)")
else()
    message(STATUS "Disc images: .iso/.gcm and .ciso; no zlib, so no .gcz")
endif()

# strikers-quest: zstd (decompression only, extern/zstd, copied from Dolphin's Externals) for .rvz
# disc images, Dolphin's own compressed format (src/platform/disc.c). Built without the port prelude.
file(GLOB PORT_ZSTD_SOURCES CONFIGURE_DEPENDS
     "${CMAKE_CURRENT_SOURCE_DIR}/extern/zstd/common/*.c"
     "${CMAKE_CURRENT_SOURCE_DIR}/extern/zstd/decompress/*.c")
add_library(port_zstd STATIC ${PORT_ZSTD_SOURCES})
target_include_directories(port_zstd PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/extern/zstd")
target_compile_definitions(port_zstd PRIVATE ZSTD_DISABLE_ASM=1 ZSTD_LEGACY_SUPPORT=0)
set_target_properties(port_zstd PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_compile_definitions(strikers_platform PRIVATE STRIKERS_ZSTD=1)
target_link_libraries(strikers_platform PRIVATE port_zstd)
target_link_libraries(strikers PRIVATE port_zstd)
message(STATUS "Disc images: also .rvz (zstd)")

# strikers-quest r41: the other .rvz/.wia compressions, so any image Dolphin makes reads as it is.
# LZMA and LZMA2 through the LZMA SDK's decoders (extern/lzma, public domain, as vendored by mGBA),
# bzip2 through libbzip2 1.0.8 (extern/bzip2, Dolphin's Externals, its own BSD-style LICENSE).
add_library(port_lzma STATIC
    "${CMAKE_CURRENT_SOURCE_DIR}/extern/lzma/LzmaDec.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/extern/lzma/Lzma2Dec.c")
target_include_directories(port_lzma PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/extern/lzma")
set_target_properties(port_lzma PROPERTIES POSITION_INDEPENDENT_CODE ON)
add_library(port_bzip2 STATIC
    "${CMAKE_CURRENT_SOURCE_DIR}/extern/bzip2/blocksort.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/extern/bzip2/bzlib.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/extern/bzip2/compress.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/extern/bzip2/crctable.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/extern/bzip2/decompress.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/extern/bzip2/huffman.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/extern/bzip2/randtable.c")
target_include_directories(port_bzip2 PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/extern/bzip2")
set_target_properties(port_bzip2 PROPERTIES POSITION_INDEPENDENT_CODE ON)
# The calls disc.c makes into both, kept out of its translation unit (the LZMA SDK's types).
add_library(port_disc_codecs STATIC "${CMAKE_CURRENT_SOURCE_DIR}/src/platform/disc_codecs.c")
target_link_libraries(port_disc_codecs PUBLIC port_lzma port_bzip2)
set_target_properties(port_disc_codecs PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_compile_definitions(strikers_platform PRIVATE STRIKERS_LZMA_BZIP2=1)
target_link_libraries(strikers_platform PRIVATE port_disc_codecs)
target_link_libraries(strikers PRIVATE port_disc_codecs)
message(STATUS "Disc images: also .rvz/.wia with LZMA, LZMA2, bzip2 or purge")

# Dead-strip the debug classes (StatsGatherer, cFuzzyDebugger) whose vtables reference methods
# defined only in files the decomp excludes, and which nothing ever constructs. On visionOS the app's
# link does it (project.yml: DEAD_CODE_STRIPPING, and the same -unexported_symbol list).
if(STRIKERS_VISIONOS)
elseif(APPLE)
    target_link_options(strikers PRIVATE -dead_strip)
elseif(MSVC)
    # clang-cl sets CMake's MSVC, and link.exe/lld-link take neither spelling. /OPT:REF is the
    # equivalent; no compile-side flag is needed because the MSVC-style frontend already emits one
    # COMDAT per function.
    target_link_options(strikers PRIVATE /OPT:REF /OPT:ICF)
else()
    target_compile_options(port_flags INTERFACE -ffunction-sections -fdata-sections)
    target_link_options(strikers PRIVATE -Wl,--gc-sections)
endif()

# operator new/delete are weak, coalesced symbols, so dyld merges them process-wide and
# SDL3/Dawn/AppKit would otherwise bind to the game's.
if(STRIKERS_VISIONOS)
elseif(APPLE)
    foreach(_sym __Znwm __Znam __ZdlPv __ZdaPv)
        target_link_options(strikers PRIVATE "-Wl,-unexported_symbol,${_sym}")
    endforeach()
elseif(NOT MSVC)
    # An ELF executable exports a definition a shared library needs, so libc++ would bind to the
    # game's operators exactly as dyld does.
    set(_local_ops "${CMAKE_CURRENT_BINARY_DIR}/local-operators.map")
    file(WRITE "${_local_ops}" "{ local: _Znw*; _Zna*; _ZdlPv*; _ZdaPv*; };\n")
    target_link_options(strikers PRIVATE "-Wl,--version-script=${_local_ops}")
    # A bundled libstdc++ breaks Mesa's LLVM, so it is linked in; glibc 2.41 refuses executable stacks.
    # Not on Android: the NDK ships libc++ only, and which of it is linked is ANDROID_STL's to say.
    if(ANDROID)
        target_link_options(strikers PRIVATE -Wl,-z,noexecstack)
    else()
        target_link_options(strikers PRIVATE -static-libstdc++ -static-libgcc -Wl,-z,noexecstack)
    endif()
endif()

if(STRIKERS_AURORA)
    # Deliberately NOT aurora::os, Aurora's OS has no threads, and ours is already complete.
    target_link_libraries(strikers PRIVATE
        aurora::core aurora::gx aurora::gd aurora::vi aurora::pad aurora::si
        aurora::card)
    # aurora::main is the process entry point (SDL_main -> aurora_main); on visionOS the app has that.
    if(NOT STRIKERS_VISIONOS)
        target_link_libraries(strikers PRIVATE aurora::main)
    endif()

    # Frame capture lives inside Aurora now (lib/aurora.cpp, namespace capture).

    # strikers-quest: the Quest OpenXR layer (include/port/xr_quest.h). Elsewhere the same file
    # compiles to stubs, so main.cpp links everywhere.
    # StrikersVision: on visionOS the PortXr* layer is the app's (visionos/xr, Compositor Services),
    # so neither the OpenXR layer nor its Vulkan effects are built; xr_net.cpp is, over the app's
    # transports (sv_net_* in visionos/include/sv_bridge.h).
    if(STRIKERS_VISIONOS)
        add_library(strikers_xr OBJECT "${CMAKE_CURRENT_SOURCE_DIR}/src/platform/xr_net.cpp")
    else()
        add_library(strikers_xr OBJECT "${CMAKE_CURRENT_SOURCE_DIR}/src/platform/xr_quest.cpp"
                                       "${CMAKE_CURRENT_SOURCE_DIR}/src/platform/xr_fx.cpp"
                                       "${CMAKE_CURRENT_SOURCE_DIR}/src/platform/xr_net.cpp")
    endif()
    set_target_properties(strikers_xr PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)
    # Aurora's full usage requirements (includes incl. Dawn's, and TARGET_PC and the rest) and
    # nothing of the port's: include/ here is NOT on the path, because include/dolphin/pad.h is the
    # decomp's, and the PADStatus this file hands to PADSetVirtualStatus must be the one Aurora's PAD
    # was built with. xr_quest.cpp reaches its own header by relative path.
    # dawn::webgpu_dawn for webgpu_cpp.h: aurora::core links Dawn privately, so its headers do not
    # come along. Usage requirements only; the library itself reaches libmain.so through Aurora.
    target_link_libraries(strikers_xr PRIVATE aurora::core aurora::pad dawn::webgpu_dawn)
    target_sources(strikers PRIVATE $<TARGET_OBJECTS:strikers_xr>)

    if(ANDROID)
        option(STRIKERS_XR "Build the Quest OpenXR layer" ON)
    endif()
    if(ANDROID AND STRIKERS_XR)
        # The Khronos loader and headers, unpacked from org.khronos.openxr:openxr_loader_for_android
        # into include/ and lib/<abi>/. STRIKERS_OPENXR_ROOT in the environment wins.
        if(DEFINED ENV{STRIKERS_OPENXR_ROOT})
            set(_xr_root "$ENV{STRIKERS_OPENXR_ROOT}")
        else()
            get_filename_component(_xr_root "${CMAKE_CURRENT_SOURCE_DIR}/../../tools/openxr" ABSOLUTE)
        endif()
        if(NOT EXISTS "${_xr_root}/include/openxr/openxr.h" OR
           NOT EXISTS "${_xr_root}/lib/${ANDROID_ABI}/libopenxr_loader.so")
            message(FATAL_ERROR "STRIKERS_XR: no OpenXR headers/loader under ${_xr_root}")
        endif()
        add_library(strikers_openxr_loader SHARED IMPORTED)
        set_target_properties(strikers_openxr_loader PROPERTIES
            IMPORTED_LOCATION "${_xr_root}/lib/${ANDROID_ABI}/libopenxr_loader.so")
        target_include_directories(strikers_xr PRIVATE "${_xr_root}/include")
        target_compile_definitions(strikers_xr PRIVATE STRIKERS_XR=1 PORT_XR_HEADSET=1)
        target_compile_definitions(port_flags INTERFACE PORT_XR_HEADSET=1)
        # The system Vulkan loader, linked directly: the XR device's calls must not go through
        # vulkan_shim.c, which is why the Quest build does not ship the shim at all.
        target_link_libraries(strikers PRIVATE strikers_openxr_loader vulkan android log)
        set(STRIKERS_OPENXR_LOADER "${_xr_root}/lib/${ANDROID_ABI}/libopenxr_loader.so" CACHE INTERNAL "")
        message(STATUS "strikers: Quest OpenXR layer ON (${_xr_root})")
    endif()
endif()

# Windows: what a runnable build needs that a linkable one does not.
if(MSVC)
    # 1. dbghelp, for src/platform/crashlog.c.
    target_link_libraries(strikers PRIVATE dbghelp)

    # 2. Debug info, in Release as well. /Z7 puts it in the objects and /DEBUG makes lld-link emit
    # the PDB.
    target_compile_options(port_flags INTERFACE /Z7)
    target_link_options(strikers PRIVATE /DEBUG)
endif()

if(MSVC AND STRIKERS_AURORA)
    # 3. The prebuilt DLLs, beside the executable.
    include("${CMAKE_CURRENT_SOURCE_DIR}/extern/aurora/cmake/AuroraCopyRuntimeDLLs.cmake")
    aurora_copy_runtime_dlls(strikers)
endif()

# CMAKE_MSVC_RUNTIME_LIBRARY is MultiThreaded at the top level, but Abseil replaces it in its own
# directory with MultiThreadedDebug for Debug builds.
if(MSVC AND STRIKERS_AURORA)
    function(port_force_windows_runtime directory)
        get_property(_targets DIRECTORY "${directory}" PROPERTY BUILDSYSTEM_TARGETS)
        foreach(_target IN LISTS _targets)
            get_target_property(_imported "${_target}" IMPORTED)
            get_target_property(_type "${_target}" TYPE)
            if(NOT _imported AND _type MATCHES
                    "^(EXECUTABLE|STATIC_LIBRARY|SHARED_LIBRARY|MODULE_LIBRARY|OBJECT_LIBRARY)$")
                set_property(TARGET "${_target}" PROPERTY
                    MSVC_RUNTIME_LIBRARY MultiThreaded)
            endif()
        endforeach()

        get_property(_subdirectories DIRECTORY "${directory}" PROPERTY SUBDIRECTORIES)
        foreach(_subdirectory IN LISTS _subdirectories)
            port_force_windows_runtime("${_subdirectory}")
        endforeach()
    endfunction()
    port_force_windows_runtime("${CMAKE_CURRENT_SOURCE_DIR}")
endif()

# Tests, reachable with `ctest` from the build directory. Aurora registers its own under
# extern/aurora/tests, and without enable_testing() here the root CTest file listed nothing.
if(NOT CMAKE_CROSSCOMPILING)
    enable_testing()
    add_subdirectory(tests)
endif()

# StrikersVision: libstrikers.a and the headers the app's native code needs, in STRIKERS_XROS_OUT
# (cmake --build <dir> --target strikers_xros).
if(STRIKERS_VISIONOS)
    set(STRIKERS_XROS_OUT "${CMAKE_BINARY_DIR}/xros" CACHE PATH "Where libstrikers.a and its headers go")
    include(cmake/VisionOSBundle.cmake)
    strikers_visionos_bundle(strikers "${STRIKERS_XROS_OUT}")
endif()

list(LENGTH GAME_SOURCES _n_game)
list(LENGTH ODE_SOURCES  _n_ode)
message(STATUS "in scope: ${_n_game} game/engine + ${_n_ode} ODE translation units")
