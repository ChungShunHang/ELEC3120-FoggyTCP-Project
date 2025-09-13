/**
 * Copyright (C) 2024 Hong Kong University of Science and Technology
 *
 * This repository is used for the Computer Networks (ELEC 3120) course taught
 * at Hong Kong University of Science and Technology.
 *
 * No part of the project may be copied and/or distributed without the express
 * permission of the course staff. Everyone is prohibited from releasing their
 * forks in any public places.
 */

#include <fstream>
#include <iostream>
#include <cstring>
using namespace std;

#include "foggy_tcp.h"

#define BUF_SIZE 4096

/**
 * This file implements a simple TCP server. Its purpose is to provide simple
 * test cases and demonstrate how the sockets will be used.
 *
 * Usage: ./server <server-ip> <server-port> <filename>
 *
 * For example:
 * ./server 10.0.1.1 3120 test.out
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

  /* Create a listener socket */
  void* sock = foggy_socket(TCP_LISTENER, server_port, server_ip);

  /* Open the output file. If the file can't be opened, print an error message
   * and return -1 */
  ofstream ofs(filename);
  if (!ofs) {
    cerr << "Error: Can't open \"" << filename << "\"\n";
    return -1;
  }

  char buf[BUF_SIZE + sizeof(struct timespec)];
  bool first_packet = true;
  
  while (true) {
    /* Read data from the socket into the buffer. The amount of data read is
     * stored in bytes_read. If bytes_read is less than or equal to 0, it means
     * we've reached the end of transmission or an error occurred. We break out
     * of the loop */
    int bytes_read = foggy_read(sock, buf, BUF_SIZE + sizeof(struct timespec));
    if (bytes_read <= 0)
      break;

    if (first_packet) {
      /* Extract start time from first packet */
      if (bytes_read < sizeof(start_time)) {
        cerr << "Error: First packet too small to contain timestamp\n";
        return -1;
      }
      
      memcpy(&start_time, buf, sizeof(start_time));
      
      /* Write the actual data (excluding timestamp) to file */
      int actual_data_size = bytes_read - sizeof(start_time);
      if (actual_data_size > 0) {
        ofs.write(buf + sizeof(start_time), actual_data_size);
      }
      first_packet = false;
    } else {
      ofs.write((char*)buf, bytes_read);
    }
  }

  struct timespec end_time;
  timespec_get(&end_time, TIME_UTC);

  /* Close the socket and the output file */
  foggy_close(sock);
  ofs.close();

  time_t transmission_time = (end_time.tv_sec - start_time.tv_sec) * 1000 +
                             (end_time.tv_nsec - start_time.tv_nsec) / 1000000;
  cout << "Complete transmission in " << transmission_time << " ms\n";
  cout << "Done: Transmitted \"" << filename << "\"\n";

  return 0;
}

// Checkpoint 1 completed