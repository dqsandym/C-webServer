#include "http_conn.h"

int http_conn::m_epollfd = -1;// 所有socket上的事件都被注册到同一个epoll内核事件中，所以设置成静态的
int http_conn::m_user_count = 0;// 所有的客户数
 
// 定义HTTP响应的一些状态信息
const char* ok_200_title = "OK";
const char* error_400_title = "Bad Request";
const char* error_400_form = "Your request has bad syntax or is inherently impossible to satisfy.\n";
const char* error_403_title = "Forbidden";
const char* error_403_form = "You do not have permission to get file from this server.\n";
const char* error_404_title = "Not Found";
const char* error_404_form = "The requested file was not found on this server.\n";
const char* error_500_title = "Internal Error";
const char* error_500_form = "There was an unusual problem serving the requested file.\n";

//网站的根目录
const char* doc_root = "/home/werther/vs_code/Webserver/resource";

//设置文件描述符非阻塞
int setnonblocking(int fd)
{
    int old_flag = fcntl(fd, F_GETFL);
    int new_flag = old_flag | O_NONBLOCK;
    fcntl(fd,F_SETFL, new_flag);
    return old_flag;
}

//向epoll中添加需要监听的文件描述符
void addfd(int epollfd, int fd, bool one_shot)
{
    epoll_event event;
    event.data.fd = fd;
    /*
        在使用2.6.17之后的版本内核的服务器系统中，
        对端连接断开触发的epoll事件会包含  EPOLLIN | EPOLLRDHUP，
        有了这个事件，对端断开连接的异常就可以在底层进行处理了，不用在移交到上层
    */
    event.events = EPOLLIN | EPOLLRDHUP;
    //event.events = EPOLLIN | EPOLLET | EPOLLRDHUP;

    if(one_shot)
    {   
        //防止同一个通信被不同的线程处理
        event.events |= EPOLLONESHOT;
    }

    epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &event);

    setnonblocking(fd);

}

//从epoll中移除监听的文件描述符
void removefd(int epollfd, int fd)
{
    epoll_ctl(epollfd, EPOLL_CTL_DEL, fd ,0);
    close(fd);
}

// 修改文件描述符，重置socket上的EPOLLONESHOT事件，以确保下一次可读时，EPOLLIN事件能被触发
void modfd(int epollfd, int fd, int ev){

    epoll_event event;
    event.data.fd = fd;
    event.events = ev | EPOLLET | EPOLLONESHOT | EPOLLRDHUP;
    epoll_ctl(epollfd, EPOLL_CTL_MOD, fd, &event); 

}

//关闭连接
void http_conn::close_conn()
{
    if(m_sockfd != -1)
    {
        removefd(m_epollfd, m_sockfd);
        m_sockfd = -1;
        m_user_count--;//关闭一个连接，将客户总数量-1
    }
}

// 初始化连接,外部调用初始化套接字地址
void http_conn::init(int sockfd, const sockaddr_in & addr)
{
    m_sockfd = sockfd;
    m_address = addr;

    //2MSL:主动关闭一方会有
    /*端口复用：
        在server的TCP连接没有完全断开之前不允许重新监听是不合理的。
        因为，TCP连接没有完全断开指的是connfd（127.0.0.1:6666）没有完全断开，
        而我们重新监听的是lis-tenfd（0.0.0.0:6666），虽然是占用同一个端口，但IP地址不同，
        connfd对应的是与某个客户端通讯的一个具体的IP地址，而listenfd对应的是wildcard address。
        解决这个问题的方法是使用setsockopt()设置socket描述符的选项SO_REUSEADDR为1，
        表示允许创建端口号相同但IP地址不同的多个socket描述符。
    */
    int reuse = 1;
    setsockopt(m_sockfd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    addfd(m_epollfd, m_sockfd, true);

    m_user_count++;

    init();
}

void http_conn::init()
{
    bytes_have_send = 0;
    bytes_to_send = 0; 

    m_check_state = CHECK_STATE_REQUESTLINE;    // 初始状态为检查请求行

    m_method = GET;                         // 默认请求方式为GET
    m_url = 0;
    m_version = 0;
    m_linger = false;                       // 默认不保持链接  Connection : keep-alive保持连接
    m_host = 0;
    m_content_length = 0;

    m_checked_index = 0;
    m_start_line = 0;
    m_read_idx = 0;
    m_write_idx = 0;

    bzero(m_read_buf, READ_BUFFER_SIZE);
    bzero(m_write_buf, WRITE_BUFFER_SIZE);
    bzero(m_real_file, FILENAME_LEN);
}

// 循环读取客户数据，直到无数据可读或者对方关闭连接
bool http_conn::read()
{
    if(m_read_idx >= READ_BUFFER_SIZE)
    {
        return false;
    }
    int read_bytes = 0;
    while(true)
    {
        // 从m_read_buf + m_read_idx索引出开始保存数据，大小是READ_BUFFER_SIZE - m_read_idx
        read_bytes = recv(m_sockfd,m_read_buf + m_read_idx,
                            READ_BUFFER_SIZE - m_read_idx,0);

        if(read_bytes == -1)
        {
            //非阻塞读：如果你连续做read操作而没有数据可读。此时程序不会阻塞起来等待数据准备就绪返回，
            //read函数会返回一个错误EAGAIN，提示你的应用程序现在没有数据可读请稍后再试。
            if(errno == EAGAIN || errno == EWOULDBLOCK)
            {
                //读完了，没有数据
                break;
            }
            return false;
        }
        else if(read_bytes == 0)//对方关闭连接
        {
            return false;
        }   
        m_read_idx += read_bytes;  //改变读到的索引值=当前的偏移量+实际读到的字节数
    }
    //printf("read data: %s\n",m_read_buf);
    return true;
}

//由线程池中的工作线程调用，这是处理HTTP请求的入口函数
void http_conn::process()
{
    //解析HTTP请求
    HTTP_CODE read_ret = process_read();
    if(read_ret == NO_REQUEST)
    {
        modfd(m_epollfd, m_sockfd, EPOLLIN);//请求不完整，需要继续接收，因此监听读事件
        return;
    }

    //生成HTTP响应:根据解析结果进行响应
    bool write_ret = process_write( read_ret );
    if(!write_ret)
    {
        close_conn();
    }
    modfd(m_epollfd,m_sockfd,EPOLLOUT);

}

//主状态机，解析请求 ：  请求行\r\n请求头\r\n\r\n请求体\r\n
http_conn::HTTP_CODE http_conn::process_read()
{
    LINE_STATUS line_status = LINE_OK;//一行一行的读取，因此每行判断读到的状态
    HTTP_CODE ret = NO_REQUEST;
    
    char * text = 0;//指针，指向读缓冲区

    while(((m_check_state == CHECK_STATE_CONTENT) &&(line_status == LINE_OK)) 
            || ((line_status = parse_line()) == LINE_OK ))
    {
        text = get_line();//获取一行数据,下次函数的返回值 就指向  下一行数据的开头了

        m_start_line = m_checked_index;//一行的末尾
        printf("got 1 http line : %s\n",text);

        switch (m_check_state)
        {
            case CHECK_STATE_REQUESTLINE:
            {
                ret = parse_request_line(text);
                if(ret == BAD_REQUEST){
                    return BAD_REQUEST;
                }
                break;
            }
            case CHECK_STATE_HEADER:
            {
                ret = parse_headers(text);
                if(ret == BAD_REQUEST)
                {
                    return BAD_REQUEST;
                }
                else if(ret == GET_REQUEST) //没有请求体时，HTTP请求 结束
                {
                    return do_request();
                }
                break;
            }
            case CHECK_STATE_CONTENT:
            {
              ret = parse_content(text);
              if(ret == GET_REQUEST)   //有请求体时，HTTP请求 结束
                {
                    return do_request();
                }

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

//解析一行数据，判断依据  \r\n ,并 把每一行末尾 \r\n -->  \0 ：字符串结束符
//同时m_checked_index  指向一行的末尾
http_conn::LINE_STATUS http_conn::parse_line()
{
    char temp;
    
    for(; m_checked_index < m_read_idx; ++m_checked_index)
    {
        temp = m_read_buf[m_checked_index];
        if(temp == '\r')
        {
            if((m_checked_index + 1 ) == m_read_idx )
            {
                return LINE_OPEN;
            }
            else if(m_read_buf[m_checked_index +1 ] == '\n')  //读到一行的末尾了
            {
                m_read_buf[m_checked_index++] = '\0';  //把  '\r' 变成  '\0' ，字符串的结束符
                m_read_buf[m_checked_index++] = '\0';  //把  '\n' 变成  '\0'

                return LINE_OK;
            }

            return LINE_BAD;
        }
        else if(temp == '\n')
        {
            if((m_checked_index > 1) && (m_read_buf[m_checked_index - 1] == '\r'))
            {
                m_read_buf[m_checked_index-1] = '\0';
                m_read_buf[m_checked_index++] = '\0';
                return LINE_OK;
            }

            return LINE_BAD;
        }
    }
    return LINE_OPEN;
}

//解析HTTP请求行：请求方法 /目标URL /HTTP版本号   GET /index.html HTTP/1.1
http_conn::HTTP_CODE http_conn::parse_request_line(char * text)
{
    //    \t是制表符
    //strpbrk()函数检索两个字符串中首个相同字符的位置
    //strpbrk()从s1的第一个字符向后检索，直到'\0'，如果当前字符存在于s2中，那么返回当前字符的地址，并停止检索。
    m_url = strpbrk(text, " \t");//找空格

    //GET /index.html HTTP/1.1   -->    GET\0/index.html HTTP/1.1 
    *m_url++ = '\0';//  '\0' 字符串结束符

    // text现在是  GET\0/index.html HTTP/1.1 
    //遇到  \0 表示结束  即 text = “GET”；
    char * method = text;

    //strcasecmp函数
    //【1】函数功能 ：比较参数s1和s2字符串，比较时会自动忽略大小写的差异。
    //【2】返回值： 若参数s1和s2字符串相等则返回0。s1大于s2则返回大于0 的值，s1 小于s2 则返回小于0的值。
    if(strcasecmp(method,"GET") == 0 )
    {
        m_method = GET;
    }
    else{
        return BAD_REQUEST;
    }

    // m_url = "/index.html HTTP/1.1";
    m_version = strpbrk(m_url," \t"); //从 text 的 m_url位置开始向后找 空格“ ”
    if(!m_version)
    {
        return BAD_REQUEST;
    }
    //  "/index.html\0HTTP/1.1";
    *m_version++ = '\0';

    //  m_version = "HTTP/1.1\0";
    if(strcasecmp(m_version,"HTTP/1.1") != 0)
    {
        return BAD_REQUEST;
    }

    //函数说明：strncasecmp()用来比较参数s1和s2字符串前n个字符，比较时会自动忽略大小写的差异
    //返回值   ：若参数s1和s2字符串相同则返回0 s1若大于s2则返回大于0的值 s1若小于s2则返回小于0的值
    //    http://192.168.1.1:10000/index.html
    if(strncasecmp(m_url,"http://",7) == 0)
    {
        m_url += 7;

        //函数原型: char* strchr(char*   str,char   ch);
        //函数功能: 找出str指向的字符串中第一次出现字符ch的位置
        //函数返回: 返回指向该位置的指针,如找不到,则返回空指针
        //m_url = 192.168.1.1:10000/index.html
        m_url = strchr(m_url,'/'); //   m_url = /index.html

    }

    if(!m_url || m_url[0] != '/')
    {
        return BAD_REQUEST;
    }

    m_check_state = CHECK_STATE_HEADER; //主状态机变成： 检查状态请求头

    return NO_REQUEST;

    //也可以使用正则方式
}

//解析HTTP请求头部信息
http_conn::HTTP_CODE http_conn::parse_headers(char * text)
{
    //遇到空行，表示头部信息解析完
    if(text[0] == '\0')
    {
        //如果HTTP请求 有 请求体，则还需要读取 m_content_length字节的消息体
        //主状态机 转移到 CHECK_STATE_CONTENT状态
        if(m_content_length != 0)
        {
            m_check_state = CHECK_STATE_CONTENT;
            return NO_REQUEST;
        }

        //否则说明我们已经得到一个完整的HTTP请求
        return GET_REQUEST;
    }
    else if(strncasecmp(text,"connection:",11) == 0 )
    {
        //处理connection头部字段
        //Connection: keep-alive
        text +=  11;

        // text = " keep-alive";
        //处理空格
        //函数原型为size_t strspn(const char *str, const char * accept);
        //strspn()函数：计算字符串str中连续有几个字符都属于字符串accept
        text += strspn(text," \t");//text = "keep-alive";

        if(strcasecmp(text,"keep-alive") == 0)
        {
            m_linger = true;
        }
    }
    else if(strncasecmp(text,"Content-Length:",15) == 0)
    {
        text += 15;
        text += strspn(text," \t");
        //函数名: atol
        //功 能: 把字符串转换成长整型数
        m_content_length = atol(text);
    }
    else if(strncasecmp(text,"Host:",5) == 0)
    {
        //处理Host头部字段
        //Host: 192.168.88.217:10000\0
        text += 5;
        text += strspn(text," \t");
        m_host = text;
    }
    else{
        
        printf("oop! unkown head %s",text);
    }
    return NO_REQUEST;
}

//解析请求体：只是判断是否被完整的读入
http_conn::HTTP_CODE http_conn::parse_content(char * text)
{
    if(m_read_idx >= (m_content_length + m_checked_index))
    {
        m_read_buf[m_content_length] = '\0';
        return GET_REQUEST;
    }
    return NO_REQUEST;
}

//当得到一个完整的、正确的HTTP请求时，我们就分析目标文件的属性，
//如果目标文件存在、对所有用户可读，且不是目录，则
//调用mmap，将其映射到内存地址 m_file_address处，告诉调用者获取文件成功
http_conn::HTTP_CODE http_conn::do_request()
{
    //   /home/werther/vs_code/Webserver/resource/index.html
    //到服务器本地去寻找资源
    //原型：char *strcpy(char *dest, const char *src)
    //作用： strcpy函数的作用是把含有转义字符\0即空字符作为结束符，然后把src该字符串复制到dest
    //doc_root = "/home/werther/vs_code/Webserver/resource";
    strcpy(m_real_file, doc_root);
    int len = strlen(doc_root);
    // m_url = "/index.html\0";
    //原型：char * strncpy ( char * destination, const char * source, size_t num );
    //函数功能:将第source串的前n个字符拷贝到destination串
    strncpy(m_real_file+len,m_url,FILENAME_LEN -len -1);

    //获取m_real_file文件的相关的状态信息， -1失败 ，0成功
    /*
        定义函数:    int stat(const char *file_name, struct stat *buf);
        函数说明:    通过文件名filename获取文件信息，并保存在buf所指的结构体stat中
        返回值:     执行成功则返回0，失败返回-1，错误代码存于errno
    */
    if(stat(m_real_file,&m_file_stat) < 0)
    {
        return NO_RESOURCE;
    }

    //判断访问权限
    if(!(m_file_stat.st_mode & S_IROTH))//其他用户具可读取权限
    {
        return FORBIDDEN_REQUEST;
    }

    //判断是否是目录
    if( S_ISDIR( m_file_stat.st_mode))// S_ISDIR (st_mode)    是否为目录
    {
        return BAD_REQUEST;
    }

    //以只读方式打开文件
    int fd = open(m_real_file,O_RDONLY);
    //创建内存映射：把网页的数据映射到m_file_address上
    //函数作用：mmap将一个文件或者其它对象映射进内存。文件被映射到多个页上，如果文件的大小不是所有页的大小之和，
    //         最后一个页不被使用的空间将会清零。mmap在用户空间映射调用系统中作用很大。
    //函数原型
    //      void* mmap(void* start,size_t length,int prot,int flags,int fd,off_t offset);
    
    m_file_address = (char* )mmap(0,m_file_stat.st_size, PROT_READ,MAP_PRIVATE,fd,0);
    close(fd);
    return FILE_REQUEST;
}

//对内存映射区执行 munmap操作
void http_conn::unmap()
{
    if( m_file_address )
    {
        // 函数作用：munmap函数释放由mmap创建的这段内存空间
        // 函数原型：int munmap(void* start,size_t length);
        munmap(m_file_address, m_file_stat.st_size);
        m_file_address = 0;
    }
}

//根据服务器处理HTTP请求的结果，决定返回给客户端的内容
bool http_conn::process_write(HTTP_CODE ret)
{
    switch(ret)
    {
        case INTERNAL_ERROR:
            add_status_line(500, error_500_title);
            add_headers(strlen(error_500_form));
            if(!add_content(error_500_form))
            {
                return false;
            }
            break;
        case BAD_REQUEST:
            add_status_line(400, error_400_title);
            add_headers(strlen(error_400_form));
            if(!add_content(error_400_form))
            {
                return false;
            }
            break;
        case NO_RESOURCE:
            add_status_line(404, error_404_title);
            add_headers(strlen(error_404_form));
            if(!add_content(error_404_form))
            {
                return false;
            }
            break;
        case FORBIDDEN_REQUEST:
            add_status_line(404, error_403_title);
            add_headers(strlen(error_403_form));
            if(!add_content(error_403_form))
            {
                return false;
            }
            break;
        case FILE_REQUEST:
            add_status_line(200, ok_200_title);
            add_headers(m_file_stat.st_size);
            m_iv[0].iov_base = m_write_buf;//第一块内存的地址
            m_iv[0].iov_len = m_write_idx;//第一块内存的长度
            m_iv[1].iov_base = m_file_address;//第二块内存的地址
            m_iv[1].iov_len = m_file_stat.st_size;//第二块内存的长度
            m_iv_count = 2;//一共有两块内存

            bytes_to_send = m_write_idx + m_file_stat.st_size;

            return true;
        default:
            return false;
    }
    //不是文件请求的话，就只有 m_write_buf 一块内存需要 写
    m_iv[0].iov_base = m_write_buf;
    m_iv[0].iov_len = m_write_idx;
    m_iv_count = 1;
    bytes_to_send = m_write_idx;
    return true;
}

//生成 HTTP应答的  状态行
bool http_conn::add_status_line(int status, const char* title)
{
    return add_response("%s %d %s\r\n","HTTP/1.1",status,title);
}

////生成 HTTP应答的  响应头
bool http_conn::add_headers(int content_len)
{
    add_content_length(content_len);
    add_content_type();
    add_linger();
    add_blank_line();
}

//响应头 的  Content-Length  字段
bool http_conn::add_content_length(int content_len)
{
    return add_response("Content-Length: %d\r\n",content_len);
}

//响应头 的  Conection  字段
bool http_conn::add_linger()
{
    return add_response("Conection: %s\r\n",(m_linger == true) ? "keep-alive" : "close");
}

//响应头 的  Content-Type  字段
bool http_conn::add_content_type()
{
    return add_response("Content-Type: %s\r\n","text/html");
}

//添加空行
bool http_conn::add_blank_line()
{
    return add_response("%s","\r\n");
}

//生成 HTTP应答的  响应正文
bool http_conn::add_content(const char* content)
{
    return add_response( "%s", content );
}

//往写缓冲中写入待发送的数据：组成HTTP响应报文
bool http_conn::add_response(const char* format,...)
{
    if(m_write_idx >= WRITE_BUFFER_SIZE)
    {
        return false;
    }
    //va_list是在C语言中解决变参问题的一组宏，变参问题是指参数的个数不定，可以是传入一个参数也可以是多个;
    //宏定义了一个指针类型，这个指针类型指向参数列表中的参数。
    va_list arg_list;
    //void va_start(va_list ap, last_arg)
    //修改了用va_list申明的指针，比如ap，使这个指针指向了不定长参数列表省略号前的参数。
    va_start(arg_list, format);
    /*        
        int vsnprintf(char *s, size_t n, const char *format, va_list arg)
        功能：将格式化的可变参数列表写入大小的缓冲区，如果在printf上使用格式，
        则使用相同的文本组成字符串，如果使用arg标识的变量则将参数列表中的元素作为字符串存储在char型指针s中。
    */
    int len = vsnprintf(m_write_buf+m_write_idx,WRITE_BUFFER_SIZE -1 -m_write_idx,format,arg_list);
    if( len >= (WRITE_BUFFER_SIZE -1 -m_write_idx))
    {
        return false;
    }
    m_write_idx += len;

    //void va_end(va_list ap)
    //参数列表访问完以后，参数列表指针与其他指针一样，必须收回，否则出现野指针。一般va_start 和va_end配套使用。
    va_end( arg_list );
    return true;

}

//写 HTTP响应 到 socke
bool http_conn::write()
{
    int temp = 0;

    if( bytes_to_send == 0)
    {
        //将要发送的字节数为0，这一次响应结束
        modfd(m_epollfd,m_sockfd,EPOLLIN);
        init();
        return true;
    }

    while(1)
    {
        //分散写
        //writev将多个数据存储在一起，将驻留在两个或更多的不连接的缓冲区中的数据一次写出去
        //我们有两块分散的内存，m_write_buf 和  m_file_address
        temp = writev(m_sockfd, m_iv,m_iv_count);
        if(temp <= -1)
        {
            //如果tcp写缓冲没有空间，则等待下一轮EPOLLOUT事件，虽然在此期间
            //服务器无法立即接收到同一客户的下一个请求，但可以保证连接的完整性
            if(errno == EAGAIN)
            {
                modfd(m_epollfd,m_sockfd,EPOLLOUT);
                return true;
            }
            unmap();
            return false;
        }
        bytes_to_send -= temp;
        bytes_have_send += temp;

        if(bytes_have_send >= m_iv[0].iov_len)
        {
            m_iv[0].iov_len = 0;
            m_iv[1].iov_base = m_file_address + (bytes_have_send - m_write_idx);//条件满足时，bytes_have_send 和 m_write_idx 相等
            m_iv[1].iov_len = bytes_to_send;
        }
        else{
            m_iv[0].iov_base = m_write_buf + bytes_have_send;
            m_iv[0].iov_len = m_iv[0].iov_len - temp;
        }

        if(bytes_to_send <= 0)
        {
            //没有数据要发送了
            //发送 HTTP响应成功，根据HTTP请求中的Connection字段决定是否立即
            unmap();
            modfd(m_epollfd,m_sockfd,EPOLLIN);
            if(m_linger)
            {
                init();               
                return true;
            }
            else
            {
                return false;
            }
        }

    }
}  






