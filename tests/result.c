#include "result.h"

#include "greatest.h"
#include "sys/compiler_features.h"
#include "sys/debug.h"

#define ASSERT_IN(haystack, needle)                                            \
	do {                                                                   \
		auto_result_str owner = haystack;                              \
		debug("Checking for \"%s\" in \"%s\"", needle, owner);         \
		ASSERT_NEQ(NULL, strstr(owner == NULL ? "" : owner, needle));  \
	} while (0)

static WARN_UNUSED char *
make(int err_type)
{
	auto_result err = make_result(err_type);
	return result_to_str(err);
}

static WARN_UNUSED char *
make_n(int err_type)
{
	auto_result err = make_result(err_type, 0);
	return result_to_str(err);
}

TEST
print_to_str_each_enum_value(void) // NOLINT(*-function-size)
{
	ASSERT_IN(make(OK), "Success");
	ASSERT_IN(make_n(ERR_TMPFILE), "Error in tmpfile");
	ASSERT_IN(make_n(ERR_TMPFILE_FILENO), "Error fileno");
	ASSERT_IN(make_n(ERR_TMPFILE_DUP), "Error dup");
	ASSERT_IN(make_n(ERR_TMPFILE_FSTAT), "Error fstat");
	ASSERT_IN(make_n(ERR_TMPFILE_MMAP), "Error mmap");
	ASSERT_IN(make_n(ERR_TMPFILE_LSEEK), "Error seeking");
	ASSERT_IN(make_n(ERR_TMPFILE_FTRUNCATE), "Error truncating");
	PASS();
}

SUITE(print_to_str)
{
	RUN_TEST(print_to_str_each_enum_value);
}
