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
#define BT_TEST_HASH "f06f95fa2f7eb05455cb4538880634534384163d"

FEATURE ( FEATURE_PROTOCOL, "BitTorrent", DHCP_EB_FEATURE_BITTORRENT, 1 );

/** Function prototypes */
static int bt_peer_tx_handshake ();
static int bt_peer_rx_handshake ();
static struct bt_peer * bt_create_peer ();
static int bt_socket_open ();

/** Hack variables */
//static int has_peers = 0;

/**
 * A BitTorrent client
 *
 * This data structure holds the state for an ongoing BitTorrent client operation.
 */

struct bt_request {
	/** Reference count */
	struct refcnt refcnt;
	/** Data transfer interface */
	struct interface xfer;

	/** Current position within image buffer */
	size_t pos;

	/** Tracker response */
	uint8_t * raw_response ;

	/** Bencoded tracker response */
	be_node * ben_response;

	/** Parsed response */
	struct t_response * response;

	/** Server socket of this peer **/
	struct interface listener;	
	
	/** TX process */
	struct process process;
	
	/** The current torrent info hash */
	uint8_t * info_hash; 
	
	/** This client's peer id */
	uint8_t * peerid;
	
	/** The pointer to the downloader image.
	*	This pointer is useful for reading partially downloaded pieces
	*	for sharing to other peers. A separate bitmap must be maintained to
	*	keep track of the downloaded pieces. Pieces must be successfully written
	*	first to memory before getting logged downloaded at the bitmap.
	*/
	struct image *image;
	
	/**
	* List of registered peers
	*/
	struct list_head peers;
	
};



/**
 * A BitTorrent peer
 *
 * This data structure holds the state of one of this client's peers.
 */

struct bt_peer {
	
	/** Reference count */
	struct refcnt refcnt;

	/** Parent request */
	struct bt_request *bt;

	/** List of BitTorrent peers */
	struct list_head list;

	/** Socket interface */
	struct interface socket;
	
	/** Address of the peer */
	struct uri *uri;
	
	/** Port to be used in BitTorrent operations */
	unsigned int port;
	
	/** State */
	int state;
	
	/** Flags */
	unsigned int flags;

};

struct bt_message {
	uint32_t len;
	uint8_t  id;
	uint8_t *payload;
};

struct bt_block_request {

	/* Zero-based piece index */
	int index;
	
	/* Zero-based byte offset within the piece */
	int begin;
	
	/* Requested length */
	int length;
};

enum bt_peer_state {
	BT_PEER_CREATED = 0,
	BT_PEER_HANDSHAKE_SENT,
	BT_PEER_HANDSHAKE_RCVD,
	BT_PEER_LEECHING,
	BT_PEER_SEEDING
};

enum bt_message_id {
	BT_CHOKE = 0,
	BT_UNCHOKE,
	BT_INTERESTED,
	BT_NOT_INTERESTED,
	BT_HAVE,
	BT_BITFIELD,
	BT_REQUEST,
	BT_PIECE,
	BT_CANCEL,
	BT_PORT
};

enum bt_peer_flags {
	/** This client is choking the peer */
	BT_PEER_AM_CHOKING =	0x01,
	/** This client is interested in the peer */
	BT_PEER_AM_INTERESTED =	0x02,
	/** The peer is choking this client */
	BT_PEER_CHOKING = 		0x04,
	/** The peer is interested in this client */
	BT_PEER_INTERESTED =	0x08,
	
//	BT_PEER_HANDSHAKE_SENT = 0x10,
//	BT_PEER_HANDSHAKE_RECEIVED = 0x20
	
};

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

/** Close the peer connection */
static void bt_peer_close ( struct bt_peer *peer, int rc ) {
	list_del( &peer->list );
	intf_shutdown ( &peer->socket, rc);
	
	/** Reference to bt->peer_info and bt->peerid is no longer needed */
	ref_put ( &peer->bt->refcnt );
	
	bt_peer_free ( &peer->refcnt );
}

/** BitTorrent peer message transmit */
static int bt_peer_xmit ( struct bt_peer *peer, uint8_t *message, size_t len ) {
	int rc;
	rc = xfer_deliver_raw ( &peer->socket, message, len );
	printf ( "Peer message transmitted.\n" );
	return rc;
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

/** Handle new data arriving via BitTorrent peer connection */
static int bt_peer_socket_deliver ( struct bt_peer *peer,
									struct io_buffer *iobuf,
									struct xfer_metadata *meta __unused ) {
	int rc = 0;
	size_t data_len;
	struct bt_message *message = iobuf->data;
	
	/** Sanity check of the message */
	data_len = sizeof ( message->id ) + sizeof ( message->payload );
	assert ( message->len == data_len );
	printf ( "Message length:\t\t %d\n", message->len );
	printf ( "Message length computed:\t %zd\n\n", data_len );							 
	
	/** Check in which state is the peer right now. */
	switch ( peer->state ) {
		/** Peer is waiting for handshake */
		case BT_PEER_HANDSHAKE_SENT:
			 // process received handshake then send own
			 bt_peer_rx_handshake ( peer );
			 peer->state = BT_PEER_HANDSHAKE_RCVD;
			 break;
		case BT_PEER_HANDSHAKE_RCVD:
			// handshake sent to peer, waiting for ack
			// send peer handshake	 
		default:
			printf ( "Some data received from TCP.\n" );
			break;			
	}
	
	while ( iobuf && iob_len ( iobuf ) ) {
		
	}
	
	return rc;
}

/** BitTorrent process */
static void bt_step ( struct bt_request *bt ) {
	
	
	
	if ( bt )
	//	printf("*");
		
	//if ( ! xfer_window ( &bt->listener ) )
	//	return;	
	
	/** If there are no peers get list from tracker */
	if ( ! bt_count_peers ( bt ) ) {
		
		/** The following is just a test scenario
		 Simulated tracker comm. Populate list 
		 Contact peers in list. Send handshake. */
		struct bt_peer *peer;
		peer = bt_create_peer ( bt );
		peer->uri = parse_uri ( "tcp://192.168.4.1:50986" );
		ref_init ( &peer->uri->refcnt, NULL );
		uri_get ( peer->uri );	
		if ( ! bt_socket_open ( peer ) )
			printf ( "Test socket opened!\n" );
		
		
		
		/** Initialize peer URI */
		//peer->uri = 
		
		/** For now, let's manually set the info hash */
		//bt->info_hash = BT_DEF_INFO_HASH;
		
		//while ( ! xfer_window ( &peer->socket ) )
		//	sleep ( 1 );
		
		//bt_peer_tx_handshake ( peer );
		
		//printf ( "Handshake sent!\n" );
		
		/** Test scenario ends here */
		return;	
	}
	
	//printf ( "%d\n", bt_count_peers ( bt ) );
	struct bt_peer *peer;
	list_for_each_entry ( peer, &bt->peers, list ) {
		//printf ( "+" );
		if ( xfer_window ( &peer->socket ) && peer->state == BT_PEER_CREATED ) {
			bt_peer_tx_handshake ( peer );
			peer->state = BT_PEER_HANDSHAKE_SENT;
			printf ( "BT Handshake sent!\n" );
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
	// bt_peer_tx_handshake ()
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

	peer = bt_create_peer ( peer->bt );
	if ( !peer )
		return -ENOMEM;
	

	/* Open socket */
	memset ( &server, 0, sizeof ( server ) );
	server.st_port = htons ( uri_port ( uri, 80 ) );
	socket = &peer->socket;
	
	printf ( "Opening socket!\n" );
	printf ( "URI Scheme: %s\n", uri->scheme );
	printf ( "URI Host: %s\n", uri->host );
	printf ( "URI Port: %s\n", uri->port );
	
	if ( ( rc = xfer_open_named_socket ( socket, SOCK_STREAM,
					     ( struct sockaddr * ) &server,
					     uri->host, NULL ) ) != 0 )
		return rc; 
	
	/** If there are no errors, add peer to list */
	list_add ( &peer->list, &peer->bt->peers );
	
	return 0;
}

/** Do handshake with a peer */ 
static int bt_peer_tx_handshake ( struct bt_peer *peer ) {
	int rc = 0;
	uint8_t *message;
	
	assert ( peer != NULL );
	//assert ( bt != NULL );
	
	message = zalloc ( BT_HANDSHAKELEN );
	message[0] = 19;
	memcpy(message + 1, "BitTorrent protocol", 19);
	/* From unworkable: set reserved bit to indicate we support the fast extension */
	//msg[27] |= 0x04;
	memcpy(message + 28, peer->bt->info_hash, 20);
	memcpy(message + 48, peer->bt->peerid, 20); 
	
	bt_peer_xmit ( peer, message, BT_HANDSHAKELEN );
	
	int i = 1;
	
	printf ( "BT Message: " );
	while ( i < BT_HANDSHAKELEN ) {
		printf (  "%c", message[i] );
		i++; 
	}
	printf ( "\n" );
	
	return rc;
}

/** Process handshake from peer */
static int bt_peer_rx_handshake ( uint8_t *message ) {
	return message[0];
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
	
	return 0;
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
 	printf ( "Error in creating BT request.\n ");
 	return rc;
	
}

/** BitTorrent URI opener */
struct uri_opener bt_uri_opener __uri_opener = {
	.scheme	= "bt",
	.open	= bt_open,
};
