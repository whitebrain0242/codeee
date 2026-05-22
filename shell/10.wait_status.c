#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
int main(int argc,char*argv[]){
    pid_t pid=fork();
    if(pid==-1){
        perror("fork");
        return 1;
    }
    if(pid==0){
        if(argc>=2&&argv[1][0]=='s'){
            printf("Child: I will terminate myself with SIGTERM.\n");
            raise(SIGTERM);
        }else{
            printf("Child: I will exit normally with status 42.\n");
            _exit(42);
        }
    }else{
        int status;
        //waitpid可以获取进程退出状态
        //注意这里拿到的status是一个编码后的状态值，不能直接printf
        //要使用宏，也就是要用专门的工具去使用他

        if(waitpid(pid,&status,0)==-1){
            perror("waitpid");
            return 1;

        }
        if(WIFEXITED(status)){
            //如果是正常退出的
            printf("Parent: child exited normally.\n");
            printf("Parent: exit status = %d\n", WEXITSTATUS(status));

        }else if(WIFSIGNALED(status)){
            //如果是信号杀死的
            printf("Parent: child was killed by signal.\n");
            printf("Parent: signal number = %d\n", WTERMSIG(status));
        } else {
            printf("Parent: child ended in another way.\n");
        }
        
    }
    return 0;
}