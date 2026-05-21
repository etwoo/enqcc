add_test(NAME nlsandler-book-e2e COMMAND
	${CMAKE_CURRENT_SOURCE_DIR}/nlsandler-book/test_compiler
	${PROJECT_SOURCE_DIR}/scripts/driver.sh
	--expected-error-codes 70 --chapter 17 --extra-credit
)
