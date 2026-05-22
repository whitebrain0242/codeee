#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
//模拟两个清理函数
static void cleanup_one(void){
    printf("one called\n");
}
static void cleanup_two(void){
    printf("two called\n");
}
int main(int argc,char*argv[]){
    //把函数注册到「程序正常退出执行列表」
    //成功执行返回0
    //重点是先注册的后执行
    if(atexit(cleanup_one)!=0){
        perror("atexit cleanup_one");
        return 1;
    }
    if(atexit(cleanup_two)!=0){
        perror("atexit cleanup_otwo");
        return 1;
    }
    printf("Main function is about to exit.\n");
    //缓冲没有刷新，输出缺失
    if(argc>=2&&strcmp(argv[1],"_exit")==0){
        _exit(0);
    }else{
        exit(0);
    }
    //atexit在进程正常的时候系统会帮你一个个调用
    //但是_exit不执行处理程序，不刷新缓冲去就直接退出了
}