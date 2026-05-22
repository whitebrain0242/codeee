#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
int main(void){
    char *argv[] = {
    "printenv",   // 命令名（给程序自己看的）
    "MYCOURSE",   // 要打印的环境变量名
    NULL          // 必须以 NULL 结尾！
};
char *envp[] = {
    "MYCOURSE=Linux_Process_Chapters_24_to_28",
    "PATH=/usr/bin:/bin",
    NULL
};
printf("Before execve: current PID = %ld\n", (long)getpid());

    execve("/usr/bin/printenv", argv, envp);

    perror("execve");
    _exit(127);
}