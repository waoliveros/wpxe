#ifndef _IPXE_BITTORRENT_H
#define _IPXE_BITTORRENT_H

/** @file
 *
 * BitTorrent protocol
 *
 */

FILE_LICENCE ( GPL2_OR_LATER );

#include <time.h>

/** BitTorrent default port */
#define BITTORRENT_PORT 49155

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

#endif /* _IPXE_BITTORRENT_H */
