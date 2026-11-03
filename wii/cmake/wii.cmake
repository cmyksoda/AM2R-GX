# Cross-compile Butterscotch for Nintendo Wii (devkitPPC + libogc 3).
set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_VERSION 1)
set(CMAKE_SYSTEM_PROCESSOR powerpc)
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

if(NOT DEFINED ENV{DEVKITPRO})
    message(FATAL_ERROR "DEVKITPRO is not set")
endif()
if(NOT DEFINED ENV{DEVKITPPC})
    message(FATAL_ERROR "DEVKITPPC is not set")
endif()

set(DEVKITPRO "$ENV{DEVKITPRO}")
set(DEVKITPPC "$ENV{DEVKITPPC}")

set(CMAKE_C_COMPILER   "${DEVKITPPC}/bin/powerpc-eabi-gcc")
set(CMAKE_CXX_COMPILER "${DEVKITPPC}/bin/powerpc-eabi-g++")
set(CMAKE_AR           "${DEVKITPPC}/bin/powerpc-eabi-ar")
set(CMAKE_RANLIB       "${DEVKITPPC}/bin/powerpc-eabi-ranlib")
set(CMAKE_STRIP        "${DEVKITPPC}/bin/powerpc-eabi-strip")

set(WII_MACHDEP "-DGEKKO -mrvl -mcpu=750 -meabi -mhard-float")
set(WII_INCLUDES "-I${DEVKITPRO}/libogc/include -I${DEVKITPRO}/portlibs/wii/include -I${DEVKITPRO}/portlibs/ppc/include")
set(WII_LDFLAGS "-mrvl -mcpu=750 -meabi -mhard-float -Wl,--gc-sections -L${DEVKITPRO}/libogc/lib/wii -L${DEVKITPRO}/portlibs/wii/lib -L${DEVKITPRO}/portlibs/ppc/lib")

set(CMAKE_C_FLAGS_INIT "${WII_MACHDEP} ${WII_INCLUDES} -ffunction-sections -fdata-sections")
set(CMAKE_CXX_FLAGS_INIT "${CMAKE_C_FLAGS_INIT}")
set(CMAKE_EXE_LINKER_FLAGS_INIT "${WII_LDFLAGS}")
set(CMAKE_EXECUTABLE_SUFFIX ".elf")

set(CMAKE_FIND_ROOT_PATH "${DEVKITPPC}" "${DEVKITPRO}/libogc" "${DEVKITPRO}/portlibs/wii" "${DEVKITPRO}/portlibs/ppc")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)

set(WII TRUE)
set(PLATFORM_WII TRUE)
