#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

static void primeSieve(int read_fd){ // read numbers from upper proc, deal with it and create new proc
	int p;
	if(read(read_fd, &p, sizeof(int)) == 0)
	{
		return;
	}

	printf("prime %d\n", p);

	int n;
	int new_pipefd[2];
	pipe(new_pipefd);

	if(fork() == 0)
	{
		close(new_pipefd[1]); // close child proc's write fd
		primeSieve(new_pipefd[0]); // recursive call
		close(new_pipefd[0]); // close child proc's read fd after reading all the numbers
		exit(0);
	}
	else
	{
		close(new_pipefd[0]); // close father proc's read fd
		while(read(read_fd, &n, sizeof(int)) != 0) // loop: read from upper proc
		{
			if(n % p != 0)
			{
				write(new_pipefd[1], &n , sizeof(int));
			}
		}
		close(new_pipefd[1]); // close father proc's write fd after passing all numbers from upper proc
		while(wait(0) > 0){}
	}
	return;
}

int main(){
	int pipefd[2];
	pipe(pipefd);
	if(fork() == 0) // the other process
	{
		close(pipefd[1]); // close child proc's write fd
		primeSieve(pipefd[0]);
		close(pipefd[0]); // close child proc's read fd
		exit(0);
	}
	else // the first process
	{
		close(pipefd[0]); // close first proc's read fd
		for(int i = 2; i <= 35; i++)
		{
			write(pipefd[1], &i, sizeof(int));
		}
		close(pipefd[1]); // close first proc's write fd after passing all numbers
		
		while(wait(0) > 0){} // wait all child procs exit
		exit(0);
		
	}
	return 0;
}
