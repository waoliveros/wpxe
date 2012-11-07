/*
 * Copyright (C) 2011 Wigi Vei Oliveros <waoliveros@up.edu.ph>.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License, or any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

FILE_LICENCE ( GPL2_OR_LATER );

/**
 * @file
 *
 * BitTorrent Protocol
 *
 */
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <byteswap.h>
#include <errno.h>
#include <assert.h>
#include <stdarg.h>
#include <unistd.h>

#include <ipxe/interface.h>
#include <ipxe/uaccess.h>
#include <ipxe/umalloc.h>
#include <ipxe/uri.h>
#include <ipxe/refcnt.h>
#include <ipxe/iobuf.h>
#include <ipxe/xfer.h>
#include <ipxe/open.h>
#include <ipxe/socket.h>
#include <ipxe/tcpip.h>
#include <ipxe/process.h>
#include <ipxe/linebuf.h>
#include <ipxe/features.h>
#include <ipxe/base64.h>
#include <ipxe/http.h>
#include <ipxe/bencode.h>
#include <ipxe/monojob.h>

/** Will be used for reading the pieces to be sent */
#include <ipxe/image.h>
#include <ipxe/downloader.h>

#include <ipxe/bittorrent.h>

#define BITTORRENT_PORT 49155
#define BT_HANDSHAKELEN (1 + 19 + 8 + 20 + 20)
#define BT_TEST_HASH "4c0e766e8bbe53baa0410a5a698c4b3916224c0f"

FEATURE ( FEATURE_PROTOCOL, "BitTorrent", DHCP_EB_FEATURE_BITTORRENT, 1 );

/** Function prototypes */
static int bt_tx_handshake ();
static int bt_rx_handshake ();
static struct bt_peer * bt_create_peer ();
static int bt_socket_open ();

static int bt_tx_keep_alive ();
static int bt_tx_interested ();
// static int bt_tx_have ();
static int bt_tx_request ();
// static int bt_tx_piece ();
// static int bt_tx_cancel ();

/** Hack variables */
//static int has_peers = 0;


/**
 * Free BitTorrent request
 *
 * @v refcnt		Reference counter
 */

static void bt_free ( struct refcnt *refcnt ) {
	struct bt_request *bt =
		container_of ( refcnt, struct bt_request, refcnt );
	free ( bt );
};

/**
 * Close BitTorrent request
 */

static void bt_close ( struct bt_request *bt, int rc ) {
	
	/* Remove process */
	process_del ( &bt->process );
	
	/* Close all data interfaces */
	intf_shutdown ( &bt->xfer, rc );
	intf_shutdown ( &bt->listener, rc );
}

/**
 * Free BitTorrent peer
 *
 * @v refcnt		Reference counter
 */

static void bt_peer_free ( struct refcnt *refcnt ) {
	struct bt_peer *peer =
		container_of ( refcnt, struct bt_peer, refcnt );
	free ( peer );
};

/**  
* Close the peer connection
* When a peer gets disconnected, this is also called.
*/
static void bt_peer_close ( struct bt_peer *peer, int rc ) {
	
	/** Remove peer from peer list */
	list_del( &peer->list );
	intf_shutdown ( &peer->socket, rc);
	
	/** Reference to bt->peer_info and bt->peerid is no longer needed */
	ref_put ( &peer->bt->refcnt );
	
	bt_peer_free ( &peer->refcnt );
}

/** Count BT peers */
static int bt_count_peers ( struct bt_request *bt ) {
	struct bt_peer *peer;
	int i = 0;
	list_for_each_entry ( peer, &bt->peers, list ) {
		i++;
	}
	return i;
}

/** BitTorrent read block from memory 
*
* This function may not be needed anymore.
* We can just use copy_from_user to load bytes to an array.
*/
//static size_t bt_fetch_block ( struct image *image, uint8_t *dest,
//									 off_t offset, size_t len ) {
//	
//	copy_from_user ( dest, image->data, offset, len );
//	
//	return iob_len ( iobuf ); 
//}

/** Handle new data arriving via BitTorrent peer connection
*	There is an assumption here that only PIECE messages are segmented.
*/
static int bt_peer_socket_deliver ( struct bt_peer *peer,
									struct io_buffer *iobuf,
									struct xfer_metadata *meta __unused ) {
	int rc = 0;
	struct bt_message *message = iobuf->data;
	struct bt_handshake *handshake = iobuf->data;
	size_t data_len = iob_len ( iobuf );

	DBG ( "BT received buffer length: %zd\n", data_len );

	/** If rx_buffer has no content, point to iobuf
	  * Else, append content of iobuf to rx_buffer
	  */

	/** We are still unsure how to handle this */
	while ( iobuf && iob_len ( iobuf ) ) {
		
		data_len = iob_len ( iobuf );

		/** Check in which state is the peer right now. */
		switch ( peer->state ) {
		/** Peer is waiting for handshake */
		case BT_PEER_HANDSHAKE_SENT:
			// Check if message received is a handshake
			if ( handshake->pstrlen == 19 ) {
				DBG ( "BT received handshake length: %zd\n", data_len );
				// process received handshake then send own
				if ( bt_rx_handshake ( peer, handshake ) == 0 ) {
					peer->state = BT_PEER_HANDSHAKE_RCVD;
					DBG ( "BT peer's info_hash is the same.\n" );
					iob_pull ( iobuf, BT_HANDSHAKELEN );
				}	
			}
			break;
		default:
			DBG ( "BT some message received from peer.\n" );

			/** Check if we are currently processing a message */
			if ( peer->rx_len == 0 ) {
				// Check buffer length
				if ( data_len >= BT_HEADER ) {
					message = iobuf->data;
					peer->rx_len = ntohl ( message->len );
					peer->rx_id = message->id;
					DBG ( "BT prefix length: %zd\n", peer->rx_len );
					if ( peer->rx_len == 0 ) {
						DBG ( "BT KEEP ALIVE received\n" );
						//bt_tx_request ( peer, 17, 0, 16 * 1024 );
					} else {
						//DBG ( "BT message received is OTHER\n" );
						peer->remaining = peer->rx_len;
						data_len -= BT_PREFIXLEN;
					}
					iob_pull ( iobuf, BT_PREFIXLEN );
				} else {
					return 0; // skip wait for more bytes
				}
			}

			/** Check if there are still remaining bytes from message
			*   and process if complete.
			*/
			if ( peer->remaining && ( peer->remaining <= data_len ) ) {
				DBG ( "BT message complete\n" );
				switch ( peer->rx_id ) {
				case BT_CHOKE:
					break;	
				case BT_UNCHOKE:
					DBG ( "BT UNCHOKE received\n" );
					while ( xfer_window ( &peer->socket ) <= 0 );
					if ( bt_tx_interested ( peer ) != 0 )
						DBG ( "BT error sending INTERESTED to peer %p\n", peer );
					if ( bt_tx_request ( peer, 0, 0,  16 * 1024 ) != 0 )
						DBG ( "BT error sending REQUEST to peer %p\n", peer );
						bt_tx_request ( peer, 1, 0,  16 * 1024 );
						bt_tx_request ( peer, 2, 0,  16 * 1024 );
						bt_tx_request ( peer, 3, 0,  16 * 1024 );
						bt_tx_request ( peer, 4, 0,  16 * 1024 );
						bt_tx_request ( peer, 5, 0,  16 * 1024 );
						bt_tx_request ( peer, 6, 0,  16 * 1024 );
						bt_tx_request ( peer, 7, 0,  16 * 1024 );
						bt_tx_request ( peer, 8, 0,  16 * 1024 );
						bt_tx_request ( peer, 9, 0,  16 * 1024 );
						bt_tx_request ( peer, 10, 0,  16 * 1024 );
					break;
				case BT_INTERESTED:
					DBG ( "BT INTERESTED received\n" );
					break;
				case BT_NOTINTERESTED:
					DBG ( "BT NOT INTERESTED received\n" );
					break;
				case BT_BITFIELD:
					DBG ( "BT BITFIELD received\n" );
					break;
				case BT_HAVE:
					DBG ( "BT HAVE received\n" );
					break;
				case BT_REQUEST:
					DBG ( "BT REQUEST received\n" );
					break;
				case BT_PIECE:
					memcpy ( iob_put ( peer->rx_buffer, peer->remaining ) , iobuf->data, peer->remaining ); 
					DBG ( "BT PIECE received\n" );
					DBG ( "BT rx_buffer length: %zd\n", iob_len ( peer->rx_buffer ) );
					
					// Add code to process piece here
					// For now we remove the contents of rx_buffer
					iob_empty ( peer->rx_buffer );
					
					// Deliver piece to upper layer
					xfer_deliver_iob ( iob_disown ( peer->rx_buffer ) );
					// Reallocate buffer
					peer->rx_buffer = alloc_iob ( sizeof ( 18 * 1024 ) );

					iob_pull ( iobuf, peer->remaining );
					DBG ( "BT iobuf length: %zd\n", iob_len ( iobuf ) );
					// We put return here to skip code lines below
					goto done;
				case BT_CANCEL:
					DBG ( "BT CANCEL received\n" );
					break;
				case BT_PORT:
					DBG ( "BT PORT received\n" );
					break;
				}

				DBG ( "BT removing %zd bytes from buffer\n", peer->rx_len );
				iob_pull ( iobuf, peer->rx_len ); 
			done:
				//data_len -= peer->remaining;
				peer->remaining = 0;
				peer->rx_len = 0;
				peer->rx_id = 0;				
			} else if ( peer->remaining && ( peer->remaining > data_len ) ) {
				DBG ( "BT message remaining %zd\n", peer->remaining );
				peer->remaining -= data_len;
				// Add buffer to list of buffers
				// list_add_tail ( &iobuf->list, &peer->buffers );
				// Add piece data to buffer
				memcpy ( iob_put ( peer->rx_buffer, data_len ) , iobuf->data, data_len ); 
				iob_disown ( iobuf );
				// Remove all data from buffer.
				//iob_pull ( iobuf, data_len );
			}
			break;			
		}
	}

	return rc;
}

/** BitTorrent process 
	This function is always executed in the background. 
	Chain of events should be kept to a minimum
	to avoid blocking other functions.
*/
static void bt_step ( struct bt_request *bt ) {
	
	if ( bt )
	//	printf("*");
	
	/** If there are no peers get list from tracker */

	/** If there are no connected peers */	
	if ( ! bt_count_peers ( bt ) ) {
		
		/** The following is just a test scenario
		 Simulated tracker comm. Populate list 
		 Contact peers in list. Send handshake. */
		struct bt_peer *peer;
		peer = bt_create_peer ( bt );
		peer->uri = parse_uri ( "tcp://192.168.4.1:50986" );
		ref_init ( &peer->uri->refcnt, NULL );
		uri_get ( peer->uri );	
		if ( bt_socket_open ( peer ) != 0 ) {
			// ERROR!
		}
		DBG ( "BT test socket opened!\n" );
		
		/** Initialize peer URI */
		//peer->uri = 
		
		/** For now, let's manually set the info hash */
		//bt->info_hash = BT_DEF_INFO_HASH;
		
		//while ( ! xfer_window ( &peer->socket ) )
		//	sleep ( 1 );
		
		//bt_tx_handshake ( peer );
		
		//DBG ( "BT Handshake sent!\n" );
		
		/** Test scenario ends here */
		return;	
	}
	
	//DBG ( "BT %d\n", bt_count_peers ( bt ) );
	struct bt_peer *peer;
	list_for_each_entry ( peer, &bt->peers, list ) {
		//DBG ( "BT +" );
		if ( peer->state == BT_PEER_CREATED && xfer_window ( &peer->socket ) ) {
			bt_tx_handshake ( peer );
			peer->state = BT_PEER_HANDSHAKE_SENT;
			DBG ( "BT handshake sent!\n" );
		}

		if ( peer->state == BT_PEER_HANDSHAKE_RCVD ) {

		}

	}
		
	/* Check if download is finished */
	// If finished, we return to upper layer.
	
	/* Check if tracker communication must be made */
	// If no connections, contact tracker and update list.
	
	/* Check total number of connections, and create new ones if needed */
	// If num of connections is lower than threshold, connect to more.
	// Create new TCP connection to selected peer. bt_connect()
	// Send handshake to selected peer.
	// bt_tx_handshake ()
	// Wait for handshake to finish.
	
	/* Send a request */
	
	return;
}

/** BitTorrent peer socket interface operations */
static struct interface_operation bt_peer_operations[] = {
	INTF_OP ( intf_close, struct bt_peer *, bt_peer_close ),
	INTF_OP ( xfer_deliver, struct bt_peer *, bt_peer_socket_deliver )
};

/** BitTorrent peer socket interface descriptor */
static struct interface_descriptor bt_peer_desc =
	INTF_DESC ( struct bt_peer, socket, bt_peer_operations );
	
/** BitTorrent process descriptor */	
static struct process_descriptor bt_process_desc =
	PROC_DESC ( struct bt_request, process, bt_step );

/** Create a BitTorrent peer OBJECT ONLY
*   and add to list of peers. No initiation of connection here. 
*/
static struct bt_peer * bt_create_peer ( struct bt_request *bt ) {
	
	struct bt_peer *peer;
	
	/** Allocate and initialize struture */
	peer = zalloc ( sizeof ( *peer ) );
	if ( !peer )
		return NULL;

	INIT_LIST_HEAD ( &peer->buffers );

	peer->rx_buffer = alloc_iob ( 16 * 1024 + 9 );
	if ( ! peer->rx_buffer ) {
		DBG ( "BT cannot allocate peer %p rx buffer\n", peer ); 
		return NULL;
	}
	
	/** Add reference to parent request */ 	
	peer->bt = bt;
	ref_get ( &bt->refcnt );
	
	peer->state = BT_PEER_CREATED;
		
	/** Initialize peer refcnt. Function bt_peer_free is called when counter
		drops to zero. Initialize socket with descriptor. Increment peer
		reference counter. */	
	ref_init ( &peer->refcnt, bt_peer_free );
	intf_init ( &peer->socket, &bt_peer_desc, &peer->refcnt );	
	return peer;
}

/** Establish a connection with another peer */
static int bt_socket_open (struct bt_peer *peer) {
	struct uri *uri = peer->uri;
	struct sockaddr_tcpip server;
	struct interface *socket;
	int rc;

	/* Open socket */
	memset ( &server, 0, sizeof ( server ) );
	server.st_port = htons ( uri_port ( uri, 80 ) );
	socket = &peer->socket;
	
	DBG ( "BT opening socket!\n" );
	DBG ( "BT uri scheme: %s\n", uri->scheme );
	DBG ( "BT uri host: %s\n", uri->host );
	DBG ( "BT uri port: %s\n", uri->port );
	
	if ( ( rc = xfer_open_named_socket ( socket, SOCK_STREAM,
					     ( struct sockaddr * ) &server,
					     uri->host, NULL ) ) != 0 )
		goto err; 
	
	/** If there are no errors, add peer to list */
	list_add ( &peer->list, &peer->bt->peers );
	return 0;
err:
	bt_peer_free ( &peer->refcnt );
	return rc;
}

/** Do handshake with a peer */ 
static int bt_tx_handshake ( struct bt_peer *peer ) {

	uint8_t message[BT_HANDSHAKELEN];
	
	/** Check if peer exists */	
	assert ( peer != NULL );

	message[0] = 19;
	memcpy(message + 1, "BitTorrent protocol", 19);
	memcpy(message + 28, peer->bt->info_hash, 20);
	memcpy(message + 48, peer->bt->peerid, 20); 
	DBG ( "BT sending HANDSHAKE to %p\n", peer );
	return xfer_deliver_raw ( &peer->socket, message, sizeof ( message ) );
}

/** Create KEEP-ALIVE message */
static int bt_tx_keep_alive ( struct bt_peer *peer ) {
	DBG ( "BT sending KEEP ALIVE to %p\n", peer );
	return xfer_printf ( &peer->socket, "%c%c%c%c", 0,0,0,0 );
}

/** Create INTERESTED message */
static int bt_tx_interested ( struct bt_peer *peer ) {
	uint8_t message[5];
	message[0] = 0;
	message[1] = 0;
	message[2] = 0;
	message[3] = 1; // length = 5
	message[4] = BT_INTERESTED; // id = 2 
	DBG ( "BT sending INTERESTED to %p\n", peer );
	return xfer_deliver_raw ( &peer->socket, message, sizeof ( message ) );
} 

// /** Create HAVE message */
// static int bt_tx_have ( struct bt_peer *peer, uint32_t index ) {

// 	uint8_t message[9];
	// message[0] = 0;
	// message[1] = 0;
	// message[2] = 0;
// 	message[3] = 5; // length = 5
// 	message[4] = 4; // id = 4
// 	memcpy ( message + 5, &index, sizeof ( index ) ); 

// 	DBG ( "BT sending HAVE to %p", peer );

// 	return xfer_deliver_raw ( &peer->socket, message, sizeof ( message ) );
// } 

/** Send REQUEST message */
static int bt_tx_request ( struct bt_peer *peer, uint32_t index, uint32_t begin, uint32_t length ) {
	uint8_t message[17];
	uint32_t index_n = htonl ( index ) ;
	uint32_t begin_n = htonl ( begin ) ;
	uint32_t length_n = htonl ( length ) ;
	message[0] = 0;
	message[1] = 0;
	message[2] = 0;
	message[3] = 13;
	message[4] = 6;
	memcpy ( message + 5, &index_n, 4 );
	memcpy ( message + 9, &begin_n, 4 );
	memcpy ( message + 13, &length_n, 4 );
	DBG ( "BT sending REQUEST to %p\n", peer );
	return xfer_deliver_raw ( &peer->socket, message, sizeof ( message ) );
}

// /** Send CANCEL message */
// static int bt_tx_cancel ( struct bt_peer *peer, uint32_t index, uint32_t begin, uint32_t length ) {

// 	uint8_t message[17];
	// message[0] = 0;
	// message[1] = 0;
	// message[2] = 0;
// 	message[3] = 13;
// 	message[4] = 8;
// 	memcpy ( message + 5, &index, sizeof ( index ) );
// 	memcpy ( message + 9, &begin, sizeof ( begin ) );
// 	memcpy ( message + 13, &length, sizeof ( length ) );

// 	DBG ( "BT sending CANCEL to %p", peer );

// 	return xfer_deliver_raw ( &peer->socket, message, sizeof ( message ) );
// }

// /** Send PIECE message */
// static int bt_tx_piece ( struct bt_peer *peer, uint32_t index, uint32_t begin, struct io_buffer *iobuf ) {

// 	struct bt_piece *piece;
// 	int rc = 0;
	
// 	// Ensure headroom. If none, return NO buffer -ENOBUFS
// 	if ( ( rc = iob_ensure_headroom ( iobuf, sizeof ( uint8_t ) * 9 ) ) != 0 )
// 		return rc; 
// 	// Create space for message header, iobuf->data is reused
// 	iob_reserve ( iobuf, sizeof ( uint8_t ) * 9 );
// 	piece = iob_push ( iobuf, sizeof ( uint8_t ) * 9 );
// 	piece->length = 9 + iob_len ( iobuf );
// 	piece->id = 7;
// 	piece->index = index;
// 	piece->begin = begin;

// 	DBG ( "BT sending PIECE to %p", peer );

// 	return xfer_deliver_iob ( &peer->socket, iobuf );
//  Should this buffer bedisowned?
// }

/** Process handshake from peer */
static int bt_rx_handshake ( struct bt_peer *peer, 
									struct bt_handshake *handshake ) {
	
	int i;
	int rc = 0;
	/** Check if info_hash match */
	for ( i = 0; i < 20; i++ ) {
		if ( peer->bt->info_hash[i] != handshake->info_hash[i] )
			return -EBTHM;
	}
	bt_tx_keep_alive ( peer );
	DBG ( "BT keep alive sent!\n" );	
	return rc;
}


/** Open child socket */
static int bt_xfer_open_child ( struct bt_request *bt,
						 		struct interface *child  ) {				 
	
	/** Create new peer */
	struct bt_peer *peer;
	int rc;
	
	peer = bt_create_peer ( bt );
	
	/** Check if peer is successfully allocated. */ 
	if ( ! peer )
		return -ENOMEM;

	/** Plug peer socket and child interface from listening TCP connection. */
	intf_plug_plug ( &peer->socket, child ); 	
	
	/** Add new peer to list of BTpeers */
	list_add ( &peer->list, &bt->peers );
	
	/** Sending message to client. */
	while ( ! xfer_window ( &peer->socket ) ) {
			DBGC ( bt, "BT %p waiting for sufficient window size\n", bt);
	}
	
	assert ( &peer->socket != NULL );
	
	/** For now, we just send a simple message to the peer */
	rc = xfer_printf ( &peer->socket, "Hello client!\r\n" );
	
	// Temporary while we can't process incoming messages
	bt_peer_close( peer, rc );
	
	return rc;
}

/** BitTorrent listening socket interface operations */
static struct interface_operation bt_listener_operations[] = {
		INTF_OP ( xfer_open_child, struct bt_request *, bt_xfer_open_child ),
		INTF_OP ( intf_close, struct bt_request *, bt_close )
};

/** BitTorrent listening socket interface descriptor */
static struct interface_descriptor bt_listener_desc = 
	INTF_DESC_PASSTHRU ( struct bt_request, listener,
						bt_listener_operations, xfer );

/** BitTorrent data transfer interface operations */
static struct interface_operation bt_xfer_operations[] = {
	INTF_OP ( intf_close, struct bt_request *, bt_close )
};

/** BitTorrent data transfer interface descriptor */
static struct interface_descriptor bt_xfer_desc =
	INTF_DESC_PASSTHRU ( struct bt_request, xfer,
						bt_xfer_operations, listener ); 

/**
 * BitTorrent opener
 * 
 */
static int bt_open ( struct interface *xfer, struct uri *uri ) {
	
	struct bt_request *bt;
	struct interface *listener;
	struct downloader *downloader;
	int rc;
	
	/* We can use the uri provided by the function for the tracker address. */ 
	//struct sockaddr_tcpip tracker;
	
	bt = zalloc ( sizeof ( *bt ) );
	if ( ! bt )
		return -ENOMEM;
	
	/* Initialize refcnt of bt. Function bt_free is called when
		refcnt drops to zero. */	
	ref_init ( &bt->refcnt, bt_free );
	
	/* Initialize data and listening interface */
	intf_init ( &bt->xfer, &bt_xfer_desc, &bt->refcnt );
	intf_init ( &bt->listener, &bt_listener_desc, &bt->refcnt );
	
	/** Keep a reference to the downloader for reading pieces */
	downloader = container_of ( xfer, struct downloader, xfer );
	bt->image = downloader->image;
	
	/* Initialize process */
	process_init ( &bt->process, &bt_process_desc, &bt->refcnt );
	
	/* Initialize list of peers */
	INIT_LIST_HEAD ( &bt->peers );
	
	bt->info_hash = bt_str_info_hash ( BT_TEST_HASH );
	bt->peerid = bt_generate_peerid ( );
	
	/* Open tracker socket */
	// Insert tracker communications here.
	// Try contacting the tracker.
	// If tracker is not present, abort.
	// If tracer is present, get list of peers.
	

	/* Open listening connection */
	listener = &bt->listener;
	
	if ( ( rc = xfer_open_named_socket ( listener, SOCK_STREAM, 
										NULL, uri->host, NULL ) ) != 0 )
		goto err;
	
	/* Attach to parent interface, mortalise self, and return */
	intf_plug_plug ( &bt->xfer, xfer );
	ref_put ( &bt->refcnt );
	
	return 0;
	
 err:
 	DBG ( "BT error in creating BT request.\n ");
 	return rc;
	
}

/** BitTorrent URI opener */
struct uri_opener bt_uri_opener __uri_opener = {
	.scheme	= "bt",
	.open	= bt_open,
};
