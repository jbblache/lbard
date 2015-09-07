#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <assert.h>
#include <netdb.h>
#include <time.h>

#include "serial.h"

int connect_to_port(char *host,int port)
{
  struct hostent *hostent;
  hostent = gethostbyname(host);
  if (!hostent) {
    return -1;
  }

  struct sockaddr_in addr;  
  addr.sin_family = AF_INET;     
  addr.sin_port = htons(port);   
  addr.sin_addr = *((struct in_addr *)hostent->h_addr);
  bzero(&(addr.sin_zero),8);     

  int sock=socket(AF_INET, SOCK_STREAM, 0);
  if (sock==-1) {
    perror("Failed to create a socket.");
    return -1;
  }

  if (connect(sock,(struct sockaddr *)&addr,sizeof(struct sockaddr)) == -1) {
    perror("connect() to port failed");
    close(sock);
    return -1;
  }
  return sock;
}

int num_to_char(int n)
{
  assert(n>=0); assert(n<64);
  if (n<26) return 'A'+(n-0);
  if (n<52) return 'a'+(n-26);
  if (n<62) return '0'+(n-52);
  switch(n) {
  case 62: return '+'; 
  case 63: return '/';
  default: return -1;
  }
}

int base64_append(char *out,int *out_offset,unsigned char *bytes,int count)
{
  int i;
  for(i=0;i<count;i+=3) {
    int n=4;
    unsigned int b[30];
    b[0]=bytes[i];
    if ((i+2)>=count) { b[2]=0; n=3; } else b[2]=bytes[i+2];
    if ((i+1)>=count) { b[1]=0; n=2; } else b[1]=bytes[i+1];
    out[(*out_offset)++] = num_to_char((b[0]&0xfc)>>2);
    out[(*out_offset)++] = num_to_char( ((b[0]&0x03)<<4) | ((b[1]&0xf0)>>4) );
    if (n==2) {
      out[(*out_offset)++] = '=';
      out[(*out_offset)++] = '=';
      return 0;
    }
    out[(*out_offset)++] = num_to_char( ((b[1]&0x0f)<<2) | ((b[2]&0xc0)>>6) );
    if (n==3) {
      out[(*out_offset)++] = '=';
      return 0;
    }
    out[(*out_offset)++] = num_to_char((b[2]&0x3f)>>0);
  }
  return 0;
}
  

int http_get_simple(char *server_and_port, char *auth_token,
		    char *path, FILE *outfile, int timeout)
{
  // Send simple HTTP request to server, and write result into outfile.

  char server_name[1024];
  int server_port=-1;

  if (sscanf(server_and_port,"%[^:]:%d",server_name,&server_port)!=2) return -1;
  
  int timeout_time=time(0)+timeout;
  
  if (strlen(auth_token)>500) return -1;
  if (strlen(path)>500) return -1;
  
  char request[1024];
  char authdigest[1024];
  int zero=0;
  
  base64_append(authdigest,&zero,(unsigned char *)auth_token,strlen(auth_token));

  // Build request
  snprintf(request,1024,
	   "GET %s HTTP/1.1\n"
	   "Authorisation: Basic %s\n"
	   "Host: %s:%d\n"
	   "Accept: */*\n"
	   "\n",
	   path,
	   authdigest,
	   server_name,server_port);

  int sock=connect_to_port(server_name,server_port);
  if (sock<0) return -1;

  write_all(sock,request,strlen(request));

  // Read reply, streaming output to file after we have skipped the header
  int http_response=-1;
  char line[1024];
  int len=0;
  int empty_count=0;
  set_nonblock(sock);
  int r;
  while(len<1024) {
    r=read_nonblock(sock,&line[len],1);
    if (r==1) {
      if ((line[len]=='\n')||(line[len]=='\r')) {
	if (len) empty_count=0; else empty_count++;
	line[len+1]=0;
	if (len) printf("Line of response: %s\n",line);
	if (sscanf(line,"HTTP/1.0 %d",&http_response)==1) {
	  // got http response
	}
	len=0;
	// Have we found end of headers?
	if (empty_count==3) break;
      } else len++;
    } else usleep(1000);
    if (time(0)>timeout_time) {
      // If still in header, just quit on timeout
      close(sock);
      return -1;
    }
  }

  // Got headers, read body and write to file
  printf("  reading body...\n");
  
  close(sock);
  
  return http_response;
}
