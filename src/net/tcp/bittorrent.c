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

#define BITTORRENT_PORT 45501
#define BT_HANDSHAKELEN (1 + 19 + 8 + 20 + 20)

#define BT_NUMOFPIECES 3214
#define BT_FILESIZE 50 * 1024 * 1024
// dsl-4.4.10-initrd.iso 52.7 MB - 66KB 804
//#define BT_TEST_HASH "d73fcc244c629b5f498599a3c478e0f549a7a63e"
// dsl-4.4.10-initrd.iso 52.7 MB - 16KB 3214 pieces 
#define BT_TEST_HASH "0bfd2e8c7b603aec481d6b32296f29aee524207c"
// miku.jpg 1.4MB 
//#define BT_TEST_HASH "4c0e766e8bbe53baa0410a5a698c4b3916224c0f"

FEATURE ( FEATURE_PROTOCOL, "BitTorrent", DHCP_EB_FEATURE_BITTORRENT, 1 );

int num_of_packets_received = 0;  

/** Function prototypes */
static int bt_tx_handshake ();
static int bt_rx_handshake ();
static struct bt_peer * bt_create_peer ();
static int bt_socket_open ();

static int bt_tx_keep_alive ();
static int bt_tx_interested ();
static int bt_tx_request ();
//static int bt_tx_have ();
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
	bitmap_free ( &bt->bitmap ); 
	free ( bt );
};

/**
 * Close BitTorrent request
 */

static void bt_close ( struct bt_request *bt, int rc ) {
	
	DBG ( "BT closing BT request %p code (%d) \n", bt, rc );
	/* Remove process */
	process_del ( &bt->process );
	
	/* Close all data interfaces */
	intf_shutdown ( &bt->xfer, rc );
	intf_shutdown ( &bt->listener, rc );

	/* Iterate over all interfaces then delete */
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
	
	DBG ( "BT closing peer %p code (%d) \n", peer, rc );
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
	//DBG ( "BT counting peers (%d)\n", i );
	return i;
}

/** Handle new data arriving via BitTorrent peer connection
*	There is an assumption here that only PIECE messages are segmented.
*/
static int bt_peer_socket_deliver ( struct bt_peer *peer,
									struct io_buffer *iobuf,
									struct xfer_metadata *meta __unused ) {
	int rc = 0;
	int i = 0;
	struct bt_message *message = iobuf->data;
	struct bt_handshake *handshake = iobuf->data;
	size_t data_len = iob_len ( iobuf );
	uint32_t index;
	uint32_t begin;

	// global counter for packets
	num_of_packets_received++;

	DBG ("BT packets received: %d\n", num_of_packets_received);
	DBG ( "BT received buffer length: %zd\n", data_len );

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
					if ( bt_tx_interested ( peer ) != 0 ) {
						DBG ( "BT error sending INTERESTED to peer %p\n", peer );
					}
					bt_tx_keep_alive ( peer );
				}	
			}
			break;
		case BT_PEER_HANDSHAKE_EXPECTED:
			// Check if message received is a handshake
			if ( handshake->pstrlen == 19 ) {
				DBG ( "BT received handshake length: %zd\n", data_len );
				// process received handshake then send own
				if ( bt_rx_handshake ( peer, handshake ) == 0 ) {
					bt_tx_handshake ( peer );
					peer->state = BT_PEER_HANDSHAKE_RCVD;
					DBG ( "BT peer's info_hash is the same.\n" );
					iob_pull ( iobuf, BT_HANDSHAKELEN );
					if ( bt_tx_interested ( peer ) != 0 ) {
						DBG ( "BT error sending INTERESTED to peer %p\n", peer );
					}
				}	
			}
			break;
		default:
			DBG2 ( "BT receiving iobuf\n" );

			/** Check if we are currently processing a message */
			if ( peer->rx_len == 0 ) {
				// Check buffer length
				if ( data_len >= BT_HEADER ) {
					message = iobuf->data;
					peer->rx_len = ntohl ( message->len );
					peer->rx_id = message->id;
					DBG2 ( "BT prefix length: %zd\n", peer->rx_len );
					if ( peer->rx_len == 0 ) {
						DBG ( "BT KEEP ALIVE received\n" );
						//bt_tx_request ( peer, 17, 0, BT_PIECE_SIZE );
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
					DBG ( "BT peer window: %zd\n", xfer_window ( &peer->socket ) );
					while ( xfer_window ( &peer->socket ) <= 0 )
						DBG ( "BT waiting for window\n");
					
					if ( bt_tx_interested ( peer ) != 0 )
						DBG ( "BT error sending INTERESTED to peer %p\n", peer );

					for ( i = 0; i < BT_REQUESTS; i++ ) {
						if ( ( rc = bt_tx_request ( peer, i, 0, BT_PIECE_SIZE )) != 0 )
							DBG ( "BT cannot send REQUEST %d to peer %p code (%d)\n", i, peer, rc );
					}
					peer->next_piece = 0;
					peer->pieces_received = 0;
					peer->next_piece += BT_REQUESTS;

					DBG ( "BT peer window: %zd\n", xfer_window ( &peer->socket ) );

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

					// Remove <id> from piece message 
					iob_pull ( peer->rx_buffer, 1 );
					// Copy and delete <index> from message
					memcpy ( &index, peer->rx_buffer, 4 );
					iob_pull ( peer->rx_buffer, 4 );
					// Copy and delete <begin> from message
					memcpy ( &begin, peer->rx_buffer, 4 );
					iob_pull ( peer->rx_buffer, 4 );

					DBG ( "BT PIECE %08x received\n", index ) ;

					//DBG ( "BT PIECE %08x received: %zd bytes\n", index, iob_len ( peer->rx_buffer ) );
					peer->pieces_received++;

					// if ( bt_tx_request ( peer, peer->pieces_received, 0,  BT_PIECE_SIZE ) != 0 )
					// 	DBG ( "BT sending REQUEST %d to peer %p\n", peer->pieces_received, peer );

					if (peer->pieces_received % BT_REQUESTS == 0) {
						for ( i = peer->next_piece; i < peer->next_piece+BT_REQUESTS && i < BT_NUMOFPIECES; i++ ) {
							//while ( xfer_window ( &peer->socket ) <= 0 ) 
							if ( ( rc = bt_tx_request ( peer, i, 0, BT_PIECE_SIZE ) ) != 0 ) {
								DBG ( "BT cannot send REQUEST %d to peer %p code (%d)\n", i, peer, rc );
							} else {
								//DBG ( "BT sent REQUEST %d to peer %p\n", i, peer );
							}
						}
					}
					peer->next_piece+=BT_REQUESTS;

					// Deliver piece to upper layer
					struct xfer_metadata meta;
					meta.flags = XFER_FL_ABS_OFFSET;
					meta.offset = ( index * BT_PIECE_SIZE ) + begin;

					// Send HAVE to all peers
					// bt_tx_have_to_peers ( bt );

					// Incorrect code below, rewrite delivery
					// xfer_deliver ( &peer->socket, iob_disown ( peer->rx_buffer ), &meta );  
					
					free ( peer->rx_buffer ); 
					// Reallocate buffer
					peer->rx_buffer = alloc_iob ( BT_PIECE_SIZE + 9 );
					if ( ! peer->rx_buffer ) {
						DBG ( "BT cannot allocate peer %p rx buffer\n", peer ); 
						return -ENOMEM;
					}

					iob_pull ( iobuf, peer->remaining );
					//DBG ( "BT iobuf length: %zd\n", iob_len ( iobuf ) );
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
				DBG2 ( "BT message remaining: %zd\n", peer->remaining );
				DBG2 ( "BT iobuf length:      %zd\n", data_len);
				DBG2 ( "BT rx_buffer length:  %zd\n", iob_len ( peer->rx_buffer) );
				peer->remaining -= data_len;


				memcpy ( iob_put ( peer->rx_buffer, data_len ) , iobuf->data, data_len ); 
				//Remove all data from buffer.
				iob_pull ( iobuf, data_len );
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
	
	int rc = 0;
	int i = 0;

	switch ( bt->state ) {
		case BT_CONNECTING_TO_PEERS:
			for ( i = 0; i < BT_MAXNUMOFPEERS; i++ ) {
				// If ID is legit, not connected, and retries are not maxed out
				if ( bt->bt_records[i].id > 0
					&& bt->bt_records[i].connected == 0 
					&& bt->bt_records[i].retries < BT_MAXRETRIES ) {
					// Connect to peer
					struct bt_peer *peer;
					peer = bt_create_peer ( bt );
					char *uri = "tcp://192.168.4.XX:45501";
					// Modify pos 16 and 17
					uri[16] = bt->bt_records[i].id / 10 + 48;
					uri[17] = bt->bt_records[i].id % 10 + 48;
					peer->uri = parse_uri ( uri );
					ref_init ( &peer->uri->refcnt, NULL );
					uri_get ( peer->uri );
					// Open socket
					if ( ( rc = bt_socket_open ( peer ) ) != 0 ) {
						DBG ( "BT cannot connect to peer ");
						// Remove bt reference from bt_create_peer
						bt_peer_close ( peer, rc );
					} else { 
						DBG ( "BT connected to %s:%s\n", peer->uri->host, peer->uri->port );
						/** If there are no errors, add peer to list */
						list_add ( &peer->list, &bt->peers );
					}
					return;	
				} 
			}
			// Check if all connected
			// if ( connectedtoall ) {
			//	bt->state = BT_DOWNLOADING;
			//}

			int connected_to_all = 1;
			for ( i = 0; i < BT_MAXNUMOFPEERS; i++ ) {
				if ( bt->bt_records[i].id > 0 && bt->bt_records[i].connected == 1 ) {
					continue;
				} else {
					connected_to_all = 0;
					break;
				}
			}
			if ( connected_to_all ) {
				bt->state = BT_SENDING_HANDSHAKE;
			}

			break;
		case BT_SENDING_HANDSHAKE:
			// Send handshake to all peers
			DBG ( "BT sending handshake to all peers\n" );
			struct bt_peer *peer;
			list_for_each_entry ( peer, &bt->peers, list ) {
				if ( peer->state == BT_PEER_CREATED && xfer_window ( &peer->socket ) ) {
					bt_tx_handshake ( peer );
					peer->state = BT_PEER_HANDSHAKE_SENT;
					DBG ( "BT handshake sent to peer %p\n", peer );
				}
			}
			bt_count_peers ( bt );
			break;
		case BT_DOWNLOADING:
			// Continue with download
			break;
		case BT_SEEDING:
			// Undefined yet
			break;
		case BT_COMPLETE:
			break;
	} 
	return;
}

static size_t bt_peer_xfer_window ( struct bt_peer *peer ) {
	/* New block commands may be issued only when we are idle */
	peer = peer;
	return 1;
}

/** BitTorrent peer socket interface operations */
static struct interface_operation bt_peer_operations[] = {
	INTF_OP ( intf_close, struct bt_peer *, bt_peer_close ),
	INTF_OP ( xfer_deliver, struct bt_peer *, bt_peer_socket_deliver ),
	INTF_OP ( xfer_window, struct bt_peer *, bt_peer_xfer_window )
};

/** BitTorrent peer socket interface descriptor */
static struct interface_descriptor bt_peer_desc =
	INTF_DESC ( struct bt_peer, socket, bt_peer_operations );
	
/** BitTorrent process descriptor */	
static struct process_descriptor bt_process_desc =
	PROC_DESC_ONCE ( struct bt_request, process, bt_step );

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

	peer->rx_buffer = alloc_iob ( BT_PIECE_SIZE + 9 );
	if ( ! peer->rx_buffer ) {
		DBG ( "BT cannot allocate peer %p rx buffer\n", peer ); 
		return NULL;
	}
	
	/** Add reference to parent request */ 	
	peer->bt = bt;
	ref_get ( &bt->refcnt );
	
	peer->state = BT_PEER_CREATED;

	peer->pieces_received = 0;
	peer->next_piece = 0;
		
	/** Initialize peer refcnt. Function bt_peer_free is called when counter
		drops to zero. Initialize socket with descriptor. Increment peer
		reference counter. */	
	ref_init ( &peer->refcnt, bt_peer_free );
	intf_init ( &peer->socket, &bt_peer_desc, &peer->refcnt );	

	DBG ( "BT peer created\n" );

	return peer;
}

/** Establish a connection with another peer */
static int bt_socket_open (struct bt_peer *peer) {
	struct uri *uri = peer->uri;
	struct sockaddr_tcpip server;
	struct interface *socket;
	int rc = 0;

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
	return 0;
err:
	bt_peer_close ( peer, rc );
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

/** Send HAVE message */
// static int bt_tx_have ( struct bt_peer *peer, uint32_t index ) {

// 	uint8_t message[9];
// 	uint32_t index_n = htonl ( index ) ;
// 	message[0] = 0;
// 	message[1] = 0;
// 	message[2] = 0;
// 	message[3] = 5; // length = 5
// 	message[4] = 4; // id = 4
// 	memcpy ( message + 5, &index_n, 4 ); 
// 	DBG ( "BT sending HAVE to %p", peer );
// 	return xfer_deliver_raw ( &peer->socket, message, sizeof ( message ) );
// } 

/** Send HAVE to peers 
*	Use this after receiving a piece
*/
// static void bt_tx_have_to_peers ( struct bt_request *bt ) {
// 	bt = bt;
// 	return;
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
	message[3] = 13; // length
	message[4] = 6; // id
	memcpy ( message + 5, &index_n, 4 );
	memcpy ( message + 9, &begin_n, 4 );
	memcpy ( message + 13, &length_n, 4 );
	DBG ( "BT sending REQUEST %d to %p\n", index, peer );
	return xfer_deliver_raw ( &peer->socket, message, sizeof ( message ) );
}

/** Send CANCEL message */
// static int bt_tx_cancel ( struct bt_peer *peer, uint32_t index, uint32_t begin, uint32_t length ) {

// 	uint8_t message[17];
// 	uint32_t index_n = htonl ( index ) ;
// 	uint32_t begin_n = htonl ( begin ) ;
// 	uint32_t length_n = htonl ( length ) ;
// 	message[0] = 0;
// 	message[1] = 0;
// 	message[2] = 0;
// 	message[3] = 13;
// 	message[4] = 8;
// 	memcpy ( message + 5, &index_n, 4 );
// 	memcpy ( message + 9, &begin_n, 4 );
// 	memcpy ( message + 13, &length_n, 4 );
// 	DBG2 ( "BT sending CANCEL %d to %p", index, peer );
// 	return xfer_deliver_raw ( &peer->socket, message, sizeof ( message ) );
// }

/** Send PIECE message */
// static int bt_tx_piece ( struct bt_peer *peer, uint32_t index, uint32_t begin, struct io_buffer *iobuf ) {

// 	struct bt_piece *piece;
	
// 	// // Ensure headroom. If none, return NO buffer -ENOBUFS
// 	// if ( ( rc = iob_ensure_headroom ( iobuf, sizeof ( uint8_t ) * 9 ) ) != 0 )
// 	// 	return rc; 
// 	// // Create space for message header, iobuf->data is reused
// 	// iob_reserve ( iobuf, sizeof ( uint8_t ) * 9 );
// 	// piece = iob_push ( iobuf, sizeof ( uint8_t ) * 9 );
// 	// piece->length = 9 + iob_len ( iobuf );
// 	// piece->id = 7;
// 	// piece->index = index;
// 	// piece->begin = begin;

// 	piece = zalloc ( sizeof ( *piece ) );
// 	if ( !piece ) {
// 		return -ENOMEM;
// 	}
// 	piece->len = htonl ( 9 + BT_PIECE_SIZE ) ;
// 	piece->id = 8;
// 	piece->index = htonl ( index );
// 	piece->begin = htonl ( begin );
// 	piece->block = iobuf->data;

// 	DBG ( "BT sending PIECE %d to %p", index, peer );
// 	// rewrite to use xfer_alloc_iob to eliminate copying
// 	return xfer_deliver_raw ( &peer->socket, piece, 4 + 9 + BT_PIECE_SIZE );
// }

/** Process handshake from peer */
static int bt_rx_handshake ( struct bt_peer *peer, 
									struct bt_handshake *handshake ) {
	DBG ( "BT handshake received\n" );
	int i;
	int rc = 0;
	/** Check if info_hash match */
	for ( i = 0; i < 20; i++ ) {
		if ( peer->bt->info_hash[i] != handshake->info_hash[i] )
			return -EBTHM;
	}	
	return rc;
}

/** Open child socket */
static int bt_xfer_open_child ( struct bt_request *bt,
						 		struct interface *child  ) {				 
	
	/** Create new peer */
	struct bt_peer *peer;
	int rc = 0;
	
	peer = bt_create_peer ( bt );

	/** Expect to receive a handshake */
	peer->state = BT_PEER_HANDSHAKE_EXPECTED;
	
	/** Check if peer is successfully allocated. */ 
	if ( ! peer )
		return -ENOMEM;

	/** Plug peer socket and child interface from listening TCP connection. */
	intf_plug_plug ( &peer->socket, child ); 	
	
	assert ( &peer->socket != NULL );

	/** Add new peer to list of BTpeers */
	list_add ( &peer->list, &bt->peers );

	DBG ( "BT remote peer connected\n" );

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
 * HACK: We use the uri->host value passed to give ID to our client
 * The peers to connect to will be computed from this host
 */
static int bt_open ( struct interface *xfer, struct uri *uri ) {
	
	struct bt_request *bt;
	struct interface *listener;
	struct downloader *downloader;
	int rc;
	
	DBG ( "BT creating bt request\n" );
	
	bt = zalloc ( sizeof ( *bt ) );
	if ( ! bt )
		return -ENOMEM;
	bt->state = BT_CONNECTING_TO_PEERS;
	bt->id = ( int ) strtoul ( uri->host, NULL, 10 );
	DBG ( "BT this client's ID is \"%d\"\n", bt->id );
	bt_compute_records ( bt );
	
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

	/* Allocate bitmap */
	if ( ( rc = bitmap_resize ( &bt->bitmap, BT_NUMOFPIECES ) ) != 0 ) {
		DBG ( "BT %p could not resize bitmap to %d blocks\n", bt, BT_NUMOFPIECES );
		goto err;
	}	
	
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
 	bt_close ( bt, rc );
 	ref_put ( &bt->refcnt );
 	return rc;
	
}

/** BitTorrent URI opener */
struct uri_opener bt_uri_opener __uri_opener = {
	.scheme	= "bt",
	.open	= bt_open,
};
