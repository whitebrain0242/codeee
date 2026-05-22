#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#include <errno.h>
//多进程自动回收
static volatile sig_atomic_t live_children=0;
//信号处理函数
//子进程一推出，内核就给父进程发送信号sigchid
static void sigchld_handler(int sig){
    (void)sig;//防止报错
    int saved_error=errno;//保存errno,防止被破坏
    while(1){
        //非阻塞，回收所有进程
        //-1,表示等待任何一个子进程退出
        //作用：一次性回收所有推出的子进程
        pid_t pid =waitpid(-1,NULL,WNOHANG);
        if(pid>0){
            live_children--;//回收一个孩子，计数减一

        }else{
            break;
        }
    }errno=saved_error;//恢复errno
}
int main(void){
    struct sigaction sa;
    sigset_t block_set;
    sigset_t old_set;
    //创建子进程期间，暂时屏蔽sigchld信号
    sigemptyset(&block_set);
    sigaddset(&block_set,SIGCHLD);
    sigprocmask(SIG_BLOCK,&block_set,&old_set);
    //注册信号处理函数
    sa.sa_handler=sigchld_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags=SA_RESTART;//被信号打断的函数，自动继续执行，别报错
    sigaction(SIGCHLD,&sa,NULL);///SIGCHLD：子进程退出时发出的信号
    //创建五个子进程
    for(int i=0;i<5;i++){
        live_children++;
        pid_t pid=fork();
        if (pid == -1) {
            perror("fork");
            live_children--;
            continue;
        }
        if(pid==0){
            sleep(i+1);
            _exit(100+i);
        }
    }
    //恢复信号屏蔽子
    sigprocmask(SIG_SETMASK, &old_set, NULL);
    //父进程等待所有子进程推出
    while(live_children>0){
        printf("Parent: live children = %d\n",live_children);
        sleep(1);
    }
    return 0;
}