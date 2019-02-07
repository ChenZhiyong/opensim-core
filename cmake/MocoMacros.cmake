# These CMake functions serve to reduce duplication across CMakeLists.txt files.

include(CMakeParseArguments)

# Copy DLL files from a dependency's installation into the
# build directory. This is a Windows-specific function enabled 
# only on Windows. The intention is to allow the runtime loader to find all 
# the required DLLs without editing the PATH environment variable.
# Arguments:
#   DEP_NAME: Name of the dependency (used to name a custom target)
#   DEP_BIN_DIR: The directory in the dependency containing DLLs to copy.
#   INSTALL_DLLS (optional): If provided, then the dependency's DLLs are
#     installed into ${INSTALL_DLLS}.
# This is based on a similar function in OpenSimMacros.cmake.
function(MocoCopyDLLs)
    # On Windows, copy dlls into the Tropter binary directory.
    set(options)
    set(oneValueArgs DEP_NAME DEP_BIN_DIR INSTALL_DLLS)
    set(multiValueArgs)
    cmake_parse_arguments(MOCOCOPY
            "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})
    if(WIN32)
        file(GLOB DLLS ${MOCOCOPY_DEP_BIN_DIR}/*.dll)
        if(NOT DLLS)
            message(FATAL_ERROR "Zero DLLs found in directory "
                                "${MOCOCOPY_DEP_BIN_DIR}.")
        endif()
        set(DEST_DIR "${CMAKE_BINARY_DIR}/${CMAKE_CFG_INTDIR}")
        foreach(DLL IN LISTS DLLS)
            get_filename_component(DLL_NAME ${DLL} NAME)
            list(APPEND DLLS_DEST "${DEST_DIR}/${DLL_NAME}")
            add_custom_command(OUTPUT "${DEST_DIR}/${DLL_NAME}"
                COMMAND ${CMAKE_COMMAND} -E make_directory ${DEST_DIR}
                COMMAND ${CMAKE_COMMAND} -E copy ${DLL} ${DEST_DIR}
                DEPENDS ${DLL}
                COMMENT "Copying ${DLL_NAME} from ${MOCOCOPY_DEP_BIN_DIR} to ${DEST_DIR}.")
        endforeach()
        add_custom_target(Copy_${MOCOCOPY_DEP_NAME}_DLLs ALL DEPENDS ${DLLS_DEST})
        set_target_properties(Copy_${MOCOCOPY_DEP_NAME}_DLLs PROPERTIES
            PROJECT_LABEL "Copy ${MOCOCOPY_DEP_NAME} DLLs" FOLDER "Moco")
        if(MOCOCOPY_INSTALL_DLLS)
            install(FILES ${DLLS} DESTINATION ${MOCOCOPY_INSTALL_DLLS})
        endif()
    endif()
endfunction()

# Add a target to the Moco project for building an example with the given
# NAME. The example must be within a source file named ${NAME}.cpp.
# This function also installs the example file with a CMakeLists that can find
# the Moco installation and build the example.
#
# This function can only be used from the source distribution of Moco
# (e.g., not via the UseMoco.cmake file in a binary distribution).
function(MocoAddExampleCXX)
    set(options)
    set(oneValueArgs NAME)
    set(multiValueArgs RESOURCES)
    cmake_parse_arguments(MOCOEX
            "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})

    add_executable(${MOCOEX_NAME} ${MOCOEX_NAME}.cpp)
    set_target_properties(${MOCOEX_NAME} PROPERTIES
            FOLDER "Moco/Examples")
    target_link_libraries(${MOCOEX_NAME} osimMoco)
    file(COPY ${MOCOEX_RESOURCES} DESTINATION "${CMAKE_CURRENT_BINARY_DIR}")

    install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
            DESTINATION ${MOCO_INSTALL_EXAMPLEDIR}/C++
            PATTERN "CMakeLists.txt" EXCLUDE)

    set(_example_install_dir ${MOCO_INSTALL_EXAMPLEDIR}/C++/${MOCOEX_NAME})
    # These next two variables are to be configured below (they are not used
    # here, but within CMakeListsToInstall.txt.in).
    set(_example_name ${MOCOEX_NAME})
    file(RELATIVE_PATH _moco_install_hint
            "${CMAKE_INSTALL_PREFIX}/${_example_install_dir}"
            "${CMAKE_INSTALL_PREFIX}")
    configure_file(
            "${CMAKE_SOURCE_DIR}/Moco/Examples/C++/CMakeListsToInstall.txt.in"
            "${CMAKE_CURRENT_BINARY_DIR}/CMakeListsToInstall.txt"
            @ONLY)

    install(FILES
            "${CMAKE_CURRENT_BINARY_DIR}/CMakeListsToInstall.txt"
            DESTINATION ${_example_install_dir}
            RENAME CMakeLists.txt)
endfunction()
