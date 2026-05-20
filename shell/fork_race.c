#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/wait.h>
//父进程和子进程是不知道运行的顺序的，可能是副进程运行一下子进程运行一下
int main(void){
    setbuf(stdout,NULL);
    pid_t pid=fork();
    if(pid==-1){
        perror("fork");
        return 1;

    }
    if(pid==0){
        for(int i=0;i<5;i++){
            printf("Child:  i = %d\n",i);
            usleep(100000);
        }
        _exit(0);
    }else{
        for(int i=0;i<5;i++){
            printf("Parent: i = %d\n",i);
            usleep(100000);
        }
        waitpid(pid,NULL,0);
    }
    return 0;
}