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

#define TIMED 0 // or 0
#define START 1362055198LL 

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
#include <time.h>

#include <ipxe/malloc.h> 

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



#define BT_FILESIZE 50 * 1024 * 1024
// dsl-4.4.10-initrd.iso 52.7 MB - 66KB 804
//#define BT_TEST_HASH "d73fcc244c629b5f498599a3c478e0f549a7a63e"
// dsl-4.4.10-initrd.iso 52.7 MB - 16KB 3214 pieces 
#define BT_TEST_HASH "0bfd2e8c7b603aec481d6b32296f29aee524207c"
// miku.jpg 1.4MB 
//#define BT_TEST_HASH "4c0e766e8bbe53baa0410a5a698c4b3916224c0f"

FEATURE ( FEATURE_PROTOCOL, "BitTorrent", DHCP_EB_FEATURE_BITTORRENT, 1 );

time_t start;
time_t end;

int num_of_packets_received = 0;
size_t total_bytes_received = 0;  

/** Function prototypes */
static int bt_tx_handshake ();
static int bt_rx_handshake ();
static struct bt_peer * bt_create_peer ();
static int bt_socket_open ();

static int bt_tx_keep_alive ();
static int bt_tx_interested ();
static int bt_tx_request ();
static int bt_tx_have ();
static int bt_tx_piece ();
static void bt_tx_have_to_peers();
// static int bt_tx_cancel ();
static int bt_peer_xmit ();
static uint32_t bt_next_piece ();

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
	intf_shutdown ( &peer->socket, rc );
	
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
	struct bt_message *message = iobuf->data;
	struct bt_handshake *handshake = iobuf->data;
	size_t data_len = iob_len ( iobuf );
	uint32_t index;
	uint32_t begin;
	uint32_t length;

	// global counter for packets
	num_of_packets_received++;
	total_bytes_received+= data_len;

	DBG2 ("BT packets received: %d\n", num_of_packets_received);
	DBG2 ( "BT received buffer length: %zd\n", data_len );

	/** We are still unsure how to handle this */
	while ( iobuf && iob_len ( iobuf ) ) {
		
		data_len = iob_len ( iobuf );
		/** Check in which state is the peer right now. */
		switch ( peer->state ) {
		/** Peer is waiting for handshake */
		case BT_PEER_HANDSHAKE_SENT:
			// Check if message received is a handshake
			if ( handshake->pstrlen == 19 ) {
				DBG ( "BT received HANDSHAKE length: %zd\n", data_len );
				// process received handshake
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
				DBG ( "BT received HANDSHAKE length: %zd\n", data_len );
				// process received handshake then send own
				if ( bt_rx_handshake ( peer, handshake ) == 0) {
					if ( xfer_window ( &peer->socket ) != 0 )
						bt_tx_handshake ( peer );
					else
						DBG ( "BT cannot send HANDSHAKE, no window" );
					peer->state = BT_PEER_HANDSHAKE_RCVD;
					DBG ( "BT peer's info_hash is the same.\n" );
					iob_pull ( iobuf, BT_HANDSHAKELEN );
					if ( bt_tx_interested ( peer ) != 0 ) {
						DBG ( "BT error sending INTERESTED to peer %p\n", peer );
					}
					// [exp] hack
					if ( peer->bt->id == 11 )  {
						peer->bt->state = BT_DOWNLOADING;
						DBG ( "BT state transitioned to BT_DOWNLOADING\n" );
					}

				}	
			}
			break;
		default:
			DBG2 ( "BT receiving iobuf\n" );

			/** Check if we are currently processing a message */
			if ( peer->rx_len == 0 ) {
				// Check buffer length
				DBG2 ( "BT currently processing a message\n" );
				if ( data_len >= BT_PREFIXLEN ) {
					message = iobuf->data;
					peer->rx_len = ntohl ( message->len );
					DBG2 ( "BT prefix length: %zd\n", peer->rx_len );
					if ( peer->rx_len == 0 ) {
						DBG ( "BT KEEP ALIVE received\n" );
						peer->rx_id = 0; // default
					} else {
						//DBG ( "BT message received is OTHER\n" );
						peer->rx_id = message->id;
						peer->remaining = peer->rx_len;
						data_len -= BT_PREFIXLEN;
					}
					iob_pull ( iobuf, BT_PREFIXLEN );
				} else {

					DBG2 ( "BT wait for more bytes\n" );
					return 0; // skip wait for more bytes
				}
			}

			/** Check if there are still remaining bytes from message
			*   and process if complete.
			*/
			if ( peer->remaining && ( peer->remaining <= data_len ) ) {
				DBG2 ( "BT message complete\n" );
				switch ( peer->rx_id ) {
				case BT_CHOKE:
					break;	
				case BT_UNCHOKE:
					DBG ( "BT UNCHOKE received\n" );
					// DBG ( "BT peer window: %zd\n", xfer_window ( &peer->socket ) );
					// while ( xfer_window ( &peer->socket ) <= 0 )
					// 	DBG ( "BT waiting for window\n");
					
					// if ( bt_tx_interested ( peer ) != 0 )
					// 	DBG ( "BT error sending INTERESTED to peer %p\n", peer );

					// for ( i = 0; i < BT_REQUESTS; i++ ) {
					// 	if ( ( rc = bt_tx_request ( peer, i, 0, BT_PIECE_SIZE )) != 0 )
					// 		DBG ( "BT cannot send REQUEST %d to peer %p code (%d)\n", i, peer, rc );
					// }
					// peer->next_piece = 1;
					// peer->pieces_received = 0;

					// DBG ( "BT peer window: %zd\n", xfer_window ( &peer->socket ) );

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
					
					// Remove ID
					iob_pull ( iobuf, 1 );
					// Index
					memcpy ( &index, iobuf->data, 4 );
					iob_pull ( iobuf, 4 );

					DBG ( "BT HAVE %d received\n", ntohl ( index ) );

					if ( peer->id > peer->bt->id ) {
						goto done;
					} // ignore have if we are the seeder

					bitmap_set ( &peer->bitmap,  ntohl ( index ) );

					// if piece is not yet downloaded, append to rem_pieces
					if ( bitmap_test ( &peer->bt->bitmap, ntohl ( index ) ) == 0 ) {
						DBG ( "BT adding rem piece to list\n" );
						struct bt_rem_piece *rem_piece;
						rem_piece = zalloc ( sizeof ( *rem_piece ) );
						rem_piece->index = ntohl ( index );
						list_add_tail ( &rem_piece->list, &peer->bt->rem_pieces );
						bt_tx_request ( peer, bt_next_piece ( peer->bt ), 0, BT_PIECE_SIZE);
					}  else {
						DBG ( "BT piece already downloaded\n" );
					}

					goto done;
					break;
				case BT_REQUEST:

					// Remove ID
					iob_pull ( iobuf, 1 );
					// Index
					memcpy ( &index, iobuf->data, 4 );
					iob_pull ( iobuf, 4 );
					// Begin
					memcpy ( &begin, iobuf->data, 4 );
					iob_pull ( iobuf, 4 );
					// Length
					memcpy ( &length, iobuf->data, 4 ); 
					iob_pull ( iobuf, 4 );

					DBG ( "BT REQUEST %d, %d, %d received\n", ntohl(index), ntohl(begin), ntohl(length) );
					
					//if ( bitmap_test ( &peer->bt->bitmap, ntohl ( index ) ) ) {
					//	goto done; // Do not send any
					//}

					if (! xfer_window (&peer->socket))
						DBG ( "BT cannot send PIECE\n" );

					if ( bt_tx_piece ( peer, ntohl ( index ), ntohl ( begin ) ) != 0 ) {
						DBG ( "BT error sending PIECE\n" );
					}


					goto done;
					break;

				case BT_PIECE:

					peer->pending_requests--;

					memcpy ( iob_put ( peer->rx_buffer, peer->remaining ) , iobuf->data, peer->remaining ); 
					iob_pull ( iobuf, peer->remaining );

					uint8_t id;

					// Remove <id> from piece message
					memcpy ( &id, peer->rx_buffer->data, 1 ); 
					iob_pull ( peer->rx_buffer, 1 );
					// Copy and delete <index> from message
					memcpy ( &index, peer->rx_buffer->data, 4 );
					iob_pull ( peer->rx_buffer, 4 );
					// Copy and delete <begin> from message
					memcpy ( &begin, peer->rx_buffer->data, 4 );
					iob_pull ( peer->rx_buffer, 4 );

					DBG ( "BT %d PIECE %d received\n", id, ntohl ( index ) ) ;

					peer->pieces_received++;
					bitmap_set ( &peer->bt->bitmap, ntohl ( index ) );

					// Deliver piece to upper layer
					struct xfer_metadata meta;
					meta.flags = XFER_FL_ABS_OFFSET;
					meta.offset = ( index * BT_PIECE_SIZE ) + begin;

					// Incorrect code below, rewrite delivery
					// xfer_deliver ( &peer->socket, iob_disown ( peer->rx_buffer ), &meta );  

					iob_pull ( peer->rx_buffer, BT_PIECE_SIZE );
					free_iob ( peer->rx_buffer ); 
					// Reallocate buffer
					peer->rx_buffer = alloc_iob ( BT_PIECE_SIZE + 9 );
					if ( ! peer->rx_buffer ) {
						DBG ( "BT cannot allocate peer %p rx buffer\n", peer ); 
						free_iob ( iobuf );
						return -ENOMEM;
					}

					// Send HAVE to all peers
					bt_tx_have_to_peers ( peer->bt, ntohl ( index ) );

					DBG ( "BT freemem is %zd\n", freemem );
					// Check if all pieces have been downloaded
					if ( bitmap_full ( &peer->bt->bitmap ) ) {
						peer->bt->state = BT_SEEDING;
						printf ( "BT DOWNLOAD SUCCESSFUL! NOW SEEDING!\n" );
						//DBG ( "BT state transitioned to BT_SEEDING\n" );
						//bt_close ( peer->bt, 0 );
						end = time ( NULL );
						printf ( "TFTP ended at %lld\n", end );
						printf ( "TFTP time elapsed %lld\n", end - start );

					} else if ( peer->bt->id == 11) {
						// if ( xfer_window ( &peer->socket) )
						// 	DBG ( "BT window is open.\n" );
						// if ( ! list_empty ( &peer->bt->rem_pieces ) )
						// 	DBG ( "BT rem_pieces list is not empty\n" );
						// while ( xfer_window ( &peer->socket ) &&  ! list_empty ( &peer->bt->rem_pieces ) && peer->pending_requests < BT_MAXREQUESTS ) {
						// 	bt_tx_request ( peer, bt_next_piece ( peer->bt ), 0, BT_PIECE_SIZE );
						// 	peer->pending_requests++;
						// 	assert ( peer->pending_requests <= BT_MAXREQUESTS );
						bt_tx_request ( peer, bt_next_piece ( peer->bt ), 0, BT_PIECE_SIZE );
						//}
					}
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
				peer->remaining = 0;
				peer->rx_len = 0;
				peer->rx_id = 0;

			} else if ( peer->remaining && ( peer->remaining > data_len ) ) {
				// Code only enters here when a piece is being received
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
	free_iob ( iobuf );
	return rc;
}

int first_req = 1;

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
					char uri[] = "tcp://192.168.4.XX:45501";
					// Modify pos 16 and 17
					uri[16] = bt->bt_records[i].id / 10 + 48;
					uri[17] = bt->bt_records[i].id % 10 + 48;

					DBG ( "BT uri[16] = %c\n", bt->bt_records[i].id / 10 + 48 );
					DBG ( "BT uri[17] = %c\n", bt->bt_records[i].id % 10 + 48 );
					DBG ( "BT uri = %s\n", uri );

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
						list_add_tail ( &peer->list, &bt->peers );
						bt->bt_records[i].connected = 1;
					}
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
						DBG ( "BT state transitioned to BT_SENDING_HANDSHAKE\n" );
					}
					return;	
				} 
			}
			break;
		case BT_SENDING_HANDSHAKE:
			// Send handshake to all peers
			{}
			struct bt_peer *peer;
			list_for_each_entry ( peer, &bt->peers, list ) {
				if ( peer->state == BT_PEER_CREATED && xfer_window ( &peer->socket ) ) {
					bt_tx_handshake ( peer );
					peer->state = BT_PEER_HANDSHAKE_SENT;
					DBG ( "BT HANDSHAKE sent to peer %p\n", peer );
				}
			}
			int handshook_all = 1;
			list_for_each_entry ( peer, &bt->peers, list ) {
				if ( peer->state == BT_PEER_HANDSHAKE_SENT ) {
					DBG ( "BT HANDSHAKE already sent to peer %p\n", peer ); 
				} else {
					handshook_all = 0;
					break;
				}
			}
			if ( handshook_all ) {
				bt->state = BT_DOWNLOADING;
				DBG ( "BT state transitioned to BT_DOWNLOADING\n" );
			}

			break;
		case BT_DOWNLOADING:

			if ( bt->id == 11) { 
				struct bt_peer *peer = NULL;
				struct bt_peer *tmp;

				if ( bt_count_peers ( bt ) != 2 ) {
					return;
				}

				list_for_each_entry ( tmp, &bt->peers, list ) {
					DBG ( "BT tmp->id = %d\n", tmp->id );
					if ( tmp->id == bt->id - 1 ) {
						peer = tmp; 
					}
				}
				if ( ! peer ) {
					printf ( "BT not connected to designated peer\n" );
					//process_del ( &bt->process );
					return;
				}

				if ( ! xfer_window ) {
					return;
				} else {

					// Populate rem_pieces list in bt request
					DBG ( "BT populating rem_pieces list\n" );
					int i;
					//INIT_LIST_HEAD ( &bt->rem_pieces );
					for ( i = 0; i < BT_NUMOFPIECES; i++ ) {
						struct bt_rem_piece *rem_piece;
						rem_piece = zalloc ( sizeof ( *rem_piece ) );
						rem_piece->index = i;
						list_add_tail ( &rem_piece->list, &bt->rem_pieces );
					}  

					bt_tx_request ( peer, bt_next_piece ( bt ), 0, BT_PIECE_SIZE); 
					//process_del ( &bt->process );
					bt->state = BT_SEEDING;
				}

			} else if ( bt->id == 10 ) {
				// Check if we have pending pieces to send
				// sleep ( 1 );
				struct bt_peer *peer;
				list_for_each_entry ( peer, &bt->peers, list ) {
				 	bt_peer_xmit ( peer );
				}
			}

			break;
		case BT_SEEDING:
			// Undefined yet
			{}
			struct bt_peer *tmp;
			list_for_each_entry ( tmp, &bt->peers, list ) {
			 	bt_peer_xmit ( tmp );
			}
			break;
		case BT_COMPLETE:
			//bt_close ( bt, 0 ); 
			break;
	}
	return;
}

static size_t bt_xfer_window ( struct bt_request *bt __unused ) {
	return 1;
}

static size_t bt_peer_socket_window ( struct bt_peer *peer __unused ) {
	/* Window is always open.  This is to prevent TCP from
	 * stalling if our parent window is not currently open.
	 */
	return ( ~( ( size_t ) 0 ) );
}

/** BitTorrent peer socket interface operations */
static struct interface_operation bt_peer_operations[] = {
	INTF_OP ( intf_close, struct bt_peer *, bt_peer_close ),
	INTF_OP ( xfer_deliver, struct bt_peer *, bt_peer_socket_deliver ),
	INTF_OP ( xfer_window, struct bt_peer *, bt_peer_socket_window )
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

	INIT_LIST_HEAD ( &peer->queue );

	peer->rx_buffer = alloc_iob ( BT_PIECE_SIZE + 9 );
	if ( ! peer->rx_buffer ) {
		DBG2 ( "BT cannot allocate peer %p rx buffer\n", peer ); 
		return NULL;
	}
	
	/** Add reference to parent request */ 	
	peer->bt = bt;
	ref_get ( &bt->refcnt );
	
	peer->state = BT_PEER_CREATED;
	peer->pieces_received = 0;
	peer->next_piece = 0;
	peer->pending_requests = 0;

	/* Allocate bitmap */
	if ( bitmap_resize ( &peer->bitmap, BT_NUMOFPIECES ) != 0 ) {
		DBG2 ( "BT peer %p could not resize bitmap to %d blocks\n", peer, BT_NUMOFPIECES );
		return NULL;
	}	
		
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
	DBG2 ( "BT sending HANDSHAKE to %p\n", peer );
	return xfer_deliver_raw ( &peer->socket, message, sizeof ( message ) );
}

/** Create KEEP-ALIVE message */
static int bt_tx_keep_alive ( struct bt_peer *peer ) {
	DBG2 ( "BT sending KEEP ALIVE to %p\n", peer );
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
	DBG2 ( "BT sending INTERESTED to %p\n", peer );
	return xfer_deliver_raw ( &peer->socket, message, sizeof ( message ) );
} 

/** Send HAVE message */
static int bt_tx_have ( struct bt_peer *peer, uint32_t index ) {

	uint8_t message[9];
	uint32_t index_n = htonl ( index ) ;
	message[0] = 0;
	message[1] = 0;
	message[2] = 0;
	message[3] = 5; // length = 5
	message[4] = BT_HAVE; // id = 4
	memcpy ( message + 5, &index_n, 4 ); 
	DBG2 ( "BT sending HAVE to %p\n", peer );
	return xfer_deliver_raw ( &peer->socket, message, sizeof ( message ) );
} 

/** Send HAVE to peers 
*	Use this after receiving a piece
*/
static void bt_tx_have_to_peers ( struct bt_request *bt, uint32_t index ) {
	
	struct bt_peer *peer;
	list_for_each_entry ( peer, &bt->peers, list ) {
		bt_tx_have ( peer, index );
	}

	return;
}


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
static int bt_tx_piece ( struct bt_peer *peer, uint32_t index, uint32_t begin __unused) {

	//struct bt_piece *piece;
	struct io_buffer *iobuf;
	void *data;

	uint32_t length_n = htonl ( 9 + BT_PIECE_SIZE );
	uint32_t index_n = htonl ( index );
	//uint32_t begin_n = 0;

	iobuf = xfer_alloc_iob ( &peer->socket, 4 + 9 + BT_PIECE_SIZE);
	if ( ! iobuf )
		return -ENOMEM;

	data = iob_put ( iobuf, 4 ); //length
	memcpy ( data, &length_n, 4 );
	data = iob_put ( iobuf, 1 );
	memset ( data, 7, sizeof ( uint8_t ) );
	data = iob_put ( iobuf, 4 ); //index
	memcpy ( data, &index_n, 4 );
	data = iob_put ( iobuf, 4 ); //begin
	//memcpy ( data, &begin_n, 4 );
	memset ( data, 0, 4 );
	iob_put ( iobuf, BT_PIECE_SIZE ); 

	DBG ( "BT queueing PIECE %08x to %p\n", index, peer );
	DBG2 ( "BT freemem is %zd\n", freemem );
	list_add_tail ( &iobuf->list, &peer->queue );
	return bt_peer_xmit ( peer );
}

static int bt_peer_xmit ( struct bt_peer *peer ) {

	struct io_buffer *iobuf;
	int rc = 0;

	// Check if we have a pending piece and the window is open
	if ( ! list_empty ( &peer->queue ) && xfer_window ( &peer->socket ) ) {
		DBG ( "BT queue is not empty and window is open.\n" );
		iobuf = list_first_entry ( &peer->queue, struct io_buffer, list );
		list_del ( &iobuf->list );
		rc = xfer_deliver_iob ( &peer->socket, iobuf );
		DBG ( "BT sending PIECE from queue to %p\n", peer );
	} else {
		DBG ( "BT peer queue is empty or window is closed\n" );
	}
	return rc;
}

/** Process handshake from peer */
static int bt_rx_handshake ( struct bt_peer *peer, 
									struct bt_handshake *handshake ) {

	DBG ( "BT handshake received\n" );
	DBG ( "BT peer_id = %s", handshake->peer_id );
	int i;
	int rc = 0;
	/** Check if info_hash match */
	for ( i = 0; i < 20; i++ ) {
		if ( peer->bt->info_hash[i] != handshake->info_hash[i] )
			return -EBTHM;
	}
	peer->id = ((handshake->peer_id[0] - 48) * 10) + 
				(handshake->peer_id[1] - 48);	
	return rc;
}

/** Calculate next piece to download */
// static uint32_t bt_next_piece ( struct bt_request *bt ) {
// 	// Generate random number from 0 to size of undownloaded list
// 	int random;
// 	uint32_t index = 0;
// 	int i = 0;
// 	struct bt_rem_piece *rem_piece;
// 	struct bt_rem_piece *tmp;

// 	random = rand() % bt->pieces_left;
// 	list_for_each_entry_safe ( rem_piece, tmp, &bt->rem_pieces, list ) {
// 		if ( random == i ) {
// 			 index = rem_piece->index;
// 			 list_del ( &rem_piece->list );
// 			 free ( rem_piece );
// 			 break;
// 		}
// 		i++;
// 	}
// 	bt->pieces_left--;
// 	return index;
// }

/** Calculate next piece to download */
static uint32_t bt_next_piece ( struct bt_request *bt ) {
	// Generate random number from 0 to size of undownloaded list
	uint32_t index;
	struct bt_rem_piece *rem_piece;

	rem_piece = list_first_entry ( &bt->rem_pieces, struct bt_rem_piece, list );
	index = rem_piece->index;
	list_del ( &rem_piece->list );
	free ( rem_piece ); 

	bt->pieces_left--;
	return index;
}		

/** Open child socket */
static int bt_xfer_open_child ( struct bt_request *bt,
						 		struct interface *child  ) {				 
	
	/** Create new peer */
	struct bt_peer *peer;
	int rc = 0;
	
	peer = bt_create_peer ( bt );

	/** Check if peer is successfully allocated. */ 
	if ( ! peer )
		return -ENOMEM;

	/** Expect to receive a handshake */
	peer->state = BT_PEER_HANDSHAKE_EXPECTED;
	

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
	INTF_OP ( intf_close, struct bt_request *, bt_close ),
	INTF_OP ( xfer_window, struct bt_request *, bt_xfer_window )
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

	start = time ( NULL );

	struct bt_request *bt;
	struct interface *listener;
	struct downloader *downloader;
	int rc;

	char new_uri[] = "192.168.4.XX";
	new_uri[10] = uri->host[0];
	new_uri[11] = uri->host[1];

	printf ( "BT old uri : %s\n", uri->host );
	printf ( "BT new_uri : %s\n", new_uri );

	DBG ( "BT creating bt request\n" );
	
	bt = zalloc ( sizeof ( *bt ) );
	if ( ! bt )
		return -ENOMEM;
	bt->state = BT_CONNECTING_TO_PEERS;
	DBG ( "BT state transitioned to BT_CONNECTING_TO_PEERS\n" );

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

	/** Initialize pieces_left */
	bt->pieces_left = BT_NUMOFPIECES;
	
	/* Initialize process */
	process_init ( &bt->process, &bt_process_desc, &bt->refcnt );
	
	/* Initialize list of peers */
	INIT_LIST_HEAD ( &bt->peers );
	INIT_LIST_HEAD ( &bt->rem_pieces );
	
	bt->info_hash = bt_str_info_hash ( BT_TEST_HASH );
	bt->peerid = bt_generate_peerid ( bt->id );

	/* Allocate bitmap */
	if ( ( rc = bitmap_resize ( &bt->bitmap, BT_NUMOFPIECES ) ) != 0 ) {
		DBG ( "BT %p could not resize bitmap to %d blocks\n", bt, BT_NUMOFPIECES );
		goto err;
	}	
	
	/* Open listening connection */
	listener = &bt->listener;
	
	if ( ( rc = xfer_open_named_socket ( listener, SOCK_STREAM, 
										NULL, new_uri, NULL ) ) != 0 )
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
