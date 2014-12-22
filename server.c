/*
 * The MIT License (MIT)
 *
 * Copyright (c) 2014 Michael Brumlow
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include <stdlib.h>
#include <stdio.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>

struct state {  
    int header; // 1 byte (read/write) , 64bit (sector number), 32bits (size)
    unsigned char cmd; 
    unsigned long long sector; 
    unsigned size; 
    unsigned current; 
    char *buf;
};

void
resetState(struct state *state) 
{
    state->header = 0;
    state->cmd = 'x';
    state->sector = 0;
    state->size = 0;
    state->current = 0; 
    state->buf = NULL;
}

void
processMessage(int clientfd, int filefd, struct state * state) 
{
    off_t ret = 0; 

    ret = lseek(filefd, state->sector * 512, SEEK_SET);
    if(ret != state->sector * 512) {
        fprintf(stderr, "could not seek to ofset: %llu\n", state->sector * 512);   
        // FIXME: handle IO error. 
    }

    // FIXME: need to handle errors and extend protocol to support them.  
    if(state->cmd == 'r') { 
        char *buf = NULL;
        buf = malloc(state->size);
        read(filefd, buf, state->size);
        write(clientfd, buf, state->size);
        free(buf);
    } else if ( state->cmd == 'w') {
        write(filefd, state->buf, state->size); 
    }
    
    if(state->buf) {
        free(state->buf);
        state->buf = NULL;
    }
}

void
processChunk(int filefd, int clientfd, unsigned char *buf, int size, struct state *state) 
{
    int i; 
    for(i = 0; i < size; i++){
     
        if( state->header < 1) {
            state->cmd = buf[i];
            state->header++;
            continue;
        }

        if( state->header > 0 && state->header < 9 ) {
            state->sector |= buf[i] << (8 * (state->header - 1));
            state->header++;
            continue;
        }
        
        if( state->header > 8 && state->header < 13 ) {
            state->size |= buf[i] << (8 * (state->header - 9));
            state->header++;
            continue;
        }
       
        if((state->header == 13 && state->cmd == 'w' && state->current < state->size)) {
            if(!state->buf) {
                state->buf = malloc(state->size);    
            }
            state->buf[state->current++] = buf[i];
        }

        // handle message if we are ready to. 
        if((state->header == 13 && state->cmd == 'r') || 
           (state->header == 13 && state->cmd == 'w' && state->current == state->size)) {
            processMessage(clientfd, filefd, state);
            resetState(state);
        }

    }
        
    // handle message if we are ready to. 
    if((state->header == 13 && state->cmd == 'r') || 
       (state->header == 13 && state->cmd == 'w' && state->current == state->size)) {
           
        processMessage(clientfd, filefd, state);
        resetState(state);
    }
    

}

int 
main(void)
{
    int ret;
    int len; 
    int filefd, serverfd;
    struct sockaddr_in serv_addr; 
    struct sockaddr_in address;
    struct state state;
    unsigned char buf[1024];

    resetState(&state);

    filefd = open("/tmp/blockdev.img", O_RDWR | O_CREAT, S_IRWXU ); 
    if( filefd == -1 ) {
        fprintf(stderr, "Could not open backing file!\n");
        return -1;    
    }
  
   serverfd  = socket(AF_INET, SOCK_STREAM, 0);
   
   serv_addr.sin_family = AF_INET;
   serv_addr.sin_addr.s_addr = htonl(INADDR_ANY);
   serv_addr.sin_port = htons(1337);
   
   ret = bind(serverfd, (struct sockaddr*)&serv_addr, sizeof(serv_addr));
   if( ret == -1 ){
        fprintf(stderr, "Could not bind!\n");   
   }

   listen(serverfd,1);

   printf("Wating for connection from driver ...\n");
   len = sizeof(address);
   int clientfd = accept(serverfd, (struct sockaddr*)&address, &len); 
   if(clientfd < 0) {
    fprintf(stderr, "Could not accept connection!\n");   
   }
   
   printf("Got connection.\n");

   int size; 
   while(1) {
      
      // hack to not use select. 
      // replace this with a select loop and a queue of work for writes. 
      size = state.size - state.current; 
      if(size > sizeof(buf)) 
          size = sizeof(buf);
      
      if(!(size > 0)){
        size = 1;   
      }
       
      ret = read(clientfd, buf, size);
      if(ret == 0) break;
      processChunk(filefd, clientfd, buf, ret, &state); 
   }

}


