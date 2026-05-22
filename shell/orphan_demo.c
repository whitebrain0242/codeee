#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
//僵尸进程是子进程死了但是父进程没死不管不回收
//孤儿进程是父进程死了但是子进程还或者，被系统收养
int main(void){
    pid_t pid=fork();
    if(pid==-1){
        perror("fork");
        return 1;
    }
    if(pid==0){
        printf("im alive.and my pid=%ld\n",(long)getpid());
        printf("before: my parent pid= %ld\n",(long)getppid());
        sleep(5);
        printf("last my parent pid=%ld\n",(long)getppid());
        printf("im alive but my parent is gone");
        _exit(0);
    }else{
        printf("my pid =%ld\n",(long)getpid());
        printf("i want to die!");
        sleep(1);
        exit(0);
    }
    
}