#include "sys/tmpfile.h"

#include "sys/debug.h"

#include <assert.h>
#include <sys/mman.h>
#include <sys/stat.h>

result_t
tmpmap(int fd, struct string_view *addr)
{
	struct stat st = {
		.st_size = 0,
	};
	check_if(fstat(fd, &st) < 0, ERR_TMPFILE_FSTAT, errno);
	addr->sz = st.st_size;

	addr->data = mmap(NULL, addr->sz, PROT_READ, MAP_PRIVATE, fd, 0);
	check_if(addr->data == MAP_FAILED, ERR_TMPFILE_MMAP, errno);

	/*
	 * mmap() can technically return NULL on some platforms, but our
	 * callers use NULL as a default/sentinel value to signal failure.
	 * Just bail out under this condition. If we ever want to deal with
	 * this, we'll need to export MAP_FAILED and break encapsulation of
	 * the tmpfile.c module a bit.
	 */
	assert(addr->data != NULL);

	return RESULT_OK;
}

void
tmpunmap(struct string_view *addr)
{
	if (addr->data == MAP_FAILED || addr->data == NULL) {
		return;
	}

	const int rc = munmap((void *)addr->data, addr->sz);
	info_m_if(rc < 0, "Ignoring error munmap()-ing tmpfile");

	addr->data = NULL;
	addr->sz = 0;
}
