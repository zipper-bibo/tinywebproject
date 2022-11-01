#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <cassert>
#include <sys/epoll.h>
#include<signal.h>
#include<cstring>
#define MAX_EVENT_NUMBER 1024
#define MAX_BUFFER_SIZE 1024
#define MAX_FD 65536
#include<time.h>
#include "timer.h"
#include"http_conn.h"

#define TIMESLOT 5

#define listenfdLT //水平触发阻塞

int epollfd,pipefd[2];
static sort_timer_lst timer_lst;

//这三个函数在http_conn.cpp中定义，改变链接属性
extern int addfd(int epollfd, int fd, bool one_shot);
extern int remove(int epollfd, int fd);
extern int setnonblocking(int fd);

void cb_func(client_data *user_data)
{
    epoll_ctl(epollfd, EPOLL_CTL_DEL, user_data->sockfd, 0);
    assert(user_data);
    close(user_data->sockfd);
}

//定时处理任务，重新定时以不断触发SIGALRM信号
void timer_handler()
{
    timer_lst.tick();
    alarm(TIMESLOT);
}

void sig_handler(int sig)
{
    //为保证函数的可重入性，保留原来的errno
    int save_errno = errno;
    int msg = sig;
    send(pipefd[1], (char *)&msg, 1, 0);
    errno = save_errno;
}

void addsig(int sig, void(handler)(int), bool restart = true)
{
    struct sigaction sa;
    memset(&sa, '\0', sizeof(sa));
    sa.sa_handler = handler;
    if (restart)
        sa.sa_flags |= SA_RESTART;
    sigfillset(&sa.sa_mask);
    assert(sigaction(sig, &sa, NULL) != -1);
}


int main(int argc,char*argv[])
{
    
    int port =9091;
    int listenfd;
    struct sockaddr_in address;

    addsig(SIGPIPE, SIG_IGN);

    bzero(&address, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(port);

    int flag=1;int ret; 

    listenfd = socket(PF_INET, SOCK_STREAM, 0);
    
    setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, &flag, sizeof(flag));

    ret = bind(listenfd, (struct sockaddr *)&address, sizeof(address));
    if(ret<0)
    {printf("%s\n", strerror(errno));}

    ret = listen(listenfd, 5);
    assert(ret >= 0);

    epoll_event events[MAX_EVENT_NUMBER];
    epollfd = epoll_create(5);

    assert(epollfd != -1);

        addfd(epollfd, listenfd, false);
	ret = socketpair(PF_UNIX, SOCK_STREAM, 0, pipefd);
    assert(ret != -1);
    setnonblocking(pipefd[1]);
    addfd(epollfd, pipefd[0], false);
    addsig(SIGTERM, sig_handler, false);
    addsig(SIGALRM, sig_handler,false);

	client_data *users_timer = new client_data[MAX_FD]; //user-timer-timer   
	http_conn*users=new http_conn[MAX_FD];
	http_conn::m_epollfd=epollfd;
	//alarm(TIMESLOT);
    bool stop_server=false;
	bool timeout=false;

    while(!stop_server)
    {
	 int number = epoll_wait(epollfd, events, MAX_EVENT_NUMBER, -1);
	 if (number < 0 && errno != EINTR)
        {
            break;
        }
	 for(int i=0;i<number;i++)
	 {
		             int sockfd = events[i].data.fd;
		             if (sockfd == listenfd)
            {
                struct sockaddr_in client_address;
                socklen_t client_addrlength = sizeof(client_address);
                int connfd = accept(listenfd, (struct sockaddr *)&client_address, &client_addrlength);


	    
		if (connfd < 0)
                {
                    printf("accept error");
                    continue;
                }
                //if (client_conn::user_cnt >= MAX_FD)
                //{
                //    printf("Internal server busy");
                //    continue;
                //}
		addfd(epollfd,connfd,false);
		                //初始化client_data数据
                //创建定时器，设置回调函数和超时时间，绑定用户数据，将定时器添加到链表中
                users_timer[connfd].address = client_address;
                users_timer[connfd].sockfd = connfd;
		
		
		users[connfd].init(connfd, client_address);
                util_timer *timer = new util_timer;
                timer->user_data = &users_timer[connfd];
                timer->cb_func = cb_func;
                time_t cur = time(NULL);
                timer->expire = cur + 3 * TIMESLOT;
                users_timer[connfd].timer = timer;
                timer_lst.add_timer(timer);

	 }
	 else if (events[i].events & (EPOLLRDHUP | EPOLLHUP | EPOLLERR))
	 {
		 //doing nothing
		//服务器端关闭连接，移除对应的定时器
                util_timer *timer = users_timer[sockfd].timer;
                timer->cb_func(&users_timer[sockfd]);

                if (timer)
                {
                    timer_lst.del_timer(timer);
                }
	 }

	 else if ((sockfd == pipefd[0]) && (events[i].events & EPOLLIN))
            {
                int sig;
                char signals[1024];
                ret = recv(pipefd[0], signals, sizeof(signals), 0);
                if (ret == -1)
                {
                    continue;
                }
                else if (ret == 0)
                {
                    continue;
                }
                else
                {
                    for (int i = 0; i < ret; ++i)
                    {
                        switch (signals[i])
                        {
			case SIGALRM:
				timeout=true;
				break;
                        case SIGTERM:
                            stop_server = true;
			    break;
                        }
                    }
                }
            }

	     else if (events[i].events & EPOLLIN)
	     {
		util_timer *timer = users_timer[sockfd].timer;
                if (users[sockfd].read_once())
                {
                    //若监测到读事件，将该事件放入请求队列

                    users[sockfd].process();

                    //若有数据传输，则将定时器往后延迟3个单位
                    //并对新的定时器在链表上的位置进行调整
                    if (timer)
                    {
                        time_t cur = time(NULL);
                        timer->expire = cur + 3 * TIMESLOT;

                        timer_lst.adjust_timer(timer);
                    }
                }
                else
                {
                    timer->cb_func(&users_timer[sockfd]);
                    if (timer)
                    {
                        timer_lst.del_timer(timer);
                    }
                }


	     }
	else if (events[i].events & EPOLLOUT)
	{	
			//gotta something to write and have to layout its expire
			                util_timer *timer = users_timer[sockfd].timer;
                if (users[sockfd].write())
                {
                    //LOG_INFO("send data to the client(%s)", inet_ntoa(users[sockfd].get_address()->sin_addr));
                    //Log::get_instance()->flush();

                    //若有数据传输，则将定时器往后延迟3个单位
                    //并对新的定时器在链表上的位置进行调整
                    if (timer)
                    {
                        time_t cur = time(NULL);
                        timer->expire = cur + 3 * TIMESLOT;
                       //LOG_INFO("%s", "adjust timer once");
                       //Log::get_instance()->flush();
                        timer_lst.adjust_timer(timer);
                    }
                }
                else//?
                {
                    timer->cb_func(&users_timer[sockfd]);
                    if (timer)
                    {
                        timer_lst.del_timer(timer);
                    }
                }
	}
	 	}//event circle
	 
	 if(timeout)
	 {
		 //timer_handler();

		 timeout=false;
	 }
	 }//while circle

    close(epollfd);
    close(listenfd);
    close(pipefd[1]);
    close(pipefd[0]);
    delete[] users_timer;
    delete[] users;
    return 0;
}
