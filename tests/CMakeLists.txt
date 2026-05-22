macro(add_chapter n)
	add_test(NAME nlsandler-book-ch${n} COMMAND
		${CMAKE_CURRENT_SOURCE_DIR}/nlsandler-book/test_compiler
		${PROJECT_SOURCE_DIR}/scripts/driver.sh
		--expected-error-codes 70
		--latest-only
		--chapter ${n}
		--extra-credit
	)
endmacro()

foreach (chapter RANGE 1 17)
	add_chapter(${chapter})
endforeach ()
