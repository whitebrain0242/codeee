#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/wait.h>
#include <errno.h>
// 整体的功能就是
// 传入一个shell命令字符串，创建子进程执行该命令，
// 副进程等待命令执行完成 最后返回执行状态
int my_system(const char *command)
{
    if (command == NULL)
    {
        return 1;
    }
    pid_t pid = fork();
    if (pid == -1)
    {
        return -1;
    }
    if (pid == 0)
    {
        execl("/bin/sh", "sh", "-c", command, NULL);
        _exit(127);
    }
    int status;
    while (waitpid(pid, &status, 0) == -1)
    {
        if (errno == EINTR)
        {
            continue;
        }
        return -1;
    }
    return status;
}
int main(void)
{
    int status = my_system("echo Hello from my_system;ls -l /tmp | head");
    if (status == -1)
    {
        perror("my_system");
        return 1;
    }
    // 如果是正常退出的花
    if (WIFEXITED(status))
    {
        printf("Command exited with status %d\n", WEXITSTATUS(status));
        // 如果是信号杀死的
    }
    else if (WIFSIGNALED(status))
    {
        printf("Command killed by signal %d\n", WTERMSIG(status));
    }
    return 0;
}
