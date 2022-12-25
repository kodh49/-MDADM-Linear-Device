#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdio.h>
#include <errno.h>
#include <err.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <arpa/inet.h>
#include "net.h"
#include "jbod.h"

/* the client socket descriptor for the connection to the server */
int cli_sd = -1;

/* Retrieve the command field of a given jbod operation from its 32 bit opcode */
int get_command(uint32_t op) {
  uint32_t mask = 258048; // 32 bit-wide mask: all bits are 1s except for 0s at [12:17]
  return (int)((op & mask) >> 12); // isolate bitstream [12:17] of opcode 
}

/* attempts to read n (len) bytes from fd; returns true on success and false on failure. 
It may need to call the system call "read" multiple times to reach the given size len. 
*/
static bool nread(int fd, int len, uint8_t *buf) {
  int count = 0; // amount of remaining bytes to read
  while (count != len) {
    int rl = read(fd, buf+count, len-count); // number of bytes read from single syscall
    if (rl < 0) return false; // error while reading
    count = count + rl; // update amount of bytes read
  }
  return true; // read complete
}


/* attempts to write n bytes to fd; returns true on success and false on failure 
It may need to call the system call "write" multiple times to reach the size len.
*/
static bool nwrite(int fd, int len, uint8_t *buf) {
  int count = 0; // amount of remaining bytes to write
  while (count != len) {
    int wl = write(fd, buf+count, len-count); // amount of bytes written from single syscall
    if (wl < 0) return false; // error while writing
    count = count + wl; // update amount of bytes wrote
  }
  return true; // write complete
}

/* Through this function call the client attempts to receive a packet from sd 
(i.e., receiving a response from the server.). It happens after the client previously 
forwarded a jbod operation call via a request message to the server.  
It returns true on success and false on failure. 
The values of the parameters (including op, ret, block) will be returned to the caller of this function: 

op - the address to store the jbod "opcode"  
ret - the address to store the info code (lowest bit represents the return value of the server side calling the corresponding jbod_operation function. 2nd lowest bit represent whether data block exists after HEADER_LEN.)
block - holds the received block content if existing (e.g., when the op command is JBOD_READ_BLOCK)

In your implementation, you can read the packet header first (i.e., read HEADER_LEN bytes first), 
and then use the length field in the header to determine whether it is needed to read 
a block of data from the server. You may use the above nread function here.  
*/
static bool recv_packet(int sd, uint32_t *op, uint8_t *ret, uint8_t *block) {
  uint8_t header[HEADER_LEN];
  uint32_t opcode;
  if (nread(sd, HEADER_LEN, header) == false) return false; // read header from response packet
  // decode header
  memcpy(&opcode, header, sizeof(uint32_t));
  *op = ntohl(opcode); // op = first 4 bytes of header (host format)
  *ret = header[4]; // ret = last byte of header
  if ((header[4] == 2) || (header[4] == 3)) { // info_code = 2'b10 or 2'b11: payload exists
    if (nread(sd, JBOD_BLOCK_SIZE, block) == false) return false; // read payload from response packet
  } else { // info_code = 2'b01 or 2'b00: payload does not exist
    block = NULL;
  }
  return true;
}


/* The client attempts to send a jbod request packet to sd (i.e., the server socket here); 
returns true on success and false on failure. 

op - the opcode. 
block- when the command is JBOD_WRITE_BLOCK, the block will contain data to write to the server jbod system;
otherwise it is NULL.

The above information (when applicable) has to be wrapped into a jbod request packet (format specified in readme).
You may call the above nwrite function to do the actual sending.  
*/
static bool send_packet(int sd, uint32_t op, uint8_t *block) {
  int command = get_command(op);
  uint32_t opcode = htonl(op);
  int PACKET_SIZE = (command == JBOD_WRITE_BLOCK) ? (HEADER_LEN+JBOD_BLOCK_SIZE) : HEADER_LEN;
  // packet construction
  uint8_t packet[PACKET_SIZE];
  memcpy(packet, &opcode, sizeof(uint32_t)); // packet[0:3] = opcode (network byte form)
  if (command == JBOD_WRITE_BLOCK) { // payload exists
    packet[4] = 3; // packet[4] = info_code = 2'b11: payload exists
    memcpy(&packet[HEADER_LEN], block, JBOD_BLOCK_SIZE); // packet[5:260] = block
  }
  else { // payload does not exist
    packet[4] = 1; // packet[4] = info_code = 2'b01: payload does not exist
  }
  // write packet to active socket descriptor
  return nwrite(sd, PACKET_SIZE, packet);
}



/* attempts to connect to server and set the global cli_sd variable to the
 * socket; returns true if successful and false if not. 
 * this function will be invoked by tester to connect to the server at given ip and port.
 * you will not call it in mdadm.c
*/
bool jbod_connect(const char *ip, uint16_t port) {
  // setup the address information
  struct sockaddr_in sa;
  sa.sin_family = AF_INET;
  sa.sin_port = htons(port);

  if (inet_pton(AF_INET, ip, &(sa.sin_addr)) == 0) {
    return false; // error on specifying address
  }
  
  int new_sd = socket(AF_INET, SOCK_STREAM, 0);
  if (new_sd == -1) {
    return false; // error on socket creation
  } else {
    // initialize new successful socket
    cli_sd = new_sd;
  }
  
  if (connect(cli_sd, (const struct sockaddr *) &sa, sizeof(sa)) == -1) {
    return false; // error on connection
  }
  
  return true; // connection successful
}




/* disconnects from the server and resets cli_sd */
void jbod_disconnect(void) {
  close(cli_sd);
  cli_sd = -1;
}



/* sends the JBOD operation to the server (use the send_packet function) and receives 
(use the recv_packet function) and processes the response. 

The meaning of each parameter is the same as in the original jbod_operation function. 
return: 0 means success, -1 means failure.
*/
int jbod_client_operation(uint32_t op, uint8_t *block) {
  uint32_t recv_op; // opcode of response packet
  uint8_t ret; // info code of response packet
  if (send_packet(cli_sd, op, block) == false) return -1;
  if (recv_packet(cli_sd, &recv_op, &ret, block) == false) return -1;
  if (recv_op == op) { // received packet is a response of previously requested one
    if ((ret == 1) || (ret == 3)) return -1; // JBOD_OPERATION failed
    return 0; // JBOD_OPERATION successful
  }
  return -1;
}
