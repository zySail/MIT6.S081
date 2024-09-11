#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"
#include "kernel/param.h"


int main(int argc , char* argv[]){
	if(argc < 3){
		printf("Usage: trace <syscallNumber> <tracedCommend>\n");
		exit(1);
	}

	if(trace(atoi(argv[1])) < 0) // write mask into proc struct
			return -1;

	// execute 
	for(int i = 0; i < argc - 1; i++){
		argv[i] = argv[i+2];
	}
	exec(argv[0], argv);

	exit(0);
}
