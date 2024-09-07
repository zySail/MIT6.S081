#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

int main(){
	char ch;
	while(read(0, &ch, 1) > 0){
		if(ch != '\n')
			printf("%c  ", ch);
		else
			printf("\n detect a \\n \n");
	}
	exit(0);
	return 0;
}

/*
	for(int i = 0; i < 5; i++){ read(0, &ch, 1);
 */
