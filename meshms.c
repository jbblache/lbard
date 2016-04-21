/*
Serval Low-bandwidth asychronous Rhizome Demonstrator.
Copyright (C) 2016 Serval Project Inc.

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
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>
#include <dirent.h>
#include <assert.h>
#include <fcntl.h>
#include <time.h>
#include <sys/time.h>

#include "sync.h"
#include "lbard.h"

int meshms_list_conversations(char *sid_hex)
{
  return 0;
}

int meshms_list_messages(char *sender_sid_hex, char *recipient_sid_hex)
{
  return 0;
}

int meshms_send_message(char *sender_sid_hex, char *recipient_sid_hex,
			char *message)
{
  return 0;
}


int meshms_usage()
{
  fprintf(stderr,"lbard meshms commands:\n"
	  "  lbard meshms list conversations <SID>\n"
	  "  lbard meshms list messages <sender> <recipient>\n"
	  "  lbard meshms send <sender> <recipient> <message>\n");
  exit(-1);
}

int meshms_parse_command(int argc,char **argv)
{
  if (argc<4) {
    exit(meshms_usage());
    if (!strcasecmp(argv[2],"list")) {
      if (!strcasecmp(argv[3],"conversations")) {
	if (!argv[4]) exit(meshms_usage());
	exit(meshms_list_conversations(argv[4]));
      } else if (!strcasecmp(argv[3],"messages")) {
	if (!argv[4]) exit(meshms_usage());
	if (!argv[5]) exit(meshms_usage());
	exit(meshms_list_messages(argv[4],argv[5]));
      } else {
	fprintf(stderr,"Unsupported lbard meshms operation.\n");
	exit(meshms_usage());
      }
    } else if (!strcasecmp(argv[2],"send")) {
      if (!argv[4]) exit(meshms_usage());
      if (!argv[5]) exit(meshms_usage());
      if (!argv[6]) exit(meshms_usage());
      exit(meshms_send_message(argv[4],argv[5],argv[6]));
    } else {
      fprintf(stderr,"Unsupported lbard meshms operation.\n");
      exit(meshms_usage());
    }
  }
  exit(-1);
}

