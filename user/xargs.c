#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"
#include "kernel/param.h"


int main(int argc , char* argv[]){
	char* newargv[MAXARG];	
	char buf[512]; // store the arg read from std input
	char ch;
	int index = 0;

	if(argc < 2){
		printf("Usage: xargs commend <arg>");
		exit(1);
	}
	else if(argc >= MAXARG){
		printf("xargs's arg too long\n");
		exit(1);
	}

	for(int i = 0; i < argc-1; i++){
		newargv[i] = argv[i+1];
	}
	newargv[argc] = 0;

	while(read(0, &ch, sizeof(char)) > 0){
		if(ch == '\n'){
			buf[index] = '\0';
			newargv[argc-1] = buf;

			// execute with new arg
			//for(int i = 0; i < argc; i++) printf("argv[%d]: %s\n", i, newargv[i]);
			if(fork() == 0){
				exec(newargv[0], newargv);
			}
			else{
				wait(0);
			}
			
			// reset
			index = 0;
			memset(buf, 0, 512);
		}
		else{
			if(index < 512 - 1)
				buf[index++] = ch;
			else{
				printf("New argument is too long\n");
				exit(1);
			}
		}
	}
	exit(0);
	//return 0;
}

