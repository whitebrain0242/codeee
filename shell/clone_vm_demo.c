#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sched.h>
#include <signal.h>
#include <sys/wait.h>
#include <string.h>
#include <errno.h>

#define STACK_SIZE (1024 * 1024)//栈大小1mb
//clone有点类似与fork,不过fork是创立一个独立的子进程
//但是clone可以通过flags决定父子之间有哪些资源可以共享
//而clone的flags参数里面的clone_vm是共享内存地址空间的意思
//子进程会直接改变父进程的变量
static int shared_value = 10;  // 全局静态变量
//给进程传递任意类型数据，void*是万能参数
//clone规定了入口必须接收void*参数，返回int

static int child_func(void*arg){
    (void)arg;//消除未使用参数的警告
    printf("Child: initial shared_value = %d\n",shared_value);
    //修改全局变量
    shared_value=99;
    printf("Child: changed shared_value to %d\n", shared_value);
    return 0;

}
int main(int argc,char*argv[]){
    //1.手动分配任务栈
    char *stack=malloc(STACK_SIZE);
    if(stack==NULL){
        perror("malloc failed");
        return 1;
    }
    //栈向下生长，栈顶指向内存最高地址
    char *stack_top=stack+STACK_SIZE;
    //基础标志：子退出发SIGCHLD,支持waitpid回收
    int flags=SIGCHLD;


    if (argc >= 2 && strcmp(argv[1], "vm") == 0)
    {
        //开起了
        flags |= CLONE_VM;
        printf("Using clone() with CLONE_VM.\n");
    }else{
        printf("Using clone() without CLONE_VM.\n");
    }
    printf("Parent: before clone, shared_value = %d\n", shared_value);
    

    //clone调用
    pid_t pid = clone(child_func, stack_top, flags, NULL);
    if(pid==-1){
        perror("clone failed");
        free(stack);
        return 1;
    }
    //阻塞等待子任务彻底结束
    waitpid(pid,NULL,0);
    printf("Parent: after child exits, shared_value = %d\n", shared_value);
    free(stack);
    return 0;
}
