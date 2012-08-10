#ifndef _IPXE_DOWNLOADER_H
#define _IPXE_DOWNLOADER_H

/** @file
 *
 * Image downloader
 *
 */

FILE_LICENCE ( GPL2_OR_LATER );

#include <ipxe/job.h>
#include <ipxe/xfer.h>
#include <ipxe/image.h>

struct interface;
struct image;

/** A downloader */
struct downloader {
	/** Reference count for this object */
	struct refcnt refcnt;

	/** Job control interface */
	struct interface job;
	/** Data transfer interface */
	struct interface xfer;

	/** Image to contain downloaded file */
	struct image *image;
	/** Current position within image buffer */
	size_t pos;
};

extern int create_downloader ( struct interface *job, struct image *image,
			       int type, ... );

#endif /* _IPXE_DOWNLOADER_H */
