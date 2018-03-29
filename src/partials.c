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
#include <string.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <dirent.h>
#include <assert.h>

#include "sync.h"
#include "lbard.h"

int partial_recent_sender_report(struct partial_bundle *p)
{
  fprintf(stderr,"Recent senders for bundle %c%c%c%c*/%lld:\n",
	  p->bid_prefix[0],p->bid_prefix[1],p->bid_prefix[2],p->bid_prefix[3],
	  p->bundle_version);
  int i;
  time_t t=time(0);
  for(i=0;i<MAX_RECENT_SENDERS;i++)
    if ((t-p->senders.r[i].last_time)<10)
      fprintf(stderr,"  #%02d : %02X%02X* (T-%d sec)\n",
	      i,p->senders.r[i].sid_prefix[0],p->senders.r[i].sid_prefix[1],
	      (int)(t-p->senders.r[i].last_time));
  dump_partial(p);
  return 0;
}

/*
  Generate the starting offset and bitmap of 64 byte segments that we need
  relative to that point in the payload stream.  The purpose is to provide a list
  with enough pending 64 byte segments so that all our current senders know where they
  should next send from.

  The bitmap is based on the absolute first hole in the stream that we are missing.

  The segment list is arranged in reverse order, so we start by getting the last
  piece in the segment list. If it starts at 0, then our starting point is the end
  of the first segment. If not, then our starting point is 0. We then mark the bitmap
  as requiring all pieces.  Then the segment list is retraversed, and any 64 byte
  region that we have in its entirety is marked as already held.
*/
int partial_update_request_bitmap(struct partial_bundle *p)
{
  // Get starting point
  int starting_position=0;
  // 32*8*64= 16KiB of data, enough for several seconds, even with 16 senders.
  unsigned char bitmap[32];
  bzero(&bitmap[0],32);
  struct segment_list *l=p->body_segments;
  while(l&&l->next) l=l->next;
  if (l) {
    if (!l->start_offset) starting_position=l->length;
  }

  l=p->body_segments;
  while(l) {
    if ((l->start_offset>=starting_position)
	&&(l->start_offset<=(starting_position+32*8*64))) {
      int start=l->start_offset;
      int length=l->length;
      // Ignore any first partial 
      if (start&63) {
	int trim=64-(start&63);
	start+=trim;
	length-=trim;
      }
      // Work out first block number
      int block=(start-starting_position)>>6; //  divide by 64
      // Then mark as received all those we already have
      while (length>=64) {
	bitmap[block>>3]|=(1<<(block&7));
	block++; length-=64;
      }
    }

    l=l->next;
  }

  // Save request bitmap
  p->request_bitmap_start=starting_position;
  memcpy(p->request_bitmap,bitmap,32);
  
  return 0;
}

int partial_update_recent_senders(struct partial_bundle *p,char *sender_prefix_hex)
{
  // Get peer SID prefix as HEX
  unsigned char sender_prefix_bin[2];
  for(int i=0;i<2;i++)
    sender_prefix_bin[i]=hex_to_val(sender_prefix_hex[i*2+1])
      +hex_to_val(sender_prefix_hex[i*2+0])*16;

  int free_slot=random()%MAX_RECENT_SENDERS;
  int index=0;
  time_t t = time(0);
  for(index=0;index<MAX_RECENT_SENDERS;index++)
    {
      if ((sender_prefix_bin[0]==p->senders.r[index].sid_prefix[0])
	  &&(sender_prefix_bin[1]==p->senders.r[index].sid_prefix[1])) {
	break;
      }
      if ((t-p->senders.r[index].last_time)>=30) free_slot=index;
    }
  if (index==MAX_RECENT_SENDERS) index=free_slot;

  // Update record
  p->senders.r[index].sid_prefix[0]=sender_prefix_bin[0];
  p->senders.r[index].sid_prefix[1]=sender_prefix_bin[1];
  p->senders.r[index].last_time=time(0);

  partial_recent_sender_report(p);
  
  return -1;
}

int clear_partial(struct partial_bundle *p)
{
  while(p->manifest_segments) {
    struct segment_list *s=p->manifest_segments;
    p->manifest_segments=s->next;
    if (s->data) free(s->data); s->data=NULL;
    free(s);
    s=NULL;
  }
  while(p->body_segments) {
    struct segment_list *s=p->body_segments;
    p->body_segments=s->next;
    if (s->data) free(s->data); s->data=NULL;
    free(s);
    s=NULL;
  }

  bzero(p,sizeof(struct partial_bundle));
  return -1;
}

int dump_segment_list(struct segment_list *s)
{
  if (!s) return 0;
  while(s) {
    fprintf(stderr,"    [%d,%d)\n",s->start_offset,s->start_offset+s->length);
    s=s->next;
  }
  return 0;
}

int dump_progress_bitmap(unsigned char *b)
{
  for(int i=0;i<(32*8);i++) {
    if (b[i>>3]&(1<<(i&7)))
      fprintf(stderr,"."); else fprintf(stderr,"Y");
    if (((i&63)==63)&&(i!=255)) fprintf(stderr,"\n    ");
  }
  fprintf(stderr,"\n");
  return 0;
}

int dump_partial(struct partial_bundle *p)
{
  fprintf(stderr,"Progress receiving BID=%s* version %lld:\n",
	  p->bid_prefix,p->bundle_version);
  fprintf(stderr,"  manifest is %d bytes long, and body %d bytes long.\n",
	  p->manifest_length,p->body_length);
  fprintf(stderr,"  Manifest pieces received:\n");
  dump_segment_list(p->manifest_segments);
  fprintf(stderr,"  Body pieces received:\n");
  dump_segment_list(p->body_segments);
  fprintf(stderr,"  Request bitmap: start=%d, bits=\n    ",
	  p->request_bitmap_start);
  dump_progress_bitmap(p->request_bitmap);
  return 0;
}

int merge_segments(struct segment_list **s)
{
  if (!s) return -1;
  if (!(*s)) return -1;

  // Segments are sorted in descending order
  while((*s)&&(*s)->next) {
    struct segment_list *me=*s;
    struct segment_list *next=(*s)->next;
    if (me->start_offset<=(next->start_offset+next->length)) {
      // Merge this piece onto the end of the next piece
      if (debug_pieces)
	printf("Merging [%d..%d) and [%d..%d)\n",
		me->start_offset,me->start_offset+me->length,
		next->start_offset,next->start_offset+next->length);
      int extra_bytes
	=(me->start_offset+me->length)-(next->start_offset+next->length);
      int new_length=next->length+extra_bytes;
      next->data=realloc(next->data,new_length);
      assert(next->data);
      bcopy(&me->data[me->length-extra_bytes],&next->data[next->length],
	    extra_bytes);
      next->length=new_length;

      // Excise redundant segment from list
      *s=next;
      next->prev=me->prev;
      if (me->prev) me->prev->next=next;

      // Free redundant segment.
      free(me->data); me->data=NULL;
      free(me); me=NULL;
    } else 
      s=&(*s)->next;
  }
  return 0;
}