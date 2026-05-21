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
        printf("sleep five seconds");
        sleep(5);
        printf("exit normally");
        _exit(77);
    }else{
        int status;
        while(1){
            //非阻塞等待，
            //waitpid》-1,出错，0,进行中，其他，成功运行完成
            pid_t result=waitpid(pid,&status,WNOHANG);
            if(result==-1){
                error("waitpid");
                return 1;

            }else if(result==0){
                printf("ing...\n");
                sleep(1);
            }else{
                printf("finishd\n");
                if(WIFEXITED(status)){
                    //如果正常退出了
                    printf("children exit status:%d\n",WEXITSTATUS(status));

                }
                break;//退出
            }
        }
    }
    return 0;
}