#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/wait.h>
//也就是创建子进程，把紫禁城变成另一个程序，然后父进程等待紫禁城结束
int main(void){
pid_t pid =fork();
if(pid==-1){
    perror("fork");
    return 1;
}
if(pid==0){
    printf("CHILD:before exec ,PID+%ld\n",(long)getpid());
    execlp("ls","ls","-l","/tmp",NULL);
    //一般来说，execlp成功后，子进程就不再运行这个c程序了，变成了ls程序

    perror("execlp failed");
    _exit(127);
}else{
    int status;
    printf("PARENT:child pid=%ld",(long)pid);
    waitpid(pid,&status,0);
    if(WIFEXITED(status)){
        printf("PARENT:child exit status=%d\n",WEXITSTATUS(status));
    }
}return 0;
}