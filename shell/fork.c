#include <stdio.h>
#include <unistd.h>
#include <sys/types.h>
//观察父子进程
int main(void){
    //PID 本身不长，但 C 语言要求你必须转成 long 才能用 printf 打印！
    printf("Before fork: current PID = %ld\n",(long)getpid());
    pid_t pid=fork();

    //pid有三种情况
    //-1创建失败，可能是进程数量超出了系统对用户进程数量的限制
    if(pid==-1){
        perror("fork");
        return 1;
    }
    //子进程
    if(pid==0){
        printf("Child process:\n");
        //对于子进程 fork只返回0
        printf("fork() returned:%ld\n",(long)pid);
        printf("child PID:%ld\n",(long)getpid());
        printf("parent PID:%ld\n",(long)getppid());
    }else{
    //这是父进程
        printf("Parent process:\n");
        printf("fork() returned child PID: %ld\n",(long)pid);
        printf("parent PID:               %ld\n",(long)getpid());
    }
    return 0;
}