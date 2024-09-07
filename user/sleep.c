#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

int main(int argc, char *argv[]){
	if(argc != 2){
		fprintf(2, "Usage: sleep n\n");
		exit(1);
	}
	int sleepTime = atoi(argv[1]);
	if(sleepTime <= 0){
		fprintf(2, "Error: Number must be positive.\n");
	}
	sleep(sleepTime);
	exit(0);
}
