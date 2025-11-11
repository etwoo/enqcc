add_subdirectory(passes)
add_subdirectory(sys)

target_sources(compiler PRIVATE
	result.c
)
target_include_directories(compiler SYSTEM PUBLIC ${arena_SOURCE_DIR})

option(WITH_DEBUG_LOG "Enable runtime debug logging" OFF)
if (WITH_DEBUG_LOG)
	target_compile_definitions(compiler PRIVATE WITH_DEBUG_LOG)
endif (WITH_DEBUG_LOG)
