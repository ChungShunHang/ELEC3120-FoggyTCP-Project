/* Copyright (C) 2024 Hong Kong University of Science and Technology

This repository is used for the Computer Networks (ELEC 3120) 
course taught at Hong Kong University of Science and Technology. 

No part of the project may be copied and/or distributed without 
the express permission of the course staff. Everyone is prohibited 
from releasing their forks in any public places. */

#include <unistd.h>
#include <fstream>
#include <iostream>
#include <cstring>
using namespace std;

#include "foggy_tcp.h"

#define BUF_SIZE 4096

/**
 * This file implements a simple TCP client. Its purpose is to provide simple
 * test cases and demonstrate how the sockets will be used.
 *
 * Usage: ./client <server-ip> <server-port> <filename>
 *
 * For example:
 * ./client 10.0.1.1 3120 test.in
 */

int main(int argc, const char* argv[]) {
  if (argc != 4) {
    cerr << "Usage: " << argv[0] << " <server-ip> <server-port> <filename>\n";
    return -1;
  }

  const char* server_ip = argv[1];
  const char* server_port = argv[2];
  const char* filename = argv[3];
  struct timespec start_time;

  /* Create an initiator socket */
  void* sock = foggy_socket(TCP_INITIATOR, server_port, server_ip);

  /* Open the input file. If the file can't be opened, print an error message
   * and return -1 */
  ifstream ifs(filename);
  if (!ifs) {
    cerr << "Error: Can't open \"" << filename << "\"\n";
    return -1;
  }

  /* Wait for one second to ensure the socket is up */
  sleep(1);

  char buf[BUF_SIZE];
  bool first_packet = true;
  
  while (ifs) {
    /* Read data from the file into the buffer. The amount of data read is
     * stored in bytes_read */
    ifs.read(buf, BUF_SIZE);
    int bytes_read = ifs.gcount();

    if (first_packet && bytes_read > 0) {
      timespec_get(&start_time, TIME_UTC);
      
      /* Insert timestamp into first packet */
      char timestamped_buf[BUF_SIZE + sizeof(struct timespec)];
      memcpy(timestamped_buf, &start_time, sizeof(start_time));
      memcpy(timestamped_buf + sizeof(start_time), buf, bytes_read);
      
      /* Write timestamped first packet */
      int bytes_written = foggy_write(sock, timestamped_buf, bytes_read + sizeof(start_time));
      if (bytes_written < 0) {
        cerr << "Error: Write failed\n";
        return -1;
      }
      first_packet = false;
      continue;
    }

    if (bytes_read > 0) {
      int bytes_written = foggy_write(sock, buf, bytes_read);
      if (bytes_written < 0) {
        cerr << "Error: Write failed\n";
        return -1;
      }
    }
  }

  /* Close the socket and the output file void convert */
  foggy_close(sock);
  ifs.close();
  cout << "Client: File transmission completed\n";

  return 0;
}

// Checkpoint 1 completed