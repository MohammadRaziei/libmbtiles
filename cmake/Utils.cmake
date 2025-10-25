# library_name.cmake - CMake function to generate library filenames based on OS
#
# This CMake function generates the appropriate library filename based on:
# - Library name
# - Whether it's shared or static
# - Operating system
#
# Usage:
#   library_name(libname library_type [OUTPUT_VARIABLE])
#
# Examples:
#   library_name(curl SHARED)           # Returns: libcurl.so (on Linux)
#   library_name(curl STATIC)           # Returns: libcurl.a (on Linux)
#   library_name(zlib SHARED output_var) # Sets output_var to libz.so (on Linux)
#
# Supported OS: Linux, Windows, macOS

function(library_name LIB_NAME LIB_TYPE)
    # Parse arguments
    set(OUTPUT_VAR "${LIB_NAME}_LIBRARY_NAME")
    if(ARGC GREATER 2)
        set(OUTPUT_VAR "${ARGV2}")
    endif()

    # Convert library type to boolean
    if("${LIB_TYPE}" STREQUAL "SHARED")
        set(IS_SHARED TRUE)
    elseif("${LIB_TYPE}" STREQUAL "STATIC")
        set(IS_SHARED FALSE)
    else()
        message(FATAL_ERROR "Invalid library type: ${LIB_TYPE}. Use SHARED or STATIC.")
    endif()

    # Determine library prefix and suffix based on OS
    if(WIN32)
        # Windows naming convention
        if(IS_SHARED)
            set(LIB_PREFIX "")
            set(LIB_SUFFIX ".dll")
        else()
            set(LIB_PREFIX "")
            set(LIB_SUFFIX ".lib")
        endif()
    elseif(APPLE)
        # macOS naming convention
        if(IS_SHARED)
            set(LIB_PREFIX "lib")
            set(LIB_SUFFIX ".dylib")
        else()
            set(LIB_PREFIX "lib")
            set(LIB_SUFFIX ".a")
        endif()
    else()
        # Linux/Unix naming convention
        if(IS_SHARED)
            set(LIB_PREFIX "lib")
            set(LIB_SUFFIX ".so")
        else()
            set(LIB_PREFIX "lib")
            set(LIB_SUFFIX ".a")
        endif()
    endif()

    # Construct the full library name
    set(FULL_LIB_NAME "${LIB_PREFIX}${LIB_NAME}${LIB_SUFFIX}")

    # Set the output variable
    set(${OUTPUT_VAR} "${FULL_LIB_NAME}" PARENT_SCOPE)

    # Print debug info (optional)
    # message(STATUS "Library name for ${LIB_NAME} (${LIB_TYPE}): ${FULL_LIB_NAME}")
endfunction()
