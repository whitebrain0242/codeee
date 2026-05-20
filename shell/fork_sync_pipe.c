#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/wait.h>
// read() 在没有数据时会阻塞。
// write() 写入数据后，read() 才返回。
int main(void)
{
    int pipefd[2];
    if (pipe(pipefd) == -1)
    {
        perror("pipe");
        return 1;
    }
    pid_t pid = fork();
    if (pid == -1)
    {
        perror("fork");
        return 1;
    }
    if (pid == 0)
    {
        // 先关闭另外一个口
        close(pipefd[0]); // 关闭读口
        printf("Child: doing some important work...\n");
        sleep(2);
        printf("Child: work finished, notifying parent.\n");
        // 传信号，不用等待子进程结束才进行父进程
        char signal_byte = 'X';
        write(pipefd[1], &signal_byte, 1);
        close(pipefd[1]);
        _exit(0);
    }
    else
    {
        close(pipefd[1]); // 关闭写口
        printf("Parent: waiting for child notification...\n");
        char buffer;
        read(pipefd[0], &buffer, 1);
        printf("Parent: received notification. Now parent continues.\n");
        close(pipefd[0]);
        waitpid(pid, NULL, 0);
    }
    return 0;
}