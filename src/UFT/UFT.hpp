#ifndef UFT_H
#define UFT_H

#include <arpa/inet.h>
#include <sys/types.h>

/*
Main header file for the UFT library. This library allows files to be transferred reliably over
UDP using a custom protocol. It was built as a project for a networks course.
*/

/*
Run the server side of the application. Await an incoming transfer on port and write at most
max_size bytes from this transfer to the file descriptor given.

The return value of the function is a boolean indicating whether the transfer was successful.
*/
bool run_server(int file_descriptor, int port, uint32_t max_size, long long rtt);

/*
Run the client side of the application. Immediately starts a transfer to the address at remote
on the specified port, reading from file_descriptor until EOF. The remote_size should be the
size of the remote struct as required by functions such as sendto.

The return value of the function is a boolean indicating whether the transfer was successful.
*/
bool run_client(int file_descriptor, struct sockaddr *remote, socklen_t remote_size,
	long long rtt, int should_wait);


#endif