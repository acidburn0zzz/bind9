/*
 * Copyright (C) 1999  Internet Software Consortium.
 * 
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 * 
 * THE SOFTWARE IS PROVIDED "AS IS" AND INTERNET SOFTWARE CONSORTIUM DISCLAIMS
 * ALL WARRANTIES WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL INTERNET SOFTWARE
 * CONSORTIUM BE LIABLE FOR ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL
 * DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR
 * PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS
 * ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS
 * SOFTWARE.
 */

/* $Id: dir.c,v 1.6 1999/10/31 19:29:48 halley Exp $ */

/* Principal Authors: DCL */

#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>

#include <isc/dir.h>
#include <isc/assertions.h>
#include <isc/error.h>

#define ISC_DIR_MAGIC		0x4449522aU	/* DIR*. */
#define VALID_DIR(dir)		((dir) != NULL && \
				 (dir)->magic == ISC_DIR_MAGIC)

void
isc_dir_init(isc_dir_t *dir) {
	REQUIRE(dir != NULL);

	dir->entry.name[0] = '\0';
	dir->entry.length = 0;

	dir->handle = NULL;

	dir->magic = ISC_DIR_MAGIC;
}

/*
 * Allocate workspace and open directory stream. If either one fails, 
 * NULL will be returned.
 */
isc_result_t
isc_dir_open(isc_dir_t *dir, const char *dirname) {
	isc_result_t result = ISC_R_SUCCESS;

	REQUIRE(VALID_DIR(dir));
	REQUIRE(dirname != NULL);

	/*
	 * Open stream.
	 */
	dir->handle = opendir(dirname);

	if (dir->handle == NULL) {
		if (errno == ENOMEM)
			result = ISC_R_NOMEMORY;
		else if (errno == EPERM)
			result = ISC_R_NOPERM;
		else if (errno == ENOENT)
			result = ISC_R_NOTFOUND;
		else {
			UNEXPECTED_ERROR(__FILE__, __LINE__,
					 "opendir(%s) failed: %s",
					 dirname, strerror(errno));
			return (ISC_R_UNEXPECTED);
		}
	}

	return (result);
}

/*
 * Return previously retrieved file or get next one.  Unix's dirent has
 * separate open and read functions, but the Win32 and DOS interfaces open
 * the dir stream and reads the first file in one operation.
 */
isc_result_t
isc_dir_read(isc_dir_t *dir) {
	struct dirent *entry;

	REQUIRE(VALID_DIR(dir) && dir->handle != NULL);

	/*
	 * Fetch next file in directory.
	 */
	entry = readdir(dir->handle);

	if (entry == NULL)
		return (ISC_R_NOMORE);

	/*
	 * Make sure that the space for the name is long enough. 
	 */
	if (sizeof(dir->entry.name) <= strlen(entry->d_name))
	    return (ISC_R_UNEXPECTED);

	strcpy(dir->entry.name, entry->d_name);

	/*
	 * Some dirents have d_namlen, but it is not portable.
	 */
	dir->entry.length = strlen(entry->d_name);

	return (ISC_R_SUCCESS);
}

/*
 * Close directory stream.
 */
void
isc_dir_close(isc_dir_t *dir) {
       REQUIRE(VALID_DIR(dir) && dir->handle != NULL);

       (void)closedir(dir->handle);
       dir->handle = NULL;
}

/*
 * Reposition directory stream at start.
 */
isc_result_t
isc_dir_reset(isc_dir_t *dir) {
	REQUIRE(VALID_DIR(dir) && dir->handle != NULL);

	rewinddir(dir->handle);

	return (ISC_R_SUCCESS);
}

/*
 * XXX Is there a better place for this?
 */

isc_result_t
isc_dir_chdir(const char *dirname) {
	/*
	 * Change the current directory to 'dirname'.
	 */

	REQUIRE(dirname != NULL);

	if (chdir(dirname) < 0) {
		if (errno == ENOENT)
			return (ISC_R_NOTFOUND);
		else if (errno == EACCES)
			return (ISC_R_NOPERM);
		else if (errno == ENOMEM)
			return (ISC_R_NOMEMORY);
		else {
			UNEXPECTED_ERROR(__FILE__, __LINE__,
					 "chdir(%s) failed: %s",
					 dirname, strerror(errno));
			return (ISC_R_UNEXPECTED);
		}
	}

	return (ISC_R_SUCCESS);
}
