#ifndef _IPXE_BITTORRENT_H
#define _IPXE_BITTORRENT_H

/** @file
 *
 * BitTorrent protocol
 *
 */

FILE_LICENCE ( GPL2_OR_LATER );

#include <time.h>
#include <ipxe/bitmap.h>

// Experimental parameters
#define BT_MAXRETRIES 5
#define BT_NUMOFNODES 2
#define BT_MAXNUMOFPEERS 1
#define BT_REQUESTS 5
#define BT_PIECE_SIZE 16 * 1024

#define BT_PREFIXLEN 4
#define BT_HEADER 5

#define BT_CHOKE 0
#define BT_UNCHOKE 1
#define BT_INTERESTED 2
#define BT_NOTINTERESTED 3
#define BT_BITFIELD 4
#define BT_HAVE 5
#define BT_REQUEST 6
#define BT_PIECE 7
#define BT_CANCEL 8
#define BT_PORT 9


/** Record of peers to connect. For experimetns only. */
struct bt_record {
	int id;
	int retries;
	int connected;
};

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
	
	/** This bt client's peer id */
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

	/** Piece Bitmap */
	struct bitmap bitmap;

	/**  
	* This node's id, only used for experiments
	**/
	int id;

	/** 
	* Where are we in the download process?
	*/
	int state;

	/**
	*
	*/
	struct bt_record bt_records[BT_MAXNUMOFPEERS];
	
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
	
	/** Peer id */
	uint8_t peerid[20];
	
	/** Port to be used in BitTorrent operations */
	unsigned int port;
	
	/** State */
	int state;
	
	/** Flags */
	unsigned int flags;

	/** Length received */
	size_t rx_len;

	/** Message id received */
	uint8_t rx_id;

	/** RX Buffer */
	struct io_buffer *rx_buffer;

	/** Remaining length */
	size_t remaining;

	/** List of buffers */
	struct list_head buffers;

	/** Num of pieces received from this peer */
	int pieces_received;

	/** Index of next piece to download **/
	int next_piece;

};

struct bt_message {
	uint32_t len;
	uint8_t id;
	void *payload;
};

struct bt_handshake {
	uint8_t pstrlen;
	uint8_t pstr[19];
	uint8_t reserved[8];
	uint8_t info_hash[20];
	uint8_t peer_id[20];
};

struct bt_piece_request {
	uint32_t len;
	uint8_t id;
	/* Zero-based piece index */
	uint32_t index;
	/* Zero-based byte offset within the piece */
	uint32_t begin;	
	/* Requested length */
	uint32_t length;
};

struct bt_piece {
	uint32_t len;
	uint8_t id;
	uint32_t index;
	uint32_t begin;
	void *block;
};

enum bt_peer_state {
	BT_PEER_CREATED = 0,
	BT_PEER_HANDSHAKE_SENT,
	BT_PEER_HANDSHAKE_RCVD,
	BT_PEER_LEECHING,
	BT_PEER_SEEDING,
	BT_PEER_HANDSHAKE_EXPECTED
};

enum bt_state {
	BT_CONNECTING_TO_PEERS = 0,
	BT_SENDING_HANDSHAKE,
	BT_DOWNLOADING,
	BT_SEEDING,
	BT_COMPLETE
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
};

extern int bt_open_filter ( struct interface *xfer, struct uri *uri,
			      unsigned int default_port,
			      int ( * filter ) ( struct interface *,
						 struct interface ** ) );
						
static uint8_t * bt_generate_peerid ( ) {
	uint8_t * peer_id;
	int i = 7;
	
	peer_id = zalloc ( sizeof ( uint8_t ) * 20 );
	
	srandom ( time_now ( ) );
	peer_id[0] = '-';
	peer_id[1] = 'i';
	peer_id[2] = 'P';
	peer_id[3] = '1';
	peer_id[4] = '0';
	peer_id[5] = '0';
	peer_id[6] = '0';
	
	while ( i < 19 ) {
		peer_id[i] = ( random ( ) % 10 ) + 48;
		i++;
	}
	
	peer_id[19] = '-';
	
	printf ( "Peer ID generated: " );
	for ( i = 0; i < 20; i++ )
		printf ( "%c", peer_id[i] );
	printf ( "\n" );
	return peer_id;
	
}

static uint8_t * bt_str_info_hash ( char * str ) {
	uint8_t * info_hash;
	int i = 0;
	
	info_hash = zalloc ( sizeof ( uint8_t ) * 20 );
	for ( i = 0; i < 20; i++ ) {
		
		// Higher value
		/** If char is numeric */
		if ( 48 <= str[i*2] && str[i*2] <= 57 ) {
			info_hash[i] = ( str[i*2] - 48 ) * 16;
		} else { 
			info_hash[i] = ( str[i*2] - 87 ) * 16;
		}
			
		// Lower value	
		/** If char is numeric */
		if ( 48 <= str[i*2+1] && str[i*2+1] <= 57 ) {
			info_hash[i] += ( str[i*2+1] - 48 );
		} else { 
			info_hash[i] += ( str[i*2+1] - 87 );
		}			
	}
	
	for ( i = 0; i < 20; i++ ) {
		printf ("%x", info_hash[i]);
	}
	
	return info_hash;
	
}

static void bt_compute_records ( struct bt_request * bt ) {
	int i; 

	DBG ( "BT compute peer IPs and record\n" );
	for ( i = 0; i < BT_MAXNUMOFPEERS; i++ ) {
		bt->bt_records[i].retries = 0;
		bt->bt_records[i].connected = 0;
		if ( bt->id + i + 1 >= BT_NUMOFNODES + 10) {
			bt->bt_records[i].id = 0;
		} else {
			bt->bt_records[i].id = bt->id + i + 1;
		}
	}
}					

#endif /* _IPXE_BITTORRENT_H */
