#include "http_conn.h"

const char* ok_200_title = "OK";
const char* error_400_title = "Bad Request";
const char* error_400_form = "Your request has bad syntax or is inherently impossible to satisfy.\n";
const char* error_403_title = "Forbidden";
const char* error_403_form = "You do not have permission to get file from this server.\n";
const char* error_404_title = "Not Found";
const char* error_404_form = "The requested file was not found on this server.\n";
const char* error_500_title = "Internal Error";
const char* error_500_form = "There was an unusual problem serving the requested file.\n";
const char* doc_root = ".";
// 设置文件描述符为非阻塞
int setnonblocking( int fd )
{
    int old_option = fcntl( fd, F_GETFL );
    int new_option = old_option | O_NONBLOCK;
    fcntl( fd, F_SETFL, new_option );
    return old_option;
}
// 向epoll添加文件描述符
void addfd( int epollfd, int fd, bool one_shot )
{
    epoll_event event;
    event.data.fd = fd;
	// 数据可读, 边沿触发,TCP连接关闭
    event.events = EPOLLIN | EPOLLET | EPOLLRDHUP;
	// 最多触发其上的一个可读,可写或异常事件,最多触发一次
    if( one_shot )
    {
        event.events |= EPOLLONESHOT;
    }
    epoll_ctl( epollfd, EPOLL_CTL_ADD, fd, &event );
    setnonblocking( fd );
}
// 移除文件描述符
void removefd( int epollfd, int fd )
{
    epoll_ctl( epollfd, EPOLL_CTL_DEL, fd, 0 );
    close( fd );
}
// 修改文件描述符的监听事件
void modfd( int epollfd, int fd, int ev )
{
    epoll_event event;
    event.data.fd = fd;
    event.events = ev | EPOLLET | EPOLLONESHOT | EPOLLRDHUP;
    epoll_ctl( epollfd, EPOLL_CTL_MOD, fd, &event );
}

int http_conn::m_user_count = 0;
int http_conn::m_epollfd = -1;

void http_conn::close_conn( bool real_close )
{
    if( real_close && ( m_sockfd != -1 ) )
    {
        //modfd( m_epollfd, m_sockfd, EPOLLIN );
        removefd( m_epollfd, m_sockfd );
		close(m_sockfd);
        m_sockfd = -1;
        m_user_count--;
    }
}

void http_conn::init( int sockfd, const sockaddr_in& addr )
{
    m_sockfd = sockfd;
    m_address = addr;
    int error = 0;
    socklen_t len = sizeof( error );
	// 获取并清除错误信息
    getsockopt( m_sockfd, SOL_SOCKET, SO_ERROR, &error, &len );
	// 确保地址复用
    // int reuse = 1;
    // setsockopt( m_sockfd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof( reuse ) );
	// 连接描述符为一次触发,工作线程处理完成后应再次注册
    addfd( m_epollfd, sockfd, true );
    m_user_count++;

    init();
}

void http_conn::init()
{
    m_check_state = CHECK_STATE_REQUESTLINE;
    m_linger = false;

    m_method = GET;
    m_url = 0;
    m_version = 0;
    m_content_length = 0;
    m_host = 0;
    m_start_line = 0;
    m_checked_idx = 0;
    m_read_idx = 0;
    m_write_idx = 0;
    memset( m_read_buf, '\0', READ_BUFFER_SIZE );
    memset( m_write_buf, '\0', WRITE_BUFFER_SIZE );
    memset( m_real_file, '\0', FILENAME_LEN );
	//pid_socket = new int[MAX_FD];
}
// 判断当前是否读取到http请求的一行
http_conn::LINE_STATUS http_conn::parse_line()
{
    char temp;
    for ( ; m_checked_idx < m_read_idx; ++m_checked_idx )
    {
		// 当前字符
        temp = m_read_buf[ m_checked_idx ];
		// 遇到回车符
        if ( temp == '\r' )
        {
			// 到达本次读的字符串的末尾,返回信息不全状态
            if ( ( m_checked_idx + 1 ) == m_read_idx )
            {
                return LINE_OPEN;
            }
			// 下一个字符是换行符,读取到完整的一行请求,返回ok
            else if ( m_read_buf[ m_checked_idx + 1 ] == '\n' )
            {
				// 将回车换行符改为结束符
                m_read_buf[ m_checked_idx++ ] = '\0';
                m_read_buf[ m_checked_idx++ ] = '\0';
                return LINE_OK;
            }
			// 既没有到结尾,又不是换行符,请求出错
            return LINE_BAD;
        }
		// 遇到换行符
        else if( temp == '\n' )
        {
			// 检查前一字符是否是回车符,是则返回就绪状态
            if( ( m_checked_idx > 1 ) && ( m_read_buf[ m_checked_idx - 1 ] == '\r' ) )
            {
                m_read_buf[ m_checked_idx-1 ] = '\0';
                m_read_buf[ m_checked_idx++ ] = '\0';
                return LINE_OK;
            }
			// 否则请求出错
            return LINE_BAD;
        }
    }
	// 未能读取到完整的一行
    return LINE_OPEN;
}

bool http_conn::read()
{
	// 接收数据超出存储空间
    if( m_read_idx >= READ_BUFFER_SIZE )
    {
        return false;
    }

    int bytes_read = 0;
    while( true )
    {
		// 读取接收缓冲区的数据
        bytes_read = recv( m_sockfd, m_read_buf + m_read_idx, READ_BUFFER_SIZE - m_read_idx, 0 );
        if ( bytes_read == -1 )
        {
			// 没有数据可以读取,退出
            if( errno == EAGAIN || errno == EWOULDBLOCK )
            {
                break;
            }
            return false;
        }
        else if ( bytes_read == 0 )
        {
			// 连接关闭,返回
            return false;
        }
		// 继续读取数据
        m_read_idx += bytes_read;
    }
    return true;
}
// 请求示例
// telnet 219.216.110.149 12345
// GET / HTTP/1.1
// GET http://baidu.com/index.html HTTP/1.1
// Host: www.baidu.com
// 解析http请求行
http_conn::HTTP_CODE http_conn::parse_request_line( char* text )
{
	// 寻找第一个空格或者制表符所在位置
    m_url = strpbrk( text, " \t" );
    if ( ! m_url )
    {
		// 没找到则返回错误请求
        return BAD_REQUEST;
    }
	// 将空格或者制表符重新赋值为结束符,并将指针加1
    *m_url++ = '\0';
    // method即为http请求的方法
    char* method = text;
	// 判断该方法是否为get方法,忽略大小写
    if ( strcasecmp( method, "GET" ) == 0 )
    {
        m_method = GET;
    }
    else
    {
        return BAD_REQUEST;
    }
	// 跳过多余的空格和制表符
    m_url += strspn( m_url, " \t" );
	// 找到下一个空格或者制表符所在位置
    m_version = strpbrk( m_url, " \t" );
	// 没找到则为错误请求
    if ( ! m_version )
    {
        return BAD_REQUEST;
    }
	// 将协议版本与前面的请求内容断开
    *m_version++ = '\0';
    m_version += strspn( m_version, " \t" );
	// 确保请求协议是http/1.1
    if ( strcasecmp( m_version, "HTTP/1.1" ) != 0 )
    {
        return BAD_REQUEST;
    }
	// 首先跳过url的开头,然后寻找'/'所在位置
    if ( strncasecmp( m_url, "http://", 7 ) == 0 )
    {
        m_url += 7;
        m_url = strchr( m_url, '/' );
    }
	// 确保找到了url中'/'的位置
    if ( ! m_url || m_url[ 0 ] != '/' )
    {
        return BAD_REQUEST;
    }
	// 如果url只有'/',则显示默认的主页
	if (m_url[strlen(m_url)-1] == '/')
	{
		strcat(m_url, "home.html"); 
	}
	// 状态转移至解析头部信息
    m_check_state = CHECK_STATE_HEADER;
    return NO_REQUEST;
}
// 解析头部信息
http_conn::HTTP_CODE http_conn::parse_headers( char* text )
{
	// 遇到空行,这是请求头部结束的标志
    if( text[ 0 ] == '\0' )
    {
		// 请求方法为head
        if ( m_method == HEAD )
        {
            return GET_REQUEST;
        }
		// 如果接下来的请求内容不为空
        if ( m_content_length != 0 )
        {
			// 转至请求内容处理状态
            m_check_state = CHECK_STATE_CONTENT;
            return NO_REQUEST;
        }
		// 请求内容为空,则已获取完整的请求
        return GET_REQUEST;
    }
	// 处理connection的请求头部
    else if ( strncasecmp( text, "Connection:", 11 ) == 0 )
    {
        text += 11;
        text += strspn( text, " \t" );
        if ( strcasecmp( text, "keep-alive" ) == 0 )
        {
            m_linger = true;
        }
    }
	// 处理请求内容长度
    else if ( strncasecmp( text, "Content-Length:", 15 ) == 0 )
    {
        text += 15;
        text += strspn( text, " \t" );
        m_content_length = atol( text );
    }
	// 处理主机字段
    else if ( strncasecmp( text, "Host:", 5 ) == 0 )
    {
        text += 5;
        text += strspn( text, " \t" );
        m_host = text;
    }
	// 其他的请求头部忽略
    else
    {
        //printf( "oop! unknow header %s\n", text );
    }
	// 未遇到空行则继续分析请求头部
    return NO_REQUEST;

}

http_conn::HTTP_CODE http_conn::parse_content( char* text )
{
    if ( m_read_idx >= ( m_content_length + m_checked_idx ) )
    {
        text[ m_content_length ] = '\0';
        return GET_REQUEST;
    }

    return NO_REQUEST;
}
// 读取http请求并进行处理
http_conn::HTTP_CODE http_conn::process_read()
{
    LINE_STATUS line_status = LINE_OK;
    HTTP_CODE ret = NO_REQUEST;
    char* text = 0;
	// 注意,当处理请求头部完成后,状态转移至CHECK_STATE_CONTENT,此时未继续分析,
	// 只是确认后续收到的数据足够,则直接返回,若数据不够则继续读取
	// 解析行的状态为OK时 或者 分析请求内容字段且行状态为OK时,持续处理
    while ( ( ( m_check_state == CHECK_STATE_CONTENT ) && ( line_status == LINE_OK  ) )
                || ( ( line_status = parse_line() ) == LINE_OK ) )
    {
		// 获取待处理数据的起始位置
        text = get_line();
		// 更新起始偏置量,m_checked_idx指向的是下一行的起始偏置位置
        m_start_line = m_checked_idx;
        printf( "got 1 http line: %s\n", text );
		
		// 状态机转移
        switch ( m_check_state )
        {
            case CHECK_STATE_REQUESTLINE:
            {
				// 首先处理请求行
                ret = parse_request_line( text );
                if ( ret == BAD_REQUEST )
                {
                    return BAD_REQUEST;
                }
                break;
            }
            case CHECK_STATE_HEADER:
            {
				//printf("CHECK_STATE_HEADER\n");
				// 然后处理请求头部
                ret = parse_headers( text );
				//printf("ret:%d  %d\n",ret,GET_REQUEST);
                if ( ret == BAD_REQUEST )
                {
                    return BAD_REQUEST;
                }
                else if ( ret == GET_REQUEST )
                {
					//printf("request header end.Next do request\n");
					// 处理http请求
                    return do_request();
                }
                break;
            }
            case CHECK_STATE_CONTENT:
            {
				//printf("CHECK_STATE_CONTENT\n");
                ret = parse_content( text );
				// 请求数据量足够,则直接响应请求
                if ( ret == GET_REQUEST )
                {
                    return do_request();
                }
				// 否则继续读取分析
                line_status = LINE_OPEN;
                break;
            }
            default:
            {
                return INTERNAL_ERROR;
            }
        }
    }

    return NO_REQUEST;
}

http_conn::HTTP_CODE http_conn::do_request()
{
	// 首先判断是否为动态url
	if (!strstr(m_url, "cgi-bin"))
	{
		// 静态url
		// 首先复制服务器的源地址
		strcpy( m_real_file, doc_root );
		int len = strlen( doc_root );
		// 将请求的url复制到文件的地址变量
		strncpy( m_real_file + len, m_url, FILENAME_LEN - len - 1 );
		printf( "static file directory is: %s\n", m_real_file );
		// 获取请求文件的相关信息,如果请求出错,直接返回
		if ( stat( m_real_file, &m_file_stat ) < 0 )
		{
			return NO_RESOURCE;
		}
		// 确定其他人是否有读权限
		if ( ! ( m_file_stat.st_mode & S_IROTH ) )
		{
			return FORBIDDEN_REQUEST;
		}
		// 确定该地址是否是目录
		if ( S_ISDIR( m_file_stat.st_mode ) )
		{
			return BAD_REQUEST;
		}
		// 以只读方式打开文件
		int fd = open( m_real_file, O_RDONLY );
		// 映射到内存空间
		m_file_address = ( char* )mmap( 0, m_file_stat.st_size, PROT_READ, MAP_PRIVATE, fd, 0 );
		close( fd );
		return FILE_REQUEST;
	}
	else
	{
		//printf("request is dynamic.\n");
		// 动态url
		// 寻找是否有参数分隔符
		char * ptr = index(m_url, '?');                           
		// 如果找到了参数分隔符,就将参数复制到cgiargs
		if (ptr) 
		{
			strcpy(cgiargs, ptr+1);
			*ptr = '\0';
		}
		else 
			strcpy(cgiargs, "");                         //line:netp:parseuri:endextract
		// 分离出文件名
		strcpy( m_real_file, doc_root );
		int len = strlen( doc_root );
		// 将请求的url复制到文件的地址变量
		strncpy( m_real_file + len, m_url, FILENAME_LEN - len - 1 );
		// 获取请求文件的相关信息,如果请求出错,直接返回
		if ( stat( m_real_file, &m_file_stat ) < 0 )
		{
			return NO_RESOURCE;
		}
		/* Serve dynamic content */
		// 验证文件是否为普通文件,以及是否有运行搜索权限
		if (!(S_ISREG(m_file_stat.st_mode)) || !(S_IXUSR & m_file_stat.st_mode)) 
		{
			return FORBIDDEN_REQUEST;
		}
		// 表示服务动态内容
		return DYNAMIC_SERVE;
	}
}

void http_conn::unmap()
{
    if( m_file_address )
    {
		// 释放映射的内存空间
        munmap( m_file_address, m_file_stat.st_size );
        m_file_address = 0;
    }
}

bool http_conn::write()
{
    int temp = 0;
    int bytes_have_send = 0;
    int bytes_to_send = m_write_idx;
    if ( bytes_to_send == 0 )
    {
		// 用epoll监听套接字
        modfd( m_epollfd, m_sockfd, EPOLLIN );
        init();
        return true;
    }

    while( 1 )
    {
        temp = writev( m_sockfd, m_iv, m_iv_count );
        if ( temp <= -1 )
        {
			// 如果写缓冲区没有空间
            if( errno == EAGAIN )
            {
				// 等待写缓冲区有空间
                modfd( m_epollfd, m_sockfd, EPOLLOUT );
                return true;
            }
			// 释放映射的内存空间
            unmap();
            return false;
        }
		// 更新待发送的字节数
        bytes_to_send -= temp;
		// 更新已经发送的字节数
        bytes_have_send += temp;
		// 检查是否发送完毕
        if ( bytes_to_send <= bytes_have_send )
        {
			// 释放映射的内存空间
            unmap();
			printf("write complete.\n");
			// 如果客户要求保持连接
            if( m_linger )
            {
				// 初始化
                init();
				// 继续监听套接字的读事件
                modfd( m_epollfd, m_sockfd, EPOLLIN );
                return true;
            }
            else
            {
				// 客户不要求保持连接
                //modfd( m_epollfd, m_sockfd, EPOLLIN );
                return false;
            } 
        }
    }
}

bool http_conn::add_response( const char* format, ... )
{
	// 如果写缓冲区已满,返回错误
    if( m_write_idx >= WRITE_BUFFER_SIZE )
    {
        return false;
    }
    va_list arg_list;
    va_start( arg_list, format );
	// 将输入参数写入写缓冲区
    int len = vsnprintf( m_write_buf + m_write_idx, WRITE_BUFFER_SIZE - 1 - m_write_idx, format, arg_list );
    if( len >= ( WRITE_BUFFER_SIZE - 1 - m_write_idx ) )
    {
        return false;
    }
    m_write_idx += len;
    va_end( arg_list );
    return true;
}

bool http_conn::add_status_line( int status, const char* title )
{
    return add_response( "%s %d %s\r\n", "HTTP/1.1", status, title );
}

bool http_conn::add_headers( int content_len )
{
    add_content_length( content_len );
    add_linger();
    add_blank_line();
	return true;
}

bool http_conn::add_content_length( int content_len )
{
    return add_response( "Content-Length: %d\r\n", content_len );
}

bool http_conn::add_linger()
{
    return add_response( "Connection: %s\r\n", ( m_linger == true ) ? "keep-alive" : "close" );
}

bool http_conn::add_blank_line()
{
    return add_response( "%s", "\r\n" );
}

bool http_conn::add_content( const char* content )
{
    return add_response( "%s", content );
}
// 根据http请求状态处理http应答的相关内容
bool http_conn::process_write( HTTP_CODE ret )
{
    switch ( ret )
    {
		// 内部错误
        case INTERNAL_ERROR:
        {
			// 应答状态行
            add_status_line( 500, error_500_title );
			// 应答头部
            add_headers( strlen( error_500_form ) );
			// 添加应答内容
            if ( ! add_content( error_500_form ) )
            {
                return false;
            }
            break;
        }
		// 错误请求
        case BAD_REQUEST:
        {
            add_status_line( 400, error_400_title );
            add_headers( strlen( error_400_form ) );
            if ( ! add_content( error_400_form ) )
            {
                return false;
            }
            break;
        }
		// 无该请求资源
        case NO_RESOURCE:
        {
            add_status_line( 404, error_404_title );
            add_headers( strlen( error_404_form ) );
            if ( ! add_content( error_404_form ) )
            {
                return false;
            }
            break;
        }
		// 非法访问
        case FORBIDDEN_REQUEST:
        {
            add_status_line( 403, error_403_title );
            add_headers( strlen( error_403_form ) );
            if ( ! add_content( error_403_form ) )
            {
                return false;
            }
            break;
        }
		// 获取到了相关文件
        case FILE_REQUEST:
        {
            add_status_line( 200, ok_200_title );
            if ( m_file_stat.st_size != 0 )
            {
                add_headers( m_file_stat.st_size );
                m_iv[ 0 ].iov_base = m_write_buf;
                m_iv[ 0 ].iov_len = m_write_idx;
                m_iv[ 1 ].iov_base = m_file_address;
                m_iv[ 1 ].iov_len = m_file_stat.st_size;
                m_iv_count = 2;
                return true;
            }
            else
            {
				// 请求的文件的大小为空
                const char* ok_string = "<html><body></body></html>";
                add_headers( strlen( ok_string ) );
                if ( ! add_content( ok_string ) )
                {
                    return false;
                }
            }
        }
        default:
        {
            return false;
        }
    }
	// 将应答内容存入发送缓冲区
    m_iv[ 0 ].iov_base = m_write_buf;
    m_iv[ 0 ].iov_len = m_write_idx;
    m_iv_count = 1;
    return true;
}
// http的处理函数接口
void http_conn::process()
{
	// 首先读取相应http请求
    HTTP_CODE read_ret = process_read();
	// 请求不完整,继续监听套接字的读事件,本次处理结束
    if ( read_ret == NO_REQUEST )
    {
		// 继续监听套接字的读事件
        modfd( m_epollfd, m_sockfd, EPOLLIN );
        return;
    }
	else if( read_ret == DYNAMIC_SERVE)
	{
		//printf("dynamic request\n");
		// 服务动态内容
		serve_dynamic();
		// 重置连接
		// reset_socket();
		//继续监听套接字的读事件
		// init();
        // modfd( m_epollfd, m_sockfd, EPOLLIN );
        return;
	}
	// 根据请求状态往写缓冲区中写入相应内容
    bool write_ret = process_write( read_ret );
    if ( ! write_ret )
    {
        close_conn();
    }
	// 监听套接字的写事件,后续将由主线程完成数据的发送
    modfd( m_epollfd, m_sockfd, EPOLLOUT );
}

// 服务动态内容
void http_conn::serve_dynamic()
{
	char buf[MAXLINE], *emptylist[] = { NULL };
    /* Return first part of HTTP response */
    sprintf(buf, "HTTP/1.1 200 OK\r\n"); 
    Rio_writen(m_sockfd, buf, strlen(buf));
    sprintf(buf, "Server: Yuntian Web Server\r\n");
    Rio_writen(m_sockfd, buf, strlen(buf));
	int ret = Fork();
    if (ret == 0) 
	{ 
		/* Child */ //line:netp:servedynamic:fork
		/* Real server would set all CGI vars here */
		// 设置环境变量
		setenv("QUERY_STRING", cgiargs, 1); //line:netp:servedynamic:setenv
		Dup2(m_sockfd, STDOUT_FILENO);         /* Redirect stdout to client */ //line:netp:servedynamic:dup2
		// 调用CGI程序,参数列表为空,实际参数通过环境变量传递
		Execve(m_real_file, emptylist, environ); /* Run CGI program */ //line:netp:servedynamic:execve
		printf("child process end.\n");
		exit(0);
    }
	else if(ret == -1)
	{
		// 创建进程出错,关闭连接
		close_conn();
	}
	else
	{
		//close_conn();
		// 父进程
		// 继续监听套接字的读事件
        //modfd( m_epollfd, m_sockfd, EPOLLIN );
		
		pid_socket[ret] = m_sockfd;
		// printf("parent process's child:%d, socket:%d.\n",ret,m_sockfd);
		
		// wait(NULL);
		// reset_socket();
        return;
	}
    //Wait(NULL); /* Parent waits for and reaps child */ //line:netp:servedynamic:wait
}

void http_conn::reset_socket()
{
	//如果客户要求保持连接
	if( m_linger )
	{
		//初始化
		init();
		//继续监听套接字的读事件
		modfd( m_epollfd, m_sockfd, EPOLLIN );
		printf("remain sockfd:%d\n", m_sockfd);
	}
	else
	{
		//客户不要求保持连接
		//modfd( m_epollfd, m_sockfd, EPOLLIN );
		printf("close sockfd:%d\n", m_sockfd);
		close_conn();
	}
	
	//初始化
	// init();
	//继续监听套接字的读事件
	// modfd( m_epollfd, m_sockfd, EPOLLIN );
	return;	
}


