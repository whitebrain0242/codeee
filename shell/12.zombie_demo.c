#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/wait.h>
int main(void){
    pid_t pid=fork();
    if(pid==-1){
        perror("fork");
        return 1;
    }
    if(pid==0){
        printf("exit immediately and PID:%ld\n",(long)getpid());
        _exit(0);
    }else{
        
        printf("children PID:%ld    and sleep 20 seconds\n",(long)pid);
        //使用命令，查看子进程状态，是Z就是僵尸进程，因为这20秒里没有回收
        printf("Run this command in another terminal:\n");
        printf("ps -o pid,ppid,stat,cmd -p %ld\n",(long)pid);
        sleep(20);
        //回收
        printf("kill zombie:Parent: now calling waitpid() to reap child.\n");
        waitpid(pid,NULL,0);
        printf("children reaped,sleep 5 seconds\n");
        sleep(5);
    }
    return 0;
}