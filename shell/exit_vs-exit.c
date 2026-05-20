#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
//exit() 会刷新 stdio 缓冲区。
//_exit() 不刷新 stdio 缓冲区。

//printf不是每次都马上写到屏幕上面，而是先写道用户态缓冲区
//exit()会帮助你刷新缓冲区
//但是_exit()不管，直接让进程推出了
int main(int argc,char*argv[]){
     printf("This message has no newline...");
     if(argc>=2&&strcmp(argv[1],"_exit")==0){
        _exit(0);
     }else{
        exit(0);
     }
}