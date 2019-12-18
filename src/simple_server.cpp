#include <stdio.h>
#include <sys/stat.h>
#include <sys/fcntl.h>
#include <stdlib.h>

#include "UFT/UFT.hpp"

#define PRT 25565

int main(int argc, char **argv){
	int fd = open(argv[1], O_WRONLY);
	if(fd == -1){
		perror("could not open file\n");
		exit(1);
	}

	bool out = run_server(fd, PRT, (uint32_t) 10000000, 30);
	printf("Output: %d\n", out ? 1 : 0);
}