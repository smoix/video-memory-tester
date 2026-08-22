# CMake toolchain file for DJGPP cross-compilation to DOS
set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR i586)

# Set the DJGPP installation prefix
set(DJGPP_PREFIX "$ENV{HOME}/djgpp/djgpp")

# Specify the cross compilers
set(CMAKE_C_COMPILER "${DJGPP_PREFIX}/bin/i586-pc-msdosdjgpp-gcc")
set(CMAKE_CXX_COMPILER "${DJGPP_PREFIX}/bin/i586-pc-msdosdjgpp-g++")
set(CMAKE_AR "${DJGPP_PREFIX}/bin/i586-pc-msdosdjgpp-ar")
set(CMAKE_RANLIB "${DJGPP_PREFIX}/bin/i586-pc-msdosdjgpp-ranlib")

# Optimization flags for i586 DOS target - using -O2 for better stability
set(CMAKE_C_FLAGS_INIT "-O2 -march=i586 -mtune=i586")
set(CMAKE_CXX_FLAGS_INIT "-O2 -march=i586 -mtune=i586")
set(CMAKE_EXE_LINKER_FLAGS_INIT "-s")

# Search for programs in the build host directories
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
# For libraries and headers in the target directories
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
