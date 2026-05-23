#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <sys/wait.h>
// exec() 默认不会关闭已打开的文件描述符。
// 除非设置了FD_CLOEXEC
// 这个练习就是看exec后文件描述符是否会被继承
//一旦exec就自动关闭fd,以免泄漏
int main(int argc, char *argv[])
{
    int fd = open("secret_file.txt", O_CREAT | O_RDWR | O_TRUNC, 0644);
    if (fd == -1)
    {
        perror("open");
        return 1;
    }
    write(fd, "This is a secret file.\n", 23);
    printf("Parent: opened secret_file.txt as fd %d\n", fd);

    if (argc >= 2 && strcmp(argv[1], "cloexec") == 0)
    {
        // 用F_GETFD命令获取文件描述符的flags

        int flags = fcntl(fd, F_GETFD);
        if (flags == -1)
        {
            perror("fcntl F_GETFD");
            close(fd);
            return 1;
        }
        flags |= FD_CLOEXEC; // 打开这个位置
        // 把修改好的flags存回fd
        if (fcntl(fd, F_SETFD, flags) == -1)
        {
            perror("fcntl F_SETFD");
            close(fd);
            return 1;
        }
        // 添加在了fd这个文件里面
        printf("Parent: FD_CLOEXEC has been set on fd %d\n", fd);
        
        
        //fork会主动继承fd
        pid_t pid = fork();
        if (pid == -1)
        {
            perror("fork");
            close(fd);
            return 1;
        }
        if (pid == 0)
        {
            printf("Child: executing ls -l /proc/self/fd\n");
            //exec也不会主动关闭文件描述符
            execlp("ls", "ls", "-l", "/proc/self/fd", NULL);
            perror("execlp");
            _exit(127);
        }
        else
        {
            waitpid(pid, NULL, 0);
            close(fd);
        }
        return 0;
    }
}
