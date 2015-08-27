/*
Serval Low-bandwidth asychronous Rhizome Demonstrator.
Copyright (C) 2015 Serval Project Inc.

This program monitors a local Rhizome database and attempts
to synchronise it over low-bandwidth declarative transports, 
such as bluetooth name or wifi-direct service information
messages.  It is intended to give a high priority to MeshMS
converations among nearby nodes.

The design is fully asynchronous, so a call to the update_my_message()
function from time to time should be all that is required.


This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
*/

#include <stdio.h>
#include <time.h>
#include <unistd.h>
#include <stdlib.h>
#include <strings.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <curl/curl.h>
#include <dirent.h>
#include <assert.h>

#include "lbard.h"

int bundle_bar_counter=0;

int append_bar(int bundle_number,int *offset,int mtu,unsigned char *msg_out)
{
  // BAR consists of:
  // 8 bytes : BID prefix
  // 8 bytes : version
  // 4 bytes : recipient prefix

  for(int i=0;i<8;i++)
    msg_out[(*offset)++]=hex_byte_value(&bundles[bundle_number].bid[i*2]);
  for(int i=0;i<8;i++)
    msg_out[(*offset)++]=(bundles[bundle_number].version>>(i*8))&0xff;
  for(int i=0;i<4;i++)
    msg_out[(*offset)++]=hex_byte_value(&bundles[bundle_number].recipient[i*2]);
  
  return 0;
}

int announce_bundle_piece(int bundle_number,int *offset,int mtu,unsigned char *msg,
			  char *prefix,char *servald_server, char *credential)
{
  fprintf(stderr,"Preparing to announce a piece of bundle #%d\n",
	  bundle_number);

  if (prime_bundle_cache(bundle_number,
			 prefix,servald_server,credential)) return -1;

  /*
    We need to prefix any piece with the BID prefix, BID version, the offset
    of the content, which will also include a flag to indicate if it is content
    from the manifest or body.  This entails the following:
    Piece header - 1 byte
    BID prefix - 8 bytes
    bundle version - 8 bytes
    offset & manifest/body flag & length - 4 bytes
    (20 bits length, 1 bit manifest/body flag, 11 bits length)

    Total = 21 bytes.

    If start_offset>0xfffff, then 2 extra bytes are used for the upper bytes of the
    starting offset.    

  */

  int max_bytes=mtu-(*offset)-21;
  assert(max_bytes>0);
  int is_manifest=0;
  unsigned char *p=NULL;
  int actual_bytes=0;
  int bytes_available=0;
  int start_offset=0;
  
  // For journaled bundles, update the first body announced offset to begin at the
  // first byte that the recipient (if they are a peer) has received.
  // If the recipient is not a peer, then we send from the first byte that any of
  // our peers has yet to receive.
  // By checking this each time we send a piece, we will automatically skip bytes
  // that we have just heard about a peer having received.

  // Is the bundle a journalled bundle?
  if (cached_version<0x100000000LL) {
    long long first_byte=0;
    int j;
    for(j=0;j<peer_count;j++) {
      if (!strncmp(bundles[bundle_number].recipient,
		   peer_records[j]->sid_prefix,(8*2))) {
	// Bundle is address to a peer.
	int k;
	for(k=0;k<peer_records[j]->bundle_count;k++) {
	  if (!strncmp(peer_records[j]->bid_prefixes[k],bundles[bundle_number].bid,
		       8*2)) {
	    // Peer knows about this bundle, but which version?
	    first_byte=peer_records[j]->versions[k];
	  }
	}
      }
    }
    if (!first_byte) {
      // Recipient has no bytes or is not a peer, so now do the overall scan to see
      // if all our peers have at least some bytes, and if so skip them.
      first_byte=cached_body_len;
      for(j=0;j<peer_count;j++) {
	int k;
	for(k=0;k<peer_records[j]->bundle_count;k++) {
	  if (!strncmp(peer_records[j]->bid_prefixes[k],bundles[bundle_number].bid,
		       8*2)) {
	    // Peer knows about this bundle, but which version?
	    if (peer_records[j]->versions[k]<first_byte)
	      first_byte=peer_records[j]->versions[k];
	    break;
	  }
	}
	if (k==peer_records[j]->bundle_count) {
	  // Peer does not have this bundle, so we must start from the beginning.
	  first_byte=0;
	}	
      }
    }
    if (bundles[bundle_number].last_offset_announced<first_byte) {
      fprintf(stderr,"Skipping from byte %lld straight to %lld, because recipient or all peers have the intervening bytes\n",
	      bundles[bundle_number].last_offset_announced,first_byte);
      bundles[bundle_number].last_offset_announced=first_byte;
    }
  }

  int end_of_item=0;
  
  if (bundles[bundle_number].last_manifest_offset_announced<cached_manifest_len) {
    // Send some manifest
    bytes_available=cached_manifest_len-
      bundles[bundle_number].last_manifest_offset_announced;
    is_manifest=1;
    start_offset=bundles[bundle_number].last_manifest_offset_announced;
    p=&cached_manifest[bundles[bundle_number].last_manifest_offset_announced];
  } else if (bundles[bundle_number].last_offset_announced<cached_body_len) {
    // Send some body
    bytes_available=cached_body_len-
      bundles[bundle_number].last_offset_announced;
    p=&cached_body[bundles[bundle_number].last_offset_announced];    
    start_offset=bundles[bundle_number].last_offset_announced;
  }

  // If we can't announce even one byte, we should just give up.
  if (start_offset>0xfffff) {
    max_bytes-=2; if (max_bytes<0) max_bytes=0;
  }
  if (max_bytes<1) return -1;

  // Work out number of bytes to include in announcement
  if (bytes_available<max_bytes) {
    actual_bytes=bytes_available;
    end_of_item=0;
  } else {
    actual_bytes=max_bytes;
    end_of_item=1;
  }
  // Make sure byte count fits in 11 bits.
  if (actual_bytes>0x7ff) actual_bytes=0x7ff;

  // Generate 4 byte offset block (and option 2-byte extension for big bundles)
  long long offset_compound=0;
  offset_compound=(start_offset&0xfffff);
  offset_compound|=((actual_bytes&0x7ff)<<20);
  if (is_manifest) offset_compound|=0x80000000;
  offset_compound|=((start_offset>>20LL)&0xffffLL)<<32LL;

  // Now write the 21/23 byte header and actual bytes into output message
  // BID prefix (8 bytes)
  if (start_offset>0xfffff)
    msg[(*offset)++]='P'+end_of_item;
  else 
    msg[(*offset)++]='p'+end_of_item;
  
  
  for(int i=0;i<8;i++)
    msg[(*offset)++]=hex_byte_value(&bundles[bundle_number].bid[i*2]);
  // Bundle version (8 bytes)
  for(int i=0;i<8;i++)
    msg[(*offset)++]=(cached_version>>(i*8))&0xff;
  // offset_compound (4 bytes)
  for(int i=0;i<4;i++)
    msg[(*offset)++]=(offset_compound>>(i*8))&0xff;
  if (start_offset>0xfffff) {
  for(int i=4;i<6;i++)
    msg[(*offset)++]=(offset_compound>>(i*8))&0xff;
  }

  bcopy(p,&msg[(*offset)],actual_bytes);
  (*offset)+=actual_bytes;

  // Update offset announced
  if (is_manifest) {
    bundles[bundle_number].last_manifest_offset_announced+=actual_bytes;
  } else {
    bundles[bundle_number].last_offset_announced+=actual_bytes;

    if (bundles[bundle_number].last_offset_announced==cached_body_len) {
      // If we have reached the end, then mark this bundle as having been announced.
      bundles[bundle_number].last_announced_time=time(0);
      // XXX - Race condition exists where bundle version could be updated while we
      // are announcing it.  By caching the version number, we reduce, but do not
      // eliminate this risk, but at least the recipient will realise if they are being
      // given a mix of pieces.
      bundles[bundle_number].last_version_of_manifest_announced=
	cached_version;

      // Then reset offsets for announcing next time
      bundles[bundle_number].last_offset_announced=0;
      bundles[bundle_number].last_manifest_offset_announced=0;
    }

  }
  
  return 0;

}

int message_counter=0;
int update_my_message(char *my_sid, int mtu,unsigned char *msg_out,
		      char *servald_server,char *credential)
{
  /* There are a few possible options here.
     1. We have no peers. In which case, there is little point doing anything.
        EXCEPT that some people might be able to hear us, even though we can't
	hear them.  So we should walk through a prioritised ordering of some subset
	of bundles, presenting them in turn via the interface.
     2. We have peers, but we have no content addressed to them, that we have not
        already communicated to them.
        In which case, we act as for (1) above.
     3. We have peers, and have bundles addressed to one or more of them, and have
        not yet heard from those peers that they already have those bundles. In which
        case we should walk through presenting those bundles repeatedly until the
        peers acknowledge the receipt of those bundles.

	Thus we need to keep track of the bundles that other peers have, and that have
	been announced to us.

	We also need to take care to announce MeshMS bundles, and especially new and
	updated MeshMS bundles that are addressed to our peers so that MeshMS has
	minimal latency over the transport.  In other words, we don't want to have to
	wait hours for the MeshMS bundle in question to get announced.

	Thus we need some sense of "last announcement time" for bundles so that we can
	prioritise them.  This should be kept with the bundle record.  Then we can
	simply lookup the highest priority bundle, see where we got to in announcing
	it, and announce the next piece of it.

	We should probably also announce a BAR or two, so that peers know if we have
	received bundles that they are currently sending.  Similarly, if we see a BAR
	from a peer for a bundle that they have already received, we should reduce the
	priority of sending that bundle, in particular if the bundle is addressed to
	them, i.e., we have confirmation that the recipient has received the bundle.

	Finally, we should use network coding so that recipients can miss some messages
	without terribly upsetting the whole thing, unless the transport is known to be
	reliable.
  */

  // Build output message

  if (mtu<64) return -1;
  
  // Clear message
  bzero(msg_out,mtu);

  // Put prefix of our SID in first 6 bytes.
  char prefix[7];
  for(int i=0;i<6;i++) { msg_out[i]=my_sid[i]; prefix[i]=my_sid[i]; }
  prefix[6]=0;
  
  // Put 2-byte message counter.
  // lower 15 bits is message counter.
  // the 16th bit indicates if this message is a retransmission
  // (always clear when constructing the message).
  msg_out[6]=message_counter&0xff;
  msg_out[7]=(message_counter>>8)&0x7f;

  int offset=8;
  
  // Put one or more BARs
  bundle_bar_counter++;
  if (bundle_bar_counter>=bundle_count) bundle_bar_counter=0;
  if (bundle_count&&((mtu-offset)>=BAR_LENGTH)) {
    msg_out[offset++]='B'; // indicates a BAR follows
    append_bar(bundle_bar_counter,&offset,mtu,msg_out);
  }

  // Announce a bundle, if any are due.
  int bundle_to_announce=find_highest_priority_bundle();
  fprintf(stderr,"Next bundle to announce is %d\n",bundle_to_announce);
  if (bundle_to_announce!=-1)
    announce_bundle_piece(bundle_to_announce,&offset,mtu,msg_out,
			  prefix,servald_server,credential);
  // If including a bundle piece leaves space, then try announcing another piece.
  // This basically addresses the situation where the last few bytes of a manifest
  // are included, and there is space to start sending the body.
  if ((offset+21)<mtu) {
    if (bundle_to_announce!=-1)
      announce_bundle_piece(bundle_to_announce,&offset,mtu,msg_out,
			    prefix,servald_server,credential);
  }

  // Fill up spare space with BARs
  while (bundle_count&&(mtu-offset)>=BAR_LENGTH) {
    bundle_bar_counter++;
    if (bundle_bar_counter>=bundle_count) bundle_bar_counter=0;
    msg_out[offset++]='B'; // indicates a BAR follows
    append_bar(bundle_bar_counter,&offset,mtu,msg_out);
  }
  fprintf(stderr,"bundle_bar_counter=%d\n",bundle_bar_counter);
    
  // Increment message counter
  message_counter++;

  fprintf(stderr,"This message (hex): ");
  for(int i=0;i<offset;i++) fprintf(stderr,"%02x",msg_out[i]);
  fprintf(stderr,"\n");

  radio_send_message(msg_out,offset);
  
  return offset;
}
