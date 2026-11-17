##
## SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
## Copyright (C) 2020 - 2025 by the ryujin authors
##

find_program(NUMDIFF_EXECUTABLE
  NAMES numdiff
  HINTS ${NUMDIFF_DIR}
  PATH_SUFFIXES bin
  )
mark_as_advanced(NUMDIFF_EXECUTABLE)

if("${NUMDIFF_EXECUTABLE}" MATCHES "NUMDIFF_EXECUTABLE-NOTFOUND")
  message(STATUS "Could not find numdiff executable. Disabling tests.")

else()

  # Some tests instantiate templates of the library from source/:
  include_directories(${PROJECT_SOURCE_DIR}/source)

  #
  # The equations a test directory needs beyond its own name: directories
  # named after an equation need that equation (EQUATIONS), the others
  # need the equations listed here.
  #
  set(_test_equations_concept ${RYUJIN_ALL_EQUATIONS})
  set(_test_equations_driver euler)
  set(_test_equations_install euler)
  set(_test_equations_scalar_convection_diffusion
    scalar_conservation scalar_diffusion
    )

  file(GLOB _files RELATIVE ${CMAKE_CURRENT_SOURCE_DIR} CONFIGURE_DEPENDS */CMakeLists.txt)
  foreach(_file ${_files})
    get_filename_component(_directory "${_file}" DIRECTORY)

    set(_needed ${_test_equations_${_directory}})
    if(_directory IN_LIST RYUJIN_ALL_EQUATIONS)
      list(APPEND _needed ${_directory})
    endif()

    set(_enabled TRUE)
    foreach(_equation ${_needed})
      if(NOT _equation IN_LIST EQUATIONS)
        set(_enabled FALSE)
      endif()
    endforeach()

    if(_enabled)
      add_subdirectory("${_directory}")
    else()
      message(STATUS "Skipping tests/${_directory}: needs the equations ${_needed}")
    endif()
  endforeach()
endif()

configure_file(${CMAKE_CURRENT_SOURCE_DIR}/scripts/print_mass ${CMAKE_CURRENT_BINARY_DIR} COPYONLY)
