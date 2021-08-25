#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <fcntl.h>
#include <stdlib.h>
#include <cassert>
#include <sys/epoll.h>
#include <unordered_map>


#include "locker.h"
#include "threadpool.h"
#include "http_conn.h"

//#define MAX_FD 65536
#define MAX_EVENT_NUMBER 10000
//using namespace std;
extern int addfd( int epollfd, int fd, bool one_shot );
extern int removefd( int epollfd, int fd );
extern int setnonblocking( int fd );

static int pipefd[2];
int http_conn::pid_socket[MAX_FD]={0};


//SIGCHLD信号处理函数
void handle_child(http_conn* users, int *pid_socket)
{
	pid_t pid;
	int stat;
	while((pid = waitpid(-1, &stat, WNOHANG)) > 0)
	{
		// 对结束的子进程进行善后处理
		if(pid_socket[pid] > 0)
		{
			int socket = pid_socket[pid];
			printf("handle child process:%d, socket:%d.\n",pid,socket);
			users[socket].reset_socket();
			pid_socket[pid] = 0;
		}
	}
	printf("handle_child end.\n");
}
// 信号处理函数
void sig_handler( int sig )
{
    int save_errno = errno;
    int msg = sig;
    send( pipefd[1], ( char* )&msg, 1, 0 );
    errno = save_errno;
}

void addsig( int sig, void( handler )(int), bool restart = true )
{
    struct sigaction sa;
    memset( &sa, '\0', sizeof( sa ) );
	// 设置处理函数
    sa.sa_handler = handler;
	// 设置是否重启
    if( restart )
    {
        sa.sa_flags |= SA_RESTART;
    }
	// 初始化掩码
    sigfillset( &sa.sa_mask );
	// 对信号进行设置
    assert( sigaction( sig, &sa, NULL ) != -1 );
}

void show_error( int connfd, const char* info )
{
	// 首先在服务器终端输出相应错误信息
    printf( "%s", info );
	// 向客户端发送相应错误信息
    send( connfd, info, strlen( info ), 0 );
	// 关闭客户端的套接字
    close( connfd );
}


int main( int argc, char* argv[] )
{
	// 首先检查输入参数的数目是否正确
    if( argc <= 2 )
    {
		// 错误的话则输出本程序的正确用法
        printf( "usage: %s ip_address port_number\n", basename( argv[0] ) );
        return 1;
    }
	// 获取输入的ip地址和端口号
    const char* ip = argv[1];
    int port = atoi( argv[2] );
	// 对于进程收到的管道错误做忽略处理
    addsig( SIGPIPE, SIG_IGN );
	// 创建线程池
    threadpool< http_conn >* pool = NULL;
    try
    {
        pool = new threadpool< http_conn >;
    }
    catch( ... )
    {
        return 1;
    }
	// 用户类的数组
    http_conn* users = new http_conn[ MAX_FD ];
    assert( users );
    int user_count = 0;
	// 创建一个ipv4协议的字节流的套接字
    int listenfd = socket( PF_INET, SOCK_STREAM, 0 );
	// close系统调用立即返回.舍弃TCP缓冲区数据,发送复位报文段
    assert( listenfd >= 0 );
    struct linger tmp = { 1, 0 };
    setsockopt( listenfd, SOL_SOCKET, SO_LINGER, &tmp, sizeof( tmp ) );

    int ret = 0;
    struct sockaddr_in address;
    bzero( &address, sizeof( address ) );
	// 协议族为ipv4协议
    address.sin_family = AF_INET;
	// 将ip地址转换为网络字节顺序
    inet_pton( AF_INET, ip, &address.sin_addr );
	// 将端口转换为网络字节顺序
    address.sin_port = htons( port );
	// 将本地的socket地址与监听描述符listenfd绑定
    ret = bind( listenfd, ( struct sockaddr* )&address, sizeof( address ) );
    assert( ret >= 0 );
	// 将listenfd设置为监听描述符,最大值设为5
    ret = listen( listenfd, 20 );
    assert( ret >= 0 );
	// epoll事件的数组
    epoll_event events[ MAX_EVENT_NUMBER ];
	// 参数被忽略,但必须大于0,创建一个epoll实例
    int epollfd = epoll_create( 5 );
    assert( epollfd != -1 );
	// 将监听描述符添加到epoll队列
    addfd( epollfd, listenfd, false );
	// 客户类也使用同一epoll
    http_conn::m_epollfd = epollfd;
	
	// 创建管道,注册pipefd[0]的可读事件
	ret = socketpair( PF_UNIX, SOCK_STREAM, 0, pipefd );
    assert( ret != -1 );
    setnonblocking( pipefd[1] );
    addfd( epollfd, pipefd[0],false);
	// 注册SIGCHLD的处理函数
	addsig( SIGCHLD, sig_handler);


    while( true )
    {
		// 阻塞等待有事件到来
        int number = epoll_wait( epollfd, events, MAX_EVENT_NUMBER, -1 );
		printf("current user num:%d	event_num: %d\n",http_conn::m_user_count, number);
		// 如果出现错误并且错误类型不是中断错误
        if ( ( number < 0 ) && ( errno != EINTR ) )
        {
            printf( "epoll failure\n" );
            break;
        }

        for ( int i = 0; i < number; i++ )
        {
			// 获取对应的文件描述符
            int sockfd = events[i].data.fd;
			printf("event sockfd:%d.\n", sockfd);
			// 如果是来自监听描述符的事件
            if( sockfd == listenfd )
            {
				printf(" listen event.\n");
                struct sockaddr_in client_address;
                socklen_t client_addrlength = sizeof( client_address );
				// 获得连接描述符
                int connfd = accept( listenfd, ( struct sockaddr* )&client_address, &client_addrlength );
                if ( connfd < 0 )
                {
                    printf( "errno is: %d\n", errno );
                    continue;
                }
				// 检查用户数量是否超出限制
                if( http_conn::m_user_count >= MAX_FD )
                {
                    show_error( connfd, "Internal server busy" );
                    continue;
                }
                // 用户类进行初始化
                users[connfd].init( connfd, client_address );
            }
			
			// 监听信号源的管道可读
			else if( ( sockfd == pipefd[0] ) && ( events[i].events & EPOLLIN ) )
            {
				printf("pipe can be readed.\n");
                int sig;
                char signals[1024];
                ret = recv( pipefd[0], signals, sizeof( signals ), 0 );
                if( ret == -1 )
                {
                    continue;
                }
                else if( ret == 0 )
                {
                    continue;
                }
                else
                {
                    for( int i = 0; i < ret; ++i )
                    {
                        //printf( "I caugh the signal %d\n", signals[i] );
                        // switch( signals[i] )
                        // {
                            // case SIGCHLD:
							     // handle_child(users, http_conn::pid_socket);
								 // break;
                        // }
						if(signals[i] == SIGCHLD)
						{
							handle_child(users, http_conn::pid_socket);
						}
						else
						{
							printf("unknown pipe signal!\n");
						}
                    }
                }
				printf("pipe handle end.\n");
            }
			
			// EPOLLRDHUP: TCP连接被对方关闭,或者对方关闭了写操作
			// EPOLLHUP: 挂起
			// EPOLLERR: 错误
            else if( events[i].events & ( EPOLLRDHUP | EPOLLHUP | EPOLLERR ) )
            {
				printf("error event.\n");
				if(events[i].events & EPOLLRDHUP)
				{
					printf("EPOLLRDHUP.\n");
				}
				else if(events[i].events & EPOLLHUP)
				{
					printf("EPOLLHUP.\n");
				}
				else
				{
					printf("EPOLLERR.\n");
				}
                users[sockfd].close_conn();
            }
			// 数据可读
            else if( events[i].events & EPOLLIN )
            {
				printf("read event.\n");
                if( users[sockfd].read() )
                {
                    pool->append( users + sockfd );
                }
                else
                {
                    users[sockfd].close_conn();
                }
            }
			// 数据可写
            else if( events[i].events & EPOLLOUT )
            {
				printf("write event.\n");
                if( !users[sockfd].write() )
                {
                    users[sockfd].close_conn();
					printf("write error.\n");
                }
            }
            else
            {
				printf("unknown event.\n");
			}
        }
    }

    close( epollfd );
    close( listenfd );
    delete [] users;
    delete pool;
    return 0;
}
