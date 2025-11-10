# ---- Helpers ----

function(make_identifier_from_filename INPUT OUT_VAR)
    get_filename_component(_FNAME "${INPUT}" NAME)
    string(REPLACE "." ";" _PARTS "${_FNAME}")
    string(REPLACE "-" "_" _PARTS "${_PARTS}")
    string(JOIN "_" RESULT ${_PARTS})
    set(${OUT_VAR} "${RESULT}" PARENT_SCOPE)
endfunction()

function(make_include_guard INPUT OUT_VAR)
    get_filename_component(_BASE "${INPUT}" NAME)
    string(REPLACE "." "_" GUARD "${_BASE}")
    string(REPLACE "-" "_" GUARD "${GUARD}")
    string(TOUPPER "LIBMBTILES_TEMPLATE_${GUARD}_H" GUARD)
    set(${OUT_VAR} "${GUARD}" PARENT_SCOPE)
endfunction()

# Build namespace from relative dir under templates/
#   ""              -> "templates"
#   "assets"        -> "templates::assets"
#   "foo/bar"       -> "templates::foo::bar"
function(make_namespace REL_DIR OUT_VAR)
    set(_NS "templates")
    if(REL_DIR)
        string(REPLACE "-" "_" REL_DIR "${REL_DIR}")
        string(REPLACE "/" "::" REL_DIR "${REL_DIR}")
        set(_NS "templates::${REL_DIR}")
    endif()
    set(${OUT_VAR} "${_NS}" PARENT_SCOPE)
endfunction()

# ---- Unified embedding ----
# Usage:
#   embed_file(INPUT OUTPUT NAMESPACE)
# Example:
#   embed_file("${TPL}" "${OUT_HDR}" "${NAMESPACE}")              # templates
#   embed_file("${PF}"  "${OUT_HDR}" "project")                   # project files
function(embed_file INPUT OUTPUT NAMESPACE)
    set(TEMPLATES_HEADER_IN ${PROJECT_SOURCE_DIR}/cmake/utils/templates.in.h)

    file(READ "${INPUT}" CONTENT)
    get_filename_component(INPUT_BASENAME "${INPUT}" NAME)

    make_identifier_from_filename("${INPUT}" VAR)
    make_include_guard("${INPUT}" INCLUDE_GUARD)
    # NAMESPACE is passed in by caller

    get_filename_component(_OUTDIR "${OUTPUT}" DIRECTORY)
    file(MAKE_DIRECTORY "${_OUTDIR}")

    configure_file("${TEMPLATES_HEADER_IN}" "${OUTPUT}" @ONLY)
endfunction()

# ---- Main (templates) ----
set(TEMPLATES_DIR ${PROJECT_SOURCE_DIR}/src/templates)
set(GENERATED_TEMPLATE_HEADERS_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
set(GENERATED_TEMPLATE_HEADERS)

set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${TEMPLATES_DIR}/*")
file(GLOB_RECURSE TEMPLATE_FILES CONFIGURE_DEPENDS "${TEMPLATES_DIR}/*")

foreach(TPL IN LISTS TEMPLATE_FILES)
    file(RELATIVE_PATH REL_PATH "${TEMPLATES_DIR}" "${TPL}")
    get_filename_component(REL_DIR "${REL_PATH}" DIRECTORY)
    string(REPLACE "-" "_" REL_DIR "${REL_DIR}")

    make_identifier_from_filename("${TPL}" VAR_NAME)
    make_namespace("${REL_DIR}" NAMESPACE)

    if(REL_DIR STREQUAL "")
        set(OUT_HDR "${GENERATED_TEMPLATE_HEADERS_DIR}/templates/${VAR_NAME}.h")
    else()
        set(OUT_HDR "${GENERATED_TEMPLATE_HEADERS_DIR}/templates/${REL_DIR}/${VAR_NAME}.h")
    endif()

    embed_file("${TPL}" "${OUT_HDR}" "${NAMESPACE}")
    list(APPEND GENERATED_TEMPLATE_HEADERS "${OUT_HDR}")
endforeach()
