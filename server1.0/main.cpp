#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h> 
#include <sys/epoll.h>
#include <libgen.h>
#include "locker.h"
#include "threadpool.h"
#include <signal.h>
#include <string.h>
#include "http_conn.h"

#define MAX_FD 65535            //最大的文件描述符个数
#define MAX_EVENT_NUMBRE 1000   //监听的最大的事件数量


//添加信号
void addsig(int sig, void(handler)(int)){
    struct sigaction sa;
    memset(&sa,'\0',sizeof(sa));
    sa.sa_handler = handler;
    sigfillset(&sa.sa_mask);
    assert( sigaction( sig, &sa, NULL ) != -1 );
}

extern void addfd(int epollfd, int fd, bool one_shot);

extern void removefd(int epollfd, int fd);

//extern void modfd(int epollfd, int fd, int ev);

int main(int argc, char* argv[])
{
    if(argc <= 1)
    {
        printf("usage: %s port_number\n", basename(argv[0]));
        /*
            1、exit用于结束正在运行的整个程序，它将参数返回给OS，把控制权交给操作系统；而return 是退出当前函数，返回函数值，把控制权交给调用函数。
            2. exit是系统调用级别，它表示一个进程的结束；而return 是语言级别的，它表示调用堆栈的返回。
        */
        exit(-1);
    }

    int port = atoi(argv[1]); //获取端口号

    addsig(SIGPIPE, SIG_IGN);

    threadpool<http_conn> * pool = NULL;//防止内存泄露，先指空
    //try/catch 语句用于处理代码中可能出现的错误信息。
    try{
        pool = new threadpool<http_conn>;//为pool分配内存空间
    }catch(...){
        exit(-1);
    }

    http_conn* users = new http_conn[MAX_FD];

    int listenfd = socket(PF_INET, SOCK_STREAM, 0);

    //设置端口复用
    int reuse = 1;
    setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    struct sockaddr_in seraddress;
    seraddress.sin_family = AF_INET;
    seraddress.sin_port = htons(port);
    seraddress.sin_addr.s_addr = INADDR_ANY;

    bind(listenfd, (struct sockaddr*)&seraddress, sizeof(seraddress));
   
    listen(listenfd,5);
    
    //创建epoll对象、事件数组
    epoll_event events[MAX_EVENT_NUMBRE];

    int epollfd = epoll_create(5);

    addfd(epollfd, listenfd,false);//把  监听socket  挂上epoll

    http_conn::m_epollfd = epollfd;

    while(true)
    {
        int num = epoll_wait(epollfd, events, MAX_EVENT_NUMBRE, -1);
        if((num < 0) && (errno != EINTR)) //EINTR:当程序在执行处于阻塞状态的系统调用时接收到信号
        {
            printf("epoll failure\n");
            break;
        }

        for(int i = 0; i< num;i++)
        {
            int sockfd = events[i].data.fd;

            if(sockfd == listenfd){
                    struct sockaddr_in clinet_address;
                    socklen_t client_addrlen = sizeof(clinet_address);
                    int confd = accept(listenfd, (struct sockaddr *)&clinet_address,&client_addrlen);

                    if(http_conn::m_user_count >= MAX_FD){
                        close(confd);
                        continue;
                    }

                    users[confd].init(confd,clinet_address);
            }
            else if(events[i].events & (EPOLLRDHUP | EPOLLHUP | EPOLLERR))
            {
                users[sockfd].close_conn();
            }
            else if(events[i].events & EPOLLIN) //连接socket 有  读事件
            {
                if(users[sockfd].read())
                {
                    pool->append(users+sockfd);
                }
                else{
                    users[sockfd].close_conn();
                }
            }
            else if(events[i].events & EPOLLOUT)//连接socket 有  写事件
            {
                if(!users[sockfd].write())
                {
                    users[sockfd].close_conn();
                }
            }
         
        }


    }

    close(epollfd);
    close(listenfd);

    delete [] users;
    delete pool;

    return 0;
}