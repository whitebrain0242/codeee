#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <fcntl.h>
#include <sys/wait.h>

#define MAX_LINE 1024
#define MAX_ARGS 64

// 完成minishell,支持
//  ls
//  ls -l
//  pwd
//  cd ..
//  echo hello
//  ls > out.txt
//  exit

// shell是一个死循环
// 1.打印提示符>
// 2.读用户输入的一行字符串
// 3.输入分割成命令加参数
// 4.内置命令->自己执行
// 5.外部命令->创建子进程去执行


//解析命令行
static void parse_line(char *line, char *args[]){
    int i=0;
    char*token=strtok(line," \t\n");
    while(token!=NULL&&i<MAX_ARGS-1){
        args[i++]=token;
        token=strtok(NULL," \t\n");
    }
    args[i]=NULL;

}

//重定向：输出搬到文件里
static int handle_redirection(char *args[]){
    for(int i=0;args[i]!=NULL;i++){
        //先去找>重定向符号
        if(strcmp(args[i],">")==0){
            //检查有没有文化后面
            if(args[i+1]==NULL){
                fprintf(stderr, "mini-shell: missing filename after >\n");
                return -1;
            }
            int fd=open(args[i + 1],O_CREAT | O_WRONLY | O_TRUNC,0644);
            if(fd==-1){
                perror("open");
                return -1;
            }
            //把标准输出指向fd
            if(dup2(fd,STDOUT_FILENO)==-1){
                perror("dup2");
                close(fd);
                return -1;
            }
            close(fd);

            //把>变成NULL避免exec把>当作参数
            args[i] = NULL;
            return 0;
        }
    }
    return 0;
}
int main()
{
    char line[MAX_LINE];
    char *args[MAX_ARGS];

    while (1)
    {
        printf("mini_shell-> ");
        fflush(stdout); // 把缓冲区里的输出 立刻打印到屏幕上！
        if (fgets(line, 1024, stdin) == NULL)
        {
             printf("\n");
            break;
        }
        // 解析
        parse_line(line,args);

        if (args[0] == NULL)
            continue;

        // exit

        if (strcmp(args[0], "exit") == 0)
        {
            break;
        }

        // cd
        if (strcmp(args[0], "cd") == 0)
        {
            const char *target = args[1];
            
            if (target == NULL) {
                //获取当前用户的加目录
                target = getenv("HOME");
            }
            //如果连home都没有就报错推出
            if (target == NULL) {
                fprintf(stderr, "mini-shell: HOME not set\n");
                continue;
            }


            //切换目录失败-》打印错误原因
            if (chdir(target) == -1)
            {
                perror("cd");
            }
            continue;
        }
        // 创建子进程
        pid_t pid = fork();
        if (pid == -1)
        {
            perror("fork");
            continue;
        }
        if (pid == 0)
        {
            //处理重定向
            if (handle_redirection(args) == -1) {
                _exit(1);
            }
            //执行 ls、cat、echo 
            //把紫禁城替换成要执行的命令
            execvp(args[0], args);
            perror("execvp");
            _exit(127);
        }
        else
        {
            int status;
            if(waitpid(pid,&status,0)==-1){
                perror("waitpid");
                continue;
            }
            if(WIFEXITED(status)){
                int code=WEXITSTATUS(status);
                if(code!=0){
                    printf("mini-shell: command exited with status %d\n",code);

                }
            }else if(WIFSIGNALED(status)){
                printf("mini-shell: command killed by signal %d\n",WTERMSIG(status));
            }
            waitpid(pid, NULL, 0);
        }
    }
    return 0;
}