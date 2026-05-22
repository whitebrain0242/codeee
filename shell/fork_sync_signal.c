#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <signal.h>
#include <sys/wait.h>
//读写不会被信号打断，不被优化
static volatile sig_atomic_t got_signal=0;
//信号处理函数，修改标记，说信号来了
static void handler(int sig){
    (void)sig;
    got_signal=1;
}
int main(void){
//提前屏蔽SIGUSR1：防止信号提前到达，打乱等待逻辑

    sigset_t block_set;//屏蔽数组
    sigset_t old_set;//备份数组
    sigemptyset(&block_set);//清空
    sigaddset(&block_set,SIGUSR1);//把sigusr1信号加入屏蔽数组
    //设置信号屏蔽到blockset里面，把就状态保存到oldset里面
    if(sigprocmask(SIG_BLOCK,&block_set,&old_set)==-1){
        perror("sigprocmask");
        return 1;

    }

//绑定信号处理规则，记住sigusr1的处理方式

    //设置一个结构体，保存信号处理方式
    struct sigaction sa;
    sa.sa_handler=handler;//设置处理器
    sigemptyset(&sa.sa_mask);//清空屏蔽信号，也就是不屏蔽任何信号
    sa.sa_flags=0;//没有默认行为
    //当进程收到SIGUSR1的时候，按照我配置好的sa规则去处理
    if(sigaction(SIGUSR1,&sa,NULL)==-1){
        perror("sigaction");

        return 1;

    }

    //创建父子进程
    pid_t pid=fork();
    if(pid==-1){
        perror("fork");
        return 1;

    }
    //子进程先模拟耗时任务，然后给副进程发送信号sigusr1,说我万事了
    if(pid==0){
         printf("Child: doing work...\n");
         sleep(2);
          printf("Child: sending SIGUSR1 to parent.\n");
          //这里是发送信号，获取父进程的pid给他发送sigusr1信号，通知他子进程万事了
          kill(getppid(),SIGUSR1);
          _exit(0);
    }else{
        //副进程等待逻辑，没信号就i一直等待
         printf("Parent: waiting for SIGUSR1...\n");
         //没有收到信号就待机
         while(!got_signal){
            //进程原地休眠挂起
            sigsuspend(&old_set);
         }
          printf("Parent: got SIGUSR1, now continue.\n");

          //把信号屏蔽恢复到最原始的样子，不留配置
          //设置新状态是old,不保存了
          sigprocmask(SIG_SETMASK,&old_set,NULL);
          //等待子进程万事，回收子进程
          waitpid(pid,NULL,0);
    }
    return 0;


}