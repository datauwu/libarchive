/*-
 * Copyright (c) 2014 Sebastian Freundt
 * Author: Sebastian Freundt  <devel@fresse.org>
 *
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR(S) ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR(S) BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "archive_platform.h"

#ifdef HAVE_ERRNO_H
#include <errno.h>
#endif
#include <stdio.h>
#ifdef HAVE_STDLIB_H
#include <stdlib.h>
#endif
#ifdef HAVE_STRING_H
#include <string.h>
#endif
#ifdef HAVE_TIME_H
#include <time.h>
#endif

#include "archive.h"
#include "archive_entry.h"
#include "archive_entry_locale.h"
#include "archive_private.h"
#include "archive_random_private.h"
#include "archive_write_private.h"
#include "archive_write_set_format_private.h"

/*
 * Overview of the WARC writer:
 *
 * This writer emits WARC/1.0 resource records for regular files.  It
 * writes a warcinfo record once by default, unless the omit-warcinfo
 * option is used.  Each resource record gets a WARC-Target-URI from
 * the entry pathname, a generated WARC-Record-ID, and a mandatory
 * Content-Length header.
 */

struct warc {
	unsigned int omit_warcinfo:1;

	time_t now;
	mode_t filetype;
	/* Remaining bytes to write for the current entry */
	uint64_t entry_bytes_remaining;
};

#define WARC_HEADER_MAX_SIZE 512

static const char warcinfo_payload[] =
    "software: libarchive/" ARCHIVE_VERSION_ONLY_STRING "\r\n"
    "format: WARC file version 1.0\r\n";

enum warc_type {
	WARC_TYPE_NONE,
	/* WARC info */
	WARC_TYPE_INFO,
	/* Metadata */
	WARC_TYPE_METADATA,
	/* Resource */
	WARC_TYPE_RESOURCE,
	/* Request, unsupported */
	WARC_TYPE_REQUEST,
	/* Response, unsupported by this writer */
	WARC_TYPE_RESPONSE,
	/* Revisit, unsupported */
	WARC_TYPE_REVISIT,
	/* Conversion, unsupported */
	WARC_TYPE_CONVERSION,
	/* Continuation, currently unsupported */
	WARC_TYPE_CONTINUATION,
	WARC_TYPE_LAST
};

struct warc_header {
	enum warc_type type;
	const char *target_uri;
	const char *record_id;
	time_t record_time;
	time_t modification_time;
	const char *content_type;
	uint64_t content_length;
};

struct warc_uuid {
	unsigned int value[4U];
};

static int	archive_write_warc_options(struct archive_write *,
		    const char *, const char *);
static int	archive_write_warc_header(struct archive_write *,
		    struct archive_entry *);
static ssize_t	archive_write_warc_data(struct archive_write *, const void *,
		    size_t);
static int	archive_write_warc_finish_entry(struct archive_write *);
static int	archive_write_warc_close(struct archive_write *);
static int	archive_write_warc_free(struct archive_write *);

static void	warc_format_time(struct archive_string *, const char *, time_t);
static ssize_t	warc_populate_header(struct archive_string *, size_t,
		    struct warc_header);
static void	warc_generate_uuid(struct warc_uuid *);

/*
 * Set output format to ISO 28500 (aka WARC) format.
 */
int
archive_write_set_format_warc(struct archive *_a)
{
	struct archive_write *a = (struct archive_write *)_a;
	struct warc *warc;

	archive_check_magic(_a, ARCHIVE_WRITE_MAGIC,
	    ARCHIVE_STATE_NEW, "archive_write_set_format_warc");

	/* If another format was already registered, unregister it. */
	(void)__archive_write_unregister_format(a);

	warc = malloc(sizeof(*warc));
	if (warc == NULL) {
		archive_set_error(&a->archive, ENOMEM,
		    "Can't allocate warc data");
		return (ARCHIVE_FATAL);
	}
	/* Emit a warcinfo record by default. */
	warc->omit_warcinfo = 0U;
	/* Use the current time for WARC-Date values. */
	warc->now = time(NULL);
	/* Reset file type information. */
	warc->filetype = 0;

	a->format_data = warc;
	a->format_name = "WARC/1.0";
	a->format_options = archive_write_warc_options;
	a->format_write_header = archive_write_warc_header;
	a->format_write_data = archive_write_warc_data;
	a->format_close = archive_write_warc_close;
	a->format_free = archive_write_warc_free;
	a->format_finish_entry = archive_write_warc_finish_entry;
	a->archive.archive_format = ARCHIVE_FORMAT_WARC;
	a->archive.archive_format_name = "WARC/1.0";
	return (ARCHIVE_OK);
}

static int
archive_write_warc_options(struct archive_write *a, const char *key,
    const char *val)
{
	struct warc *warc = a->format_data;

	if (strcmp(key, "omit-warcinfo") == 0) {
		if (val == NULL || strcmp(val, "true") == 0) {
			/* Option accepted. */
			warc->omit_warcinfo = 1U;
			return (ARCHIVE_OK);
		}
	}

	/* ARCHIVE_WARN tells the options supervisor that this option was not
	 * handled here.  It will report an error if no module uses it. */
	return (ARCHIVE_WARN);
}

static int
archive_write_warc_header(struct archive_write *a, struct archive_entry *entry)
{
	struct warc *warc = a->format_data;
	struct archive_string hdr;

	/* Emit the warcinfo record if needed. */
	if (!warc->omit_warcinfo) {
		ssize_t r;
		int rc;
		struct warc_header wi = {
			WARC_TYPE_INFO,
			/* URI */NULL,
			/* Record ID */NULL,
			/* Record time */0,
			/* Modified time */0,
			/* Content type */"application/warc-fields",
			/* Content length */sizeof(warcinfo_payload) - 1U,
		};
		wi.record_time = warc->now;
		wi.modification_time = warc->now;

		archive_string_init(&hdr);
		r = warc_populate_header(&hdr, WARC_HEADER_MAX_SIZE, wi);
		if (r < 0) {
			archive_string_free(&hdr);
			archive_set_error(&a->archive,
			    ARCHIVE_ERRNO_FILE_FORMAT,
			    "Cannot archive warcinfo record");
			return (ARCHIVE_FAILED);
		}

		/* Reuse the header buffer for the warcinfo payload. */
		archive_strncat(&hdr, warcinfo_payload, sizeof(warcinfo_payload) - 1U);

		/* Append the end-of-record indicator. */
		archive_strncat(&hdr, "\r\n\r\n", 4);

		/* Write the warcinfo record to the output stream. */
		rc = __archive_write_output(a, hdr.s, archive_strlen(&hdr));
		if (rc != ARCHIVE_OK) {
			archive_string_free(&hdr);
			return (rc);
		}

		/* Mark the file header as written. */
		warc->omit_warcinfo = 1U;
		archive_string_free(&hdr);
	}

	if (archive_entry_pathname(entry) == NULL) {
		archive_set_error(&a->archive, EINVAL,
		    "Invalid filename");
		return (ARCHIVE_WARN);
	}

	warc->filetype = archive_entry_filetype(entry);
	warc->entry_bytes_remaining = 0U;
	if (warc->filetype == AE_IFREG) {
		struct warc_header rh = {
			WARC_TYPE_RESOURCE,
			/* URI */NULL,
			/* Record ID */NULL,
			/* Record time */0,
			/* Modified time */0,
			/* Content type */NULL,
			/* Content length */0,
		};
		ssize_t r;
		int rc;
		int64_t size;
		rh.target_uri = archive_entry_pathname(entry);
		rh.record_time = warc->now;
		rh.modification_time = archive_entry_mtime(entry);
		if (!archive_entry_size_is_set(entry)) {
			archive_set_error(&a->archive, -1,
			    "Size required");
			return (ARCHIVE_FAILED);
		}
		size = archive_entry_size(entry);
		if (size < 0) {
			archive_set_error(&a->archive, -1,
			    "Size required");
			return (ARCHIVE_FAILED);
		}
		rh.content_length = (uint64_t)size;

		archive_string_init(&hdr);
		r = warc_populate_header(&hdr, WARC_HEADER_MAX_SIZE, rh);
		if (r < 0) {
			/* Header generation failed. */
			archive_string_free(&hdr);
			archive_set_error(
				&a->archive,
				ARCHIVE_ERRNO_FILE_FORMAT,
				"WARC resource header is too large");
			return (ARCHIVE_FATAL);
		}
		/* Append the header to the output stream. */
		rc = __archive_write_output(a, hdr.s, r);
		if (rc != ARCHIVE_OK) {
			archive_string_free(&hdr);
			return (rc);
		}
		/* Save the remaining size for subsequent _data() calls. */
		warc->entry_bytes_remaining = rh.content_length;
		archive_string_free(&hdr);
		return (ARCHIVE_OK);
	}
	/* Report unsupported file types through the common helper. */
	__archive_write_entry_filetype_unsupported(
	    &a->archive, entry, "WARC");
	return (ARCHIVE_FAILED);
}

static ssize_t
archive_write_warc_data(struct archive_write *a, const void *buf, size_t len)
{
	struct warc *warc = a->format_data;

	if (warc->filetype == AE_IFREG) {
		int rc;

		/* Never write more bytes than announced. */
		if ((uint64_t)len > warc->entry_bytes_remaining) {
			len = (size_t)warc->entry_bytes_remaining;
		}

		/* Write the entry data. */
		rc = __archive_write_output(a, buf, len);
		if (rc != ARCHIVE_OK) {
			return rc;
		}
		warc->entry_bytes_remaining -= len;
	}
	return len;
}

static int
archive_write_warc_finish_entry(struct archive_write *a)
{
	static const char _eor[] = "\r\n\r\n";
	struct warc *warc = a->format_data;

	if (warc->filetype == AE_IFREG) {
		int rc;

		if (warc->entry_bytes_remaining != 0U) {
			archive_set_error(&a->archive, ARCHIVE_ERRNO_MISC,
			    "WARC entry is shorter than Content-Length");
			return (ARCHIVE_FATAL);
		}

		rc = __archive_write_output(a, _eor, sizeof(_eor) - 1U);
		if (rc != ARCHIVE_OK) {
			return rc;
		}
	}
	/* reset type info */
	warc->filetype = 0;
	return (ARCHIVE_OK);
}

static int
archive_write_warc_close(struct archive_write *a)
{
	(void)a; /* UNUSED */
	return (ARCHIVE_OK);
}

static int
archive_write_warc_free(struct archive_write *a)
{
	struct warc *warc = a->format_data;

	free(warc);
	a->format_data = NULL;
	return (ARCHIVE_OK);
}

static void
warc_format_time(struct archive_string *as, const char *fmt, time_t t)
{
/* Like strftime(3), but for time_t objects. */
	struct tm *rt;
#if defined(HAVE_GMTIME_R) || defined(HAVE_GMTIME_S)
	struct tm timeHere;
#endif
	char strtime[100];
	size_t len;

#if defined(HAVE_GMTIME_S)
	rt = gmtime_s(&timeHere, &t) ? NULL : &timeHere;
#elif defined(HAVE_GMTIME_R)
	rt = gmtime_r(&t, &timeHere);
#else
	rt = gmtime(&t);
#endif
	if (!rt)
		return;
	/* Let strftime() handle the actual formatting. */
	len = strftime(strtime, sizeof(strtime)-1, fmt, rt);
	archive_strncat(as, strtime, len);
}

static ssize_t
warc_populate_header(struct archive_string *tgt, size_t tsz,
    struct warc_header hdr)
{
	static const char _ver[] = "WARC/1.0\r\n";
	static const char * const _typ[WARC_TYPE_LAST] = {
		NULL, "warcinfo", "metadata", "resource", NULL
	};
	char std_uuid[48U];

	if (hdr.type == WARC_TYPE_NONE || hdr.type > WARC_TYPE_RESOURCE) {
		/* Invalid record type for this writer. */
		return -1;
	}

	archive_strcpy(tgt, _ver);

	archive_string_sprintf(tgt, "WARC-Type: %s\r\n", _typ[hdr.type]);

	if (hdr.target_uri != NULL) {
		/* Check whether the value already contains ://. */
		static const char _uri[] = "";
		static const char _fil[] = "file://";
		const char *u;
		const char *chk = strchr(hdr.target_uri, ':');

		if (chk != NULL && chk[1U] == '/' && chk[2U] == '/') {
			/* Already has a scheme-style :// prefix. */
			u = _uri;
		} else {
			/* Prepend file:// for local paths. */
			u = _fil;
		}
		archive_string_sprintf(tgt,
			"WARC-Target-URI: %s%s\r\n", u, hdr.target_uri);
	}

	/* Write WARC-Date from hdr.record_time. */
	warc_format_time(tgt, "WARC-Date: %Y-%m-%dT%H:%M:%SZ\r\n",
	    hdr.record_time);

	/* Also write Last-Modified from hdr.modification_time. */
	warc_format_time(tgt, "Last-Modified: %Y-%m-%dT%H:%M:%SZ\r\n",
	    hdr.modification_time);

	if (hdr.record_id == NULL) {
		/* Generate a record ID when one was not provided. */
		struct warc_uuid u;

		warc_generate_uuid(&u);
		/* archive_string_sprintf() does not support minimum field widths, so
		 * use snprintf() for UUID formatting. */
#if defined(_WIN32) && !defined(__CYGWIN__) && !( defined(_MSC_VER) && _MSC_VER >= 1900)
#define snprintf _snprintf
#endif
		snprintf(
			std_uuid, sizeof(std_uuid),
			"<urn:uuid:%08x-%04x-%04x-%04x-%04x%08x>",
			u.value[0U],
			u.value[1U] >> 16U, u.value[1U] & 0xffffU,
			u.value[2U] >> 16U, u.value[2U] & 0xffffU,
			u.value[3U]);
		hdr.record_id = std_uuid;
	}

	/* WARC-Record-ID is mandatory. */
	archive_string_sprintf(tgt, "WARC-Record-ID: %s\r\n", hdr.record_id);

	if (hdr.content_type != NULL) {
		archive_string_sprintf(tgt, "Content-Type: %s\r\n", hdr.content_type);
	}

	/* Content-Length is mandatory. */
	archive_string_sprintf(tgt, "Content-Length: %ju\r\n", (uintmax_t)hdr.content_length);
	/* End of header. */
	archive_strncat(tgt, "\r\n", 2);

	return (archive_strlen(tgt) >= tsz)? -1: (ssize_t)archive_strlen(tgt);
}

static void
warc_generate_uuid(struct warc_uuid *tgt)
{
	archive_random(tgt->value, sizeof(tgt->value));
	/* Apply UUID version 4 rules. */
	tgt->value[1U] &= 0xffff0fffU;
	tgt->value[1U] |= 0x4000U;
	tgt->value[2U] &= 0x3fffffffU;
	tgt->value[2U] |= 0x80000000U;
}
