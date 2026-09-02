find_program(SLANGC slangc HINTS $ENV{VULKAN_SDK}/bin $ENV{VULKAN_SDK}/Bin)

if(NOT SLANGC)
  message(WARNING "No Slang compiler found - Slang shader validation will be skipped")
endif()

function(validate_shaders target_name shader_dir)
  file(GLOB_RECURSE SLANG_SHADER_FILES
    "${shader_dir}/*.slang"
  )
  
  set(SLANG_VALIDATION_STAMPS "")
  
  if(SLANGC)
    foreach(SHADER ${SLANG_SHADER_FILES})
      get_filename_component(FILE_NAME ${SHADER} NAME)
      set(VALIDATION_STAMP "${CMAKE_CURRENT_BINARY_DIR}/shaders/${FILE_NAME}.validated")
      add_custom_command(
        OUTPUT ${VALIDATION_STAMP}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/shaders
        COMMAND ${SLANGC} -no-codegen -target spirv ${SHADER}
        COMMAND ${CMAKE_COMMAND} -E touch ${VALIDATION_STAMP}
        DEPENDS ${SHADER}
        COMMENT "Validating Slang shader ${FILE_NAME}"
      )
      list(APPEND SLANG_VALIDATION_STAMPS ${VALIDATION_STAMP})
    endforeach()
  endif()
  
  add_custom_target(${target_name} ALL DEPENDS ${SLANG_VALIDATION_STAMPS})
endfunction()