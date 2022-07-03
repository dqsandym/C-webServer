#ifndef HTTPCONN_H__
#define HTTPCONN_H__

#include <unistd.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/epoll.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <assert.h>
#include <sys/stat.h>
#include <string.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <stdarg.h>
#include <errno.h>
#include "locker.h"
#include <sys/uio.h>


class http_conn{

public:

    static int m_epollfd;
    static int m_user_count;

    static const int READ_BUFFER_SIZE = 2048;
    static const int WRITE_BUFFER_SIZE = 1024;
    static const int FILENAME_LEN = 200;        //文件名的最大长度

    /*
    从状态机的三种可能状态，即行的读取状态：
    LINE_OK     ：读取到一个完整的行
    LINE_BAD    ：行出错
    LINE_OPEN   ：行数据尚且不完整
*/
enum LINE_STATUS {LINE_OK = 0, LINE_BAD, LINE_OPEN};

enum METHOD {GET = 0,POST,HEAD,PUT,DELETE,TRACE,OPTIONS,CONNECT};

    /*
        The state of the master state machine when a client request is parsed
        CHECK_STATE_REQUESTLINE:The request row is currently being parsed
        CHECK_STATE_HEADER:The header field is currently being parsed
        CHECK_STATE_CONTENT:The request body is currently being parsed
    */
enum CHECK_STATE {CHECK_STATE_REQUESTLINE = 0,CHECK_STATE_HEADER,CHECK_STATE_CONTENT};
    
    /*
        Result of parsing HTTP packets
        NO_REQUEST              :请求不完整，需要继续读取客户数据
        GET_REQUEST             :表示获得了一个完整的客户请求
        BAD_REQUEST             :表示客户请求语法错误
        NO_RESOURCE             :表示服务器没有资源
        FORBIDDEN_REQUEST:      :表示客户对资源没有足够的访问权限
        FILE_REQUEST            :文件请求，获取文件成功
        INTERNAL_ERROR          :表示服务器内部错误
        CLOSED_CONNECTION       :表示客户端已经关闭连接了

    */
enum HTTP_CODE {NO_REQUEST,GET_REQUEST,BAD_REQUEST,NO_RESOURCE,FORBIDDEN_REQUEST,FILE_REQUEST,INTERNAL_ERROR,CLOSED_CONNECTION};

public:
    http_conn() {}
    ~http_conn() {}

public:

    void init(int sockfd, const sockaddr_in & addr);//初始化新接受的连接
    void process();                                 //处理客户端请求
    void close_conn();                              //关闭连接
    bool read();                                    //非阻塞读
    bool write();                                   //非阻塞写

private:

    void init();                                    //初始化连接：变量初始化
    HTTP_CODE process_read();                       //解析HTTP请求
    bool process_write(HTTP_CODE ret);              //填充HTTP应答

    // 下面这一组函数被process_read调用以分析HTTP请求

    HTTP_CODE parse_request_line(char * text);
    HTTP_CODE parse_headers(char * text);
    HTTP_CODE parse_content(char * text);
    HTTP_CODE do_request(); //具体的处理HTTP内容 
    char * get_line() {return m_read_buf+m_start_line;}
    LINE_STATUS parse_line();

    // 这一组函数被process_write调用以填充HTTP应答

    bool add_response(const char* format,...);
    bool add_content(const char* content);
    bool add_content_type();
    bool add_content_length(int content_len);
    bool add_status_line(int status, const char* title);
    bool add_headers(int content_len);
    bool add_linger();
    bool add_blank_line();

    void unmap();

private:

    int m_sockfd;                       //该HTTP连接的socket
    sockaddr_in m_address;              //通信的socket地址

    char m_read_buf[READ_BUFFER_SIZE];  //读缓冲区
    int m_read_idx;                     //标识读缓冲区中以及读入的客户端数据的最后一个字节的下一位置
    int m_checked_index;                //当前正在分析的字符在读缓冲区的位置
    int m_start_line;                   //当前正在解析的行的起始位置

    CHECK_STATE m_check_state;          //主状态机当前所处的位置
    METHOD m_method;                    //请求方法

    char m_real_file[200];              //客户请求的目标文件的完整路径，其内容等于 doc_root + m_url,doc_root是网站根目录
  
    char * m_url;                       //请求目标文件的文件名
    char * m_version;                   //协议版本，只支持HTTP1.1   
    char * m_host;                      //主机名
    bool m_linger;                      //HTTP请求是否要保持连接
    int m_content_length;               //HTTP请求的消息体的字节数
    
 
    char m_write_buf[WRITE_BUFFER_SIZE];        //写缓冲区
    int m_write_idx;                            //写缓冲区中待发送的字节数
    char * m_file_address;                      //客户端请求的目标文件被mmap到内存中的起始位置
    struct stat m_file_stat;                    //目标文件的状态。通过它我们可以判断文件是否存在、是否为目录、是否可读，并获取文件大小等信息
    struct iovec m_iv[2];                       //我们将采用writev来执行写操作，所以定义下面两个成员变量
    int m_iv_count;                             //其中 m_iv_count 表示被写内存块的数量；
  
    int bytes_have_send;            //已经发送的字节数
    int bytes_to_send;              //将要发送的字节数
};


#endif