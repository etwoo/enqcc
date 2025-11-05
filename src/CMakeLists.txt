add_subdirectory(sys)

target_sources(compiler PRIVATE
	lex.c
	result.c
)

option(WITH_DEBUG_LOG "Enable runtime debug logging" OFF)
if (WITH_DEBUG_LOG)
	target_compile_definitions(compiler PRIVATE WITH_DEBUG_LOG)
endif (WITH_DEBUG_LOG)
