/* Copyright (C) 2024 Hong Kong University of Science and Technology

This repository is used for the Computer Networks (ELEC 3120) 
course taught at Hong Kong University of Science and Technology. 

No part of the project may be copied and/or distributed without 
the express permission of the course staff. Everyone is prohibited 
from releasing their forks in any public places. */

#include <unistd.h>
#include <fstream>
#include <iostream>
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

  struct timespec start_time;
  timespec_get(&start_time, TIME_UTC);

  char buf[BUF_SIZE];
  while (ifs) {
    /* Read data from the file into the buffer. The amount of data read is
     * stored in bytes_read */
    ifs.read(buf, BUF_SIZE);
    int bytes_read = ifs.gcount();

    /* Write the data in the buffer to the socket. If an error occurs, print an
     * error message and return -1 */
    int bytes_written = foggy_write(sock, buf, bytes_read);
    if (bytes_written < 0) {
      cerr << "Error: Write failed\n";
      return -1;
    }
  }

  /* Close the socket and the output file void convert */
  foggy_close(sock);
  ifs.close();

  struct timespec end_time;
  timespec_get(&end_time, TIME_UTC);

  /* Calculate the transmission time in milliseconds */
  time_t transmission_time = (end_time.tv_sec - start_time.tv_sec) * 1000 +
                             (end_time.tv_nsec - start_time.tv_nsec) / 1000000;
  cout << "Transmission took " << transmission_time << " ms\n";

  return 0;
}

// Checkpoint 1 completed