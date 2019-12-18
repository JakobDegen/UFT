#include <stdio.h>
#include <sys/stat.h>
#include <sys/fcntl.h>
#include <stdlib.h>
#include <arpa/inet.h>

#include "UFT/UFT.hpp"

#define PRT 25565

int main(int argc, char **argv){
	int fd = open(argv[1], O_RDONLY);
	if(fd == -1){
		perror("could not open file\n");
		exit(1);
	}

	struct sockaddr_in srv;
	srv.sin_family = AF_INET;
	srv.sin_port = htons(PRT);
	inet_aton(argv[2], &srv.sin_addr);

	bool out = run_client(fd, (struct sockaddr *) &srv, sizeof(struct sockaddr_in), 30, cg);
	printf("Output: %d\n", out ? 1 : 0);
}