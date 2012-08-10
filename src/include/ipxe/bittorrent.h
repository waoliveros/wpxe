#ifndef _IPXE_BITTORRENT_H
#define _IPXE_BITTORRENT_H

/** @file
 *
 * BitTorrent protocol
 *
 */

FILE_LICENCE ( GPL2_OR_LATER );

/** BitTorrent default port */
#define BITTORRENT_PORT 49155

extern int bt_open_filter ( struct interface *xfer, struct uri *uri,
			      unsigned int default_port,
			      int ( * filter ) ( struct interface *,
						 struct interface ** ) );

#endif /* _IPXE_BITTORRENT_H */
