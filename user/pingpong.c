#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

int main(){
	char buf[5];
	int pf2c[2]; // the pipe from father to child
	int pc2f[2]; // the pipe from child to father
	pipe(pf2c);
	pipe(pc2f);

	//write(pf2c[1], "a", 1); // write 1 byte to child

	if(fork() == 0)
	{
		if(read(pf2c[0], buf, 1) == 1) // child read from the pipe
		{	
			printf("%d: received ping\n", getpid());
			write(pc2f[1], "b" , 1);
			exit(0);	
		}
		else
		{
			exit(1);
		}
	}
	else
	{
		write(pf2c[1], "a", 1); // write 1 byte to child
		if(read(pc2f[0], buf, 1) == 1) // father read from the pipe
		{
			printf("%d: received pong\n", getpid());
			exit(0);
		}
		else
		{
			exit(1);
		}
	}	
}
