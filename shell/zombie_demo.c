#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/wait.h>
int main(void){
    pid_t pid=fork();
    if(fork==-1){
        perror("fork");
        return 1;
    }
    if(pid==0){
        printf("exit immediately and PID:%ld\n",(long)getpid());
        _exit(0);
    }else{
        printf("children PID:%ld    and sleep 20 seconds\n",(long)pid);
        printf("Run this command in another terminal:\n");
        printf("ps -o pid,ppid,stat,cmd -p %ld\n",(long)pid);
        sleep(20);
        printf("kill zombie:Parent: now calling waitpid() to reap child.\n");
        waitpid(pid,NULL,0);
        printf("children reaped,sleep 5 seconds\n");
        sleep(5);
    }
    return 0;
}