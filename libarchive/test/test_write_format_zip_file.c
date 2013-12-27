/*-
 * Copyright (c) 2003-2008 Tim Kientzle
 * Copyright (c) 2008 Anselm Strauss
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

/*
 * Development supported by Google Summer of Code 2008.
 */

#include "test.h"
__FBSDID("$FreeBSD: head/lib/libarchive/test/test_write_format_zip.c 201247 2009-12-30 05:59:21Z kientzle $");

static unsigned long
bitcrc32(unsigned long c, void *_p, size_t s)
{
	/* This is a drop-in replacement for crc32() from zlib.
	 * Libarchive should be able to correctly generate
	 * uncompressed zip archives (including correct CRCs) even
	 * when zlib is unavailable, and this function helps us verify
	 * that.  Yes, this is very, very slow and unsuitable for
	 * production use, but it's correct, compact, and works well
	 * enough for this particular usage.  Libarchive internally
	 * uses a much more efficient implementation.  */
	const unsigned char *p = _p;
	int bitctr;

	if (p == NULL)
		return (0);

	for (; s > 0; --s) {
		c ^= *p++;
		for (bitctr = 8; bitctr > 0; --bitctr) {
			if (c & 1) c = (c >> 1);
			else	   c = (c >> 1) ^ 0xedb88320;
			c ^= 0x80000000;
		}
	}
	return (c);
}

/* Quick and dirty: Read 2-byte and 4-byte integers from Zip file. */
static unsigned i2(const unsigned char *p) { return ((p[0] & 0xff) | ((p[1] & 0xff) << 8)); }
static unsigned i4(const unsigned char *p) { return (i2(p) | (i2(p + 2) << 16)); }

DEFINE_TEST(test_write_format_zip_file)
{
	struct archive *a;
	struct archive_entry *ae;
	time_t t = 1234567890;
	struct tm *tm = localtime(&t);
	size_t used, buffsize = 1000000;
	unsigned long crc;
	int file_perm = 00644;
	int zip_version = 20;
	int zip_compression = 8;
	short file_uid = 10, file_gid = 20;
	unsigned char *buff, *buffend, *p, *q, *central_header, *local_header;
	unsigned char *extension_start, *extension_end;
	char file_data[] = {'1', '2', '3', '4', '5', '6', '7', '8'};
	char *file_name = "file";

#ifndef HAVE_ZLIB_H
	zip_version = 10;
	zip_compression = 0;
#endif


	buff = malloc(buffsize);

	/* Create a new archive in memory. */
	assert((a = archive_write_new()) != NULL);
	assertEqualIntA(a, ARCHIVE_OK, archive_write_set_format_zip(a));
	assertEqualIntA(a, ARCHIVE_OK,
	    archive_write_open_memory(a, buff, buffsize, &used));

	assert((ae = archive_entry_new()) != NULL);
	archive_entry_copy_pathname(ae, file_name);
	archive_entry_set_mode(ae, AE_IFREG | file_perm);
	archive_entry_set_size(ae, sizeof(file_data));
	archive_entry_set_uid(ae, file_uid);
	archive_entry_set_gid(ae, file_gid);
	archive_entry_set_mtime(ae, t, 0);
	assertEqualInt(0, archive_write_header(a, ae));
	archive_entry_free(ae);
	assertEqualInt(8, archive_write_data(a, file_data, sizeof(file_data)));
	assertEqualIntA(a, ARCHIVE_OK, archive_write_close(a));
	assertEqualInt(ARCHIVE_OK, archive_write_free(a));
	buffend = buff + used;
	dumpfile("constructed.zip", buff, used);

	/* Verify "End of Central Directory" record. */
	/* Get address of end-of-central-directory record. */
	p = buffend - 22; /* Assumes there is no zip comment field. */
	failure("End-of-central-directory begins with PK\\005\\006 signature");
	assertEqualMem(p, "PK\005\006", 4);
	failure("This must be disk 0");
	assertEqualInt(i2(p + 4), 0);
	failure("Central dir must start on disk 0");
	assertEqualInt(i2(p + 6), 0);
	failure("All central dir entries are on this disk");
	assertEqualInt(i2(p + 8), i2(p + 10));
	failure("CD start (%d) + CD length (%d) should == archive size - 22",
	    i4(p + 12), i4(p + 16));
	assertEqualInt(i4(p + 12) + i4(p + 16), used - 22);
	failure("no zip comment");
	assertEqualInt(i2(p + 20), 0);

	/* Get address of first entry in central directory. */
	central_header = p = buff + i4(buffend - 6);
	failure("Central file record at offset %d should begin with"
	    " PK\\001\\002 signature",
	    i4(buffend - 10));

	/* Verify file entry in central directory. */
	assertEqualMem(p, "PK\001\002", 4); /* Signature */
	assertEqualInt(i2(p + 4), 3 * 256 + zip_version); /* Version made by */
	assertEqualInt(i2(p + 6), zip_version); /* Version needed to extract */
	assertEqualInt(i2(p + 8), 8); /* Flags */
	assertEqualInt(i2(p + 10), zip_compression); /* Compression method */
	assertEqualInt(i2(p + 12), (tm->tm_hour * 2048) + (tm->tm_min * 32) + (tm->tm_sec / 2)); /* File time */
	assertEqualInt(i2(p + 14), ((tm->tm_year - 80) * 512) + ((tm->tm_mon + 1) * 32) + tm->tm_mday); /* File date */
	crc = bitcrc32(0, file_data, sizeof(file_data));
	assertEqualInt(i4(p + 16), crc); /* CRC-32 */
	/* assertEqualInt(i4(p + 20), sizeof(file_data)); */ /* Compressed size */
	assertEqualInt(i4(p + 24), sizeof(file_data)); /* Uncompressed size */
	assertEqualInt(i2(p + 28), strlen(file_name)); /* Pathname length */
	/* assertEqualInt(i2(p + 30), 28); */ /* Extra field length: See below */
	assertEqualInt(i2(p + 32), 0); /* File comment length */
	assertEqualInt(i2(p + 34), 0); /* Disk number start */
	assertEqualInt(i2(p + 36), 0); /* Internal file attrs */
	assertEqualInt(i4(p + 38) >> 16 & 01777, file_perm); /* External file attrs */
	assertEqualInt(i4(p + 42), 0); /* Offset of local header */
	assertEqualMem(p + 46, file_name, strlen(file_name)); /* Pathname */
	p = extension_start = central_header + 46 + strlen(file_name);
	extension_end = extension_start + i2(central_header + 30);

	while (p < extension_end) {
		switch(i2(p)) {
		case 0x5455:  /* 'UT' extension header */
			assertEqualInt(i2(p + 2), 5); /* 'UT' size */
			assertEqualInt(p[4], 1); /* 'UT' flags */
			assertEqualInt(i4(p + 5), t); /* 'UT' mtime */
			p = p + 4 + i2(p + 2);
			break;
		case 0x7875:  /* 'ux' extension header */
			assertEqualInt(i2(p + 2), 11); /* 'ux' size */
			/* TODO: verify 'ux' contents */
			p = p + 4 + i2(p + 2);
			break;
		default:
			failure("Unexpected extension 0x%04X", i2(p + 2));
			assert(0);
			break;
		}
	}
	/* Should have run exactly to end of extra data. */
	assert(p == extension_end);

	/* Verify local header of file entry. */
	q = local_header = buff;
	assertEqualMem(q, "PK\003\004", 4); /* Signature */
	assertEqualInt(i2(q + 4), zip_version); /* Version needed to extract */
	assertEqualInt(i2(q + 6), 8); /* Flags */
	assertEqualInt(i2(q + 8), zip_compression); /* Compression method */
	assertEqualInt(i2(q + 10), (tm->tm_hour * 2048) + (tm->tm_min * 32) + (tm->tm_sec / 2)); /* File time */
	assertEqualInt(i2(q + 12), ((tm->tm_year - 80) * 512) + ((tm->tm_mon + 1) * 32) + tm->tm_mday); /* File date */
	assertEqualInt(i4(q + 14), 0); /* CRC-32 */
	/* assertEqualInt(i4(q + 18), sizeof(file_data)); */ /* Compressed size */
	/* assertEqualInt(i4(q + 22), sizeof(file_data)); */ /* Uncompressed size not stored because we're using length-at-end. */
	assertEqualInt(i2(q + 26), strlen(file_name)); /* Pathname length */
	assertEqualInt(i2(q + 28), 24); /* Extra field length */
	assertEqualMem(q + 30, file_name, strlen(file_name)); /* Pathname */
	q = extension_start = local_header + 30 + strlen(file_name);
	extension_end = extension_start + i2(local_header + 28);

	while (q < extension_end) {
		switch(i2(q)) {
		case 0x5455:  /* 'UT' extension header */
			assertEqualInt(i2(q + 2), 5); /* 'UT' size */
			assertEqualInt(q[4], 1); /* 'UT' flags */
			assertEqualInt(i4(q + 5), t); /* 'UT' mtime */
			q = q + 4 + i2(q + 2);
			break;
		case 0x7875: /* 'ux' extension header */
			assertEqualInt(i2(q + 2), 11); /* 'ux' size */
			assertEqualInt(q[4], 1); /* 'ux' version */
			assertEqualInt(q[5], 4); /* 'ux' uid size */
			assertEqualInt(i4(q + 6), file_uid); /* 'Ux' UID */
			assertEqualInt(q[10], 4); /* 'ux' gid size */
			assertEqualInt(i4(q + 11), file_gid); /* 'Ux' GID */
			q = q + 4 + i2(q + 2);
			break;
		default:
			failure("Unexpected extension 0x%04X", i2(p + 2));
			assert(0);
			break;
		}
	}
	/* Should have run exactly to end of extra data. */
	assert(q == extension_end);

	/* Data descriptor should follow compressed data. */
	while (q < central_header && memcmp(q, "PK\007\010", 4) != 0)
		++q;
	assertEqualMem(q, "PK\007\010", 4);
	assertEqualInt(i4(q + 4), crc); /* CRC-32 */
	/* assertEqualInt(i4(q + 8), ???); */ /* compressed size */
	assertEqualInt(i4(q + 12), sizeof(file_data)); /* uncompressed size */

	/* Central directory should immediately follow the only entry. */
	assert(q + 16 == central_header);

	free(buff);
}
