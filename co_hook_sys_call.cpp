/*
* Tencent is pleased to support the open source community by making Libco available.

* Copyright (C) 2014 THL A29 Limited, a Tencent company. All rights reserved.
*
* Licensed under the Apache License, Version 2.0 (the "License"); 
* you may not use this file except in compliance with the License. 
* You may obtain a copy of the License at
*
*	http://www.apache.org/licenses/LICENSE-2.0
*
* Unless required by applicable law or agreed to in writing, 
* software distributed under the License is distributed on an "AS IS" BASIS, 
* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. 
* See the License for the specific language governing permissions and 
* limitations under the License.
*/

#include <sys/socket.h>
#include <sys/time.h>
#include <sys/syscall.h>
#include <sys/un.h>

#include <dlfcn.h>
#include <poll.h>
#include <unistd.h>
#include <fcntl.h>

#include <netinet/in.h>
#include <errno.h>
#include <time.h>

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdarg.h>
#include <pthread.h>

#include <resolv.h>
#include <netdb.h>

#include <time.h>
#include "co_routine.h"
#include "co_routine_inner.h"
#include "co_routine_specific.h"

typedef long long ll64_t;

//还有一个很重要的作用就是在libco中套接字在hook后的fcntl中都设置为非阻塞,这里保存了套接字原有的阻塞属性
struct rpchook_t
{
	int user_flag;// 记录套接字的状态
	struct sockaddr_in dest; //maybe sockaddr_un;    // 套接字目标地址
	int domain; //AF_LOCAL->域套接字 , AF_INET->IP	 // 套接字类型

	struct timeval read_timeout;// 读超时时间
	struct timeval write_timeout;// 写超时时间
};
//????
static inline pid_t GetPid()
{
	char **p = (char**)pthread_self();//pthread_self()获取调用线程ID 
	return p ? *(pid_t*)(p + 18) : getpid();
}
static rpchook_t *g_rpchook_socket_fd[ 102400 ] = { 0 };

//函数指针
/*
int (*socket_pfn_t)(int domain, int type, int protocol);
功能：创建套接字
返回值：成功返回一个文件描述符（注意，这个套接字不能用于和用户进行通信，只能用于listen和accept客户端的连接请求），失败返回-1
参数：   
	domain：用于设置网络通信的域，函数根据这个参数选择通信协议的族。通信协议族在文件sys/socket.h中定义
	type：用于设置套接字通信的类型，主要有SOCKET_STREAM（流式套接字）、SOCK——DGRAM(数据包套接字)等。
	protocol：用于制定某个协议的特定类型，即type类型中的某个类型。通常某协议中只有一种特定类型，这样protocol参数仅能设置为0；
			  但是有些协议有多种特定的类型，就需要设置这个参数来选择特定的类型
*/
typedef int (*socket_pfn_t)(int domain, int type, int protocol);
/*
int (*connect_pfn_t)(int socket, const struct sockaddr *address, socklen_t address_len);
功能：绑定
参数：
	socket—————客户端的socket描述符。
	address—————服务器的socket地址。
	address_len———服务器的socket地址的长度。
返回值：0为成功；-1为失败。
*/
typedef int (*connect_pfn_t)(int socket, const struct sockaddr *address, socklen_t address_len);
//关闭
typedef int (*close_pfn_t)(int fd);
//读
typedef ssize_t (*read_pfn_t)(int fildes, void *buf, size_t nbyte);
//写
typedef ssize_t (*write_pfn_t)(int fildes, const void *buf, size_t nbyte);

typedef ssize_t (*sendto_pfn_t)(int socket, const void *message, size_t length,
	                 int flags, const struct sockaddr *dest_addr,
					               socklen_t dest_len);

typedef ssize_t (*recvfrom_pfn_t)(int socket, void *buffer, size_t length,
	                 int flags, struct sockaddr *address,
					               socklen_t *address_len);

typedef size_t (*send_pfn_t)(int socket, const void *buffer, size_t length, int flags);
typedef ssize_t (*recv_pfn_t)(int socket, void *buffer, size_t length, int flags);
/*
int (*poll_pfn_t)(struct pollfd fds[], nfds_t nfds, int timeout);
功能：用于监测多个等待事件，若事件未发生，进程睡眠，放弃CPU控制权
     若监测的任何一个事件发生，poll将唤醒睡眠的进程，并判断是什么等待事件发生，执行相应的操作。
参数：
	fds：监听的文件描述符数组
	nfds：监听数组的实际有效监听个数
	timeout：超时时长，单位为毫秒，0表示不阻塞，-1表示阻塞等待
返回值：返回满足对应监听事件的文件描述符总个数
*/
typedef int (*poll_pfn_t)(struct pollfd fds[], nfds_t nfds, int timeout);
/*
int (*setsockopt_pfn_t)(int socket, int level, int option_name,const void *option_value, socklen_t option_len);
功能：用于任意类型、任意状态套接口的设置选项值。
参数：
	socket：标识一个套接口的描述字。
	level：选项定义的层次；支持SOL_SOCKET、IPPROTO_TCP、IPPROTO_IP和IPPROTO_IPV6。
	option_name：需设置的选项。
	option_value：指针，指向存放选项待设置的新值的缓冲区。
	option_len：option_value缓冲区长度。
返回值：若无错误发生返回0。否则的话，返回SOCKET_ERROR错误
*/
typedef int (*setsockopt_pfn_t)(int socket, int level, int option_name,const void *option_value, socklen_t option_len);
/*
int (*fcntl_pfn_t)(int fildes, int cmd, ...);
功能：fcntl()用来操作已打开文件的文件描述符。例如：获取或修改其访问模式或状态标志。
参数：
	fildes: 要操作文件的文件描述符。
	cmd: fcntl() 执行的命令常数。
返回值：成功返回 cmd的依赖，否则返回-1。
*/
typedef int (*fcntl_pfn_t)(int fildes, int cmd, ...);
/*
tm *(*localtime_r_pfn_t)( const time_t *timep, struct tm *result );
功能：把从1970-1-1零点零分到当前时间系统所偏移的秒数时间转换为本地时间
参数：
	const time_t *timep 从1970-1-1时计算的utc时间。
	struct tm *result 结构体用于获取返回的时间。
返回值：返回指向tm 结构体的指针，tm结构体是time.h中定义的用于分别存储时间的各个量(年月日等)的结构体
*/
typedef struct tm *(*localtime_r_pfn_t)( const time_t *timep, struct tm *result );
/*
void *(*pthread_getspecific_pfn_t)(pthread_key_t key)
功能：使用pthread_getspecific获取调用线程的键绑定，并将该绑定存储在value指向的位置中
参数：
	key：需要获取数据的键
返回值：pthread_getsepecfic不返回任何错误。
*/
typedef void *(*pthread_getspecific_pfn_t)(pthread_key_t key);
/*
int (*pthread_setspecific_pfn_t)(pthread_key_t key, const void *value);
功能：使用pthread_setspecific可以为指定线程特定数据键设置线程特定绑定
参数：
	key：需要关联的键
	value：指向需要关联的数据
返回值：成功返回0.其他任何返回值都表示出了错误。
*/
typedef int (*pthread_setspecific_pfn_t)(pthread_key_t key, const void *value);
/*
功能：设置name环境变量的值为value，如果name存在且overwrite不为零则更新，否则不变。
参数:
	name为环境变量名称字符串。
	value则为变量内容
	overwrite用来决定是否要改变已存在的环境变量
返回值:执行成功则返回0，有错误发生时返回-1。
*/
typedef int (*setenv_pfn_t)(const char *name, const char *value, int overwrite);
//删除环境变量name的定义,返回值: 成功 0, 失败 -1
typedef int (*unsetenv_pfn_t)(const char *name);
//功能：根据环境变量名，获取环境变量的值
typedef char *(*getenv_pfn_t)(const char *name);
//gethostbyname函数根据域名解析出服务器的ip地址(实现主机名到IP地址的转换)，它返回一个结构体struct hostent 
typedef hostent* (*gethostbyname_pfn_t)(const char *name);
//???
typedef res_state (*__res_state_pfn_t)();
typedef int (*__poll_pfn_t)(struct pollfd fds[], nfds_t nfds, int timeout);

//定义函数指针，并初始化对应的函数指针
/*
void*dlsym(void*handle,constchar*symbol)
功能：根据 动态链接库 操作句柄(handle)与符号(symbol)，返回符号对应的地址。使用这个函数不但可以获取函数地址，也可以获取变量地址
参数：
	handle：由dlopen打开动态链接库后返回的指针；
	symbol：要求获取的函数或全局变量的名称。
注意;函数dlsym的参数RTLD_NEXT可以在对函数实现所在动态库名称未知的情况下完成对库函数的替代
*/
static socket_pfn_t g_sys_socket_func 	= (socket_pfn_t)dlsym(RTLD_NEXT,"socket");
static connect_pfn_t g_sys_connect_func = (connect_pfn_t)dlsym(RTLD_NEXT,"connect");
static close_pfn_t g_sys_close_func 	= (close_pfn_t)dlsym(RTLD_NEXT,"close");

static read_pfn_t g_sys_read_func 		= (read_pfn_t)dlsym(RTLD_NEXT,"read");
static write_pfn_t g_sys_write_func 	= (write_pfn_t)dlsym(RTLD_NEXT,"write");

static sendto_pfn_t g_sys_sendto_func 	= (sendto_pfn_t)dlsym(RTLD_NEXT,"sendto");
static recvfrom_pfn_t g_sys_recvfrom_func = (recvfrom_pfn_t)dlsym(RTLD_NEXT,"recvfrom");

static send_pfn_t g_sys_send_func 		= (send_pfn_t)dlsym(RTLD_NEXT,"send");
static recv_pfn_t g_sys_recv_func 		= (recv_pfn_t)dlsym(RTLD_NEXT,"recv");

static poll_pfn_t g_sys_poll_func 		= (poll_pfn_t)dlsym(RTLD_NEXT,"poll");

static setsockopt_pfn_t g_sys_setsockopt_func 
										= (setsockopt_pfn_t)dlsym(RTLD_NEXT,"setsockopt");
static fcntl_pfn_t g_sys_fcntl_func 	= (fcntl_pfn_t)dlsym(RTLD_NEXT,"fcntl");

static setenv_pfn_t g_sys_setenv_func   = (setenv_pfn_t)dlsym(RTLD_NEXT,"setenv");
static unsetenv_pfn_t g_sys_unsetenv_func = (unsetenv_pfn_t)dlsym(RTLD_NEXT,"unsetenv");
static getenv_pfn_t g_sys_getenv_func   =  (getenv_pfn_t)dlsym(RTLD_NEXT,"getenv");
static __res_state_pfn_t g_sys___res_state_func  = (__res_state_pfn_t)dlsym(RTLD_NEXT,"__res_state");

static gethostbyname_pfn_t g_sys_gethostbyname_func = (gethostbyname_pfn_t)dlsym(RTLD_NEXT, "gethostbyname");

static __poll_pfn_t g_sys___poll_func = (__poll_pfn_t)dlsym(RTLD_NEXT, "__poll");


/*
static pthread_getspecific_pfn_t g_sys_pthread_getspecific_func 
			= (pthread_getspecific_pfn_t)dlsym(RTLD_NEXT,"pthread_getspecific");

static pthread_setspecific_pfn_t g_sys_pthread_setspecific_func 
			= (pthread_setspecific_pfn_t)dlsym(RTLD_NEXT,"pthread_setspecific");

static pthread_rwlock_rdlock_pfn_t g_sys_pthread_rwlock_rdlock_func  
			= (pthread_rwlock_rdlock_pfn_t)dlsym(RTLD_NEXT,"pthread_rwlock_rdlock");

static pthread_rwlock_wrlock_pfn_t g_sys_pthread_rwlock_wrlock_func  
			= (pthread_rwlock_wrlock_pfn_t)dlsym(RTLD_NEXT,"pthread_rwlock_wrlock");

static pthread_rwlock_unlock_pfn_t g_sys_pthread_rwlock_unlock_func  
			= (pthread_rwlock_unlock_pfn_t)dlsym(RTLD_NEXT,"pthread_rwlock_unlock");
*/

//将总共的时钟脉冲数读出【counter()】
static inline unsigned long long get_tick_count()
{
	uint32_t lo, hi;
	__asm__ __volatile__ (
			"rdtscp" : "=a"(lo), "=d"(hi)
			);
	return ((unsigned long long)lo) | (((unsigned long long)hi) << 32);
}

struct rpchook_connagent_head_t
{
    unsigned char    bVersion;
    struct in_addr   iIP;
    unsigned short   hPort;
    unsigned int     iBodyLen;
    unsigned int     iOssAttrID;
    unsigned char    bIsRespNotExist;
	unsigned char    sReserved[6];
}__attribute__((packed));

//该宏用于初始化对应的函数指针
#define HOOK_SYS_FUNC(name) if( !g_sys_##name##_func ) { g_sys_##name##_func = (name##_pfn_t)dlsym(RTLD_NEXT,#name); }

//获取时间
static inline ll64_t diff_ms(struct timeval &begin,struct timeval &end)
{
	ll64_t u = (end.tv_sec - begin.tv_sec) ;
	u *= 1000 * 10;
	u += ( end.tv_usec - begin.tv_usec ) / (  100 );
	return u;
}
//通过传入参数fd获取套接字的原有阻塞属性
static inline rpchook_t * get_by_fd( int fd )
{
	if( fd > -1 && fd < (int)sizeof(g_rpchook_socket_fd) / (int)sizeof(g_rpchook_socket_fd[0]) )
	{
		//前面声明static rpchook_t *g_rpchook_socket_fd[ 102400 ] = { 0 };
		return g_rpchook_socket_fd[ fd ];
	}
	return NULL;
}

static inline rpchook_t * alloc_by_fd( int fd )
{
	if( fd > -1 && fd < (int)sizeof(g_rpchook_socket_fd) / (int)sizeof(g_rpchook_socket_fd[0]) )
	{
		rpchook_t *lp = (rpchook_t*)calloc( 1,sizeof(rpchook_t) );
		lp->read_timeout.tv_sec = 1;
		lp->write_timeout.tv_sec = 1;
		g_rpchook_socket_fd[ fd ] = lp;
		return lp;
	}
	return NULL;
}

static inline void free_by_fd( int fd )
{
	if( fd > -1 && fd < (int)sizeof(g_rpchook_socket_fd) / (int)sizeof(g_rpchook_socket_fd[0]) )
	{
		rpchook_t *lp = g_rpchook_socket_fd[ fd ];
		if( lp )
		{
			g_rpchook_socket_fd[ fd ] = NULL;
			free(lp);	
		}
	}
	return;

}

//hook系统socket函数
/*
使用以下命令在域DOMAIN中创建类型为TYPE的新套接字,协议PROTOCOL。
如果PROTOCOL为零，则会自动选择一个。返回新套接字的文件描述符，如果错误则返回-1。
*/
int socket(int domain, int type, int protocol)
{
	HOOK_SYS_FUNC( socket );

	if( !co_is_enable_sys_hook() )
	{
		return g_sys_socket_func( domain,type,protocol );
	}
	int fd = g_sys_socket_func(domain,type,protocol);
	if( fd < 0 )
	{
		return fd;
	}

	rpchook_t *lp = alloc_by_fd( fd );
	lp->domain = domain;
	//功能：fcntl()用来操作已打开文件的文件描述符。例如：获取或修改其访问模式或状态标志。
	fcntl( fd, F_SETFL, g_sys_fcntl_func(fd, F_GETFL,0 ) );

	return fd;
}

/*
co_accept函数创建并返回了一个新的套接字client_sock,用于与客户端通信。
参数：
	sockfd：套接字的文件描述符，socket()系统调用返回的文件描述符fd
	addr：指向存放地址信息的结构体的首地址
	addrlen：存放地址信息的结构体的大小，其实也就是sizof(struct sockaddr)
*/
int co_accept( int fd, struct sockaddr *addr, socklen_t *len )
{
	int cli = accept( fd,addr,len );
	if( cli < 0 )
	{
		return cli;
	}
	alloc_by_fd( cli );
	return cli;
}

//hook系统connect函数
/*
客户端需要调用connect()连接服务器;
connect和bind的参数形式一致, 区别在于bind的参数是自己的地址, 而connect的参数是对方的地址;
connect()成功返回0,出错返回-1;
*/
int connect(int fd, const struct sockaddr *address, socklen_t address_len)
{
	HOOK_SYS_FUNC( connect );

	if( !co_is_enable_sys_hook() )
	{
		return g_sys_connect_func(fd,address,address_len);
	}

	//1.sys call
	int ret = g_sys_connect_func( fd,address,address_len );

	rpchook_t *lp = get_by_fd( fd );
	if( !lp ) return ret;

	if( sizeof(lp->dest) >= address_len )
	{
		 memcpy( &(lp->dest),address,(int)address_len );
	}
	if( O_NONBLOCK & lp->user_flag ) //O_NONBLOCK：非阻塞
	{
		return ret;
	}
	
	if (!(ret < 0 && errno == EINPROGRESS))//EINPROGRESS：Linux 下非阻塞connect的异常码
	{
		return ret;
	}

	//2.wait
	int pollret = 0;
	struct pollfd pf = { 0 };

	for(int i=0;i<3;i++) //25s * 3 = 75s
	{
		memset( &pf,0,sizeof(pf) );
		pf.fd = fd;
		pf.events = ( POLLOUT | POLLERR | POLLHUP );

		pollret = poll( &pf,1,25000 );

		if( 1 == pollret  )
		{
			break;
		}
	}
	if( pf.revents & POLLOUT ) //connect succ
	{
		errno = 0;
		return 0;
	}

	//3.set errno
	int err = 0;
	socklen_t errlen = sizeof(err);
	getsockopt( fd,SOL_SOCKET,SO_ERROR,&err,&errlen);
	if( err ) 
	{
		errno = err;
	}
	else
	{
		errno = ETIMEDOUT;//ETIMEDOUT:TCP连接错误
	} 
	return ret;
}
//hook函数close()
int close(int fd)
{
	HOOK_SYS_FUNC( close );
	
	if( !co_is_enable_sys_hook() )
	{
		return g_sys_close_func( fd );
	}

	free_by_fd( fd );
	int ret = g_sys_close_func(fd);

	return ret;
}
//hook函数read():从fd中读取nbyte个字节的内容存发buf中去
ssize_t read( int fd, void *buf, size_t nbyte )
{
	HOOK_SYS_FUNC( read );
	// 如果目前线程没有一个协程, 则直接执行系统调用
	if( !co_is_enable_sys_hook() )
	{
		return g_sys_read_func( fd,buf,nbyte );// dlsym以后得到的原函数
	}
	rpchook_t *lp = get_by_fd( fd );// 获取这个文件描述符的详细信息

	if( !lp || ( O_NONBLOCK & lp->user_flag ) ) // 套接字为非阻塞的,直接进行系统调用
	{
		ssize_t ret = g_sys_read_func( fd,buf,nbyte );
		return ret;
	}

	// 套接字阻塞
	 //同步的rpc调用向内核注册该fd的读事件和超时事件，若事件为发生挂起该协程处理其他协程，待事件到达后恢复协程
	int timeout = ( lp->read_timeout.tv_sec * 1000 ) 
				+ ( lp->read_timeout.tv_usec / 1000 );//ms

	struct pollfd pf = { 0 };
	pf.fd = fd;
	pf.events = ( POLLIN | POLLERR | POLLHUP );

	// 调用co_poll, co_poll中会切换协程, 协程被恢复时将会从co_poll中的挂起点继续运行
	int pollret = poll( &pf,1,timeout );
	
	ssize_t readret = g_sys_read_func( fd,(char*)buf ,nbyte );// 套接字准备就绪或者超时 执行hook前的系统调用
	
	//fd不可读，相当于rpc同步时的等待超时，只是在这里是异步超时因为线程并没有挂起而是协程挂起去处理其他协程
	if( readret < 0 )
	{
		co_log_err("CO_ERR: read fd %d ret %ld errno %d poll ret %d timeout %d",
					fd,readret,errno,pollret,timeout);
	}

	return readret;//成功读取
	
}
//hook函数write():write函数将buf中的nbytes字节内容写入文件描述符fd.成功时返回写的字节数.失败时返回-1.
ssize_t write( int fd, const void *buf, size_t nbyte )
{
	HOOK_SYS_FUNC( write );
	
	if( !co_is_enable_sys_hook() )
	{
		return g_sys_write_func( fd,buf,nbyte );
	}
	rpchook_t *lp = get_by_fd( fd );

	if( !lp || ( O_NONBLOCK & lp->user_flag ) )
	{
		ssize_t ret = g_sys_write_func( fd,buf,nbyte );
		return ret;
	}
	size_t wrotelen = 0;
	int timeout = ( lp->write_timeout.tv_sec * 1000 ) 
				+ ( lp->write_timeout.tv_usec / 1000 );

	ssize_t writeret = g_sys_write_func( fd,(const char*)buf + wrotelen,nbyte - wrotelen );

	if (writeret == 0)
	{
		return writeret;
	}

	if( writeret > 0 )
	{
		wrotelen += writeret;	
	}
	while( wrotelen < nbyte )
	{

		struct pollfd pf = { 0 };
		pf.fd = fd;
		pf.events = ( POLLOUT | POLLERR | POLLHUP );
		poll( &pf,1,timeout );

		writeret = g_sys_write_func( fd,(const char*)buf + wrotelen,nbyte - wrotelen );
		
		if( writeret <= 0 )
		{
			break;
		}
		wrotelen += writeret ;
	}
	if (writeret <= 0 && wrotelen == 0)
	{
		return writeret;
	}
	return wrotelen;
}

//sendto与recvfrom通常用于UDP协议
/*
功能：sendto() 用来将数据由指定的socket 传给对方主机. 
参数：
	socket: 为已建好连线的socket, 如果利用UDP协议则不需经过连线操作
	message: 指向欲连线的数据内容
	length:message的长度
	flags：一般设0
	dest_addr：用来指定欲传送的网络地址
	dest_len：为struct sockaddr的结果长度.常常被赋值为sizeof （struct sockaddr）

返回值：成功则返回实际传送出去的字符数, 失败返回－1, 错误原因存于errno 中.
*/
ssize_t sendto(int socket, const void *message, size_t length,
	                 int flags, const struct sockaddr *dest_addr,
					               socklen_t dest_len)
{
	HOOK_SYS_FUNC( sendto );
	if( !co_is_enable_sys_hook() )
	{
		return g_sys_sendto_func( socket,message,length,flags,dest_addr,dest_len );
	}

	rpchook_t *lp = get_by_fd( socket );
	if( !lp || ( O_NONBLOCK & lp->user_flag ) )
	{
		return g_sys_sendto_func( socket,message,length,flags,dest_addr,dest_len );
	}

	ssize_t ret = g_sys_sendto_func( socket,message,length,flags,dest_addr,dest_len );
	if( ret < 0 && EAGAIN == errno )
	{
		int timeout = ( lp->write_timeout.tv_sec * 1000 ) 
					+ ( lp->write_timeout.tv_usec / 1000 );


		struct pollfd pf = { 0 };
		pf.fd = socket;
		pf.events = ( POLLOUT | POLLERR | POLLHUP );
		poll( &pf,1,timeout );

		ret = g_sys_sendto_func( socket,message,length,flags,dest_addr,dest_len );

	}
	return ret;
}
/*
功能：从套接字上接收一个消息。对于recvfrom ，可同时应用于面向连接的和无连接的套接字。
参数：
socket：接收端套接字描述符。
buffer：存放消息接收后的缓冲区。
length：buffer所指缓冲区的容量。
flags：是以下一个或者多个标志的组合体，可通过or操作连在一起
address：指向存放对端地址的区域，如果为NULL，不储存对端地址。
address_len：作为入口参数，指向存放表示from最大容量的内存单元。作为出口参数，指向存放表示from实际长度的内存单元。
*/
ssize_t recvfrom(int socket, void *buffer, size_t length,
	                 int flags, struct sockaddr *address,
					               socklen_t *address_len)
{
	HOOK_SYS_FUNC( recvfrom );
	if( !co_is_enable_sys_hook() )
	{
		return g_sys_recvfrom_func( socket,buffer,length,flags,address,address_len );
	}

	rpchook_t *lp = get_by_fd( socket );
	if( !lp || ( O_NONBLOCK & lp->user_flag ) )
	{
		return g_sys_recvfrom_func( socket,buffer,length,flags,address,address_len );
	}

	int timeout = ( lp->read_timeout.tv_sec * 1000 ) 
				+ ( lp->read_timeout.tv_usec / 1000 );


	struct pollfd pf = { 0 };
	pf.fd = socket;
	pf.events = ( POLLIN | POLLERR | POLLHUP );
	poll( &pf,1,timeout );

	ssize_t ret = g_sys_recvfrom_func( socket,buffer,length,flags,address,address_len );
	return ret;
}

/*
参数：
	socket：指定发送端套接字描述符。
	buffer：存放要发送数据的缓冲区
	length:实际要改善的数据的字节数
	flags：一般设0
*/
ssize_t send(int socket, const void *buffer, size_t length, int flags)
{
	HOOK_SYS_FUNC( send );
	
	if( !co_is_enable_sys_hook() )
	{
		return g_sys_send_func( socket,buffer,length,flags );
	}
	rpchook_t *lp = get_by_fd( socket );

	if( !lp || ( O_NONBLOCK & lp->user_flag ) )
	{
		return g_sys_send_func( socket,buffer,length,flags );
	}
	size_t wrotelen = 0;
	int timeout = ( lp->write_timeout.tv_sec * 1000 ) 
				+ ( lp->write_timeout.tv_usec / 1000 );

	ssize_t writeret = g_sys_send_func( socket,buffer,length,flags );
	if (writeret == 0)
	{
		return writeret;
	}

	if( writeret > 0 )
	{
		wrotelen += writeret;	
	}
	while( wrotelen < length )
	{

		struct pollfd pf = { 0 };
		pf.fd = socket;
		pf.events = ( POLLOUT | POLLERR | POLLHUP );
		poll( &pf,1,timeout );

		writeret = g_sys_send_func( socket,(const char*)buffer + wrotelen,length - wrotelen,flags );
		
		if( writeret <= 0 )
		{
			break;
		}
		wrotelen += writeret ;
	}
	if (writeret <= 0 && wrotelen == 0)
	{
		return writeret;
	}
	return wrotelen;
}

//recv一般只用在面向连接的套接字，几乎等同于recvfrom，只要将recvfrom的第五个参数设置NULL。
ssize_t recv( int socket, void *buffer, size_t length, int flags )
{
	HOOK_SYS_FUNC( recv );
	
	if( !co_is_enable_sys_hook() )
	{
		return g_sys_recv_func( socket,buffer,length,flags );
	}
	rpchook_t *lp = get_by_fd( socket );

	if( !lp || ( O_NONBLOCK & lp->user_flag ) ) 
	{
		return g_sys_recv_func( socket,buffer,length,flags );
	}
	int timeout = ( lp->read_timeout.tv_sec * 1000 ) 
				+ ( lp->read_timeout.tv_usec / 1000 );

	struct pollfd pf = { 0 };
	pf.fd = socket;
	pf.events = ( POLLIN | POLLERR | POLLHUP );

	int pollret = poll( &pf,1,timeout );

	ssize_t readret = g_sys_recv_func( socket,buffer,length,flags );

	if( readret < 0 )
	{
		co_log_err("CO_ERR: read fd %d ret %ld errno %d poll ret %d timeout %d",
					socket,readret,errno,pollret,timeout);
	}

	return readret;
	
}

extern int co_poll_inner( stCoEpoll_t *ctx,struct pollfd fds[], nfds_t nfds, int timeout, poll_pfn_t pollfunc);

// 在hook以后的poll中应该执行协程的调度
int poll(struct pollfd fds[], nfds_t nfds, int timeout)
{

	HOOK_SYS_FUNC( poll );//真正的poll函数指针
	
	if( !co_is_enable_sys_hook() )// 如果本线程不存在协程调用hook前的poll
	{
		return g_sys_poll_func( fds,nfds,timeout );
	}

	return co_poll_inner( co_get_epoll_ct(),fds,nfds,timeout, g_sys_poll_func);

}
/*
int setsockopt(int socket, int level, int option_name,const void *option_value, socklen_t option_len);
功能：用于任意类型、任意状态套接口的设置选项值。
参数：
	socket：标识一个套接口的描述字。
	level：选项定义的层次；支持SOL_SOCKET、IPPROTO_TCP、IPPROTO_IP和IPPROTO_IPV6。
	option_name：需设置的选项。
	option_value：指针，指向存放选项待设置的新值的缓冲区。
	option_len：option_value缓冲区长度。
返回值：若无错误发生返回0。否则的话，返回SOCKET_ERROR错误
*/
int setsockopt(int fd, int level, int option_name,
			                 const void *option_value, socklen_t option_len)
{
	HOOK_SYS_FUNC( setsockopt );

	if( !co_is_enable_sys_hook() )
	{
		return g_sys_setsockopt_func( fd,level,option_name,option_value,option_len );
	}
	rpchook_t *lp = get_by_fd( fd );
	
	if( lp && SOL_SOCKET == level )
	{
		struct timeval *val = (struct timeval*)option_value;
		//SO_RCVTIMEO和SO_SNDTIMEO ，它们分别用来设置socket接收数据超时时间和发送数据超时时间。
		if( SO_RCVTIMEO == option_name  ) 
		{
			memcpy( &lp->read_timeout,val,sizeof(*val) );//拷贝
		}
		else if( SO_SNDTIMEO == option_name )
		{
			memcpy( &lp->write_timeout,val,sizeof(*val) );
		}
	}
	return g_sys_setsockopt_func( fd,level,option_name,option_value,option_len );
}

//功能：fcntl()用来操作已打开文件的文件描述符。例如：获取或修改其访问模式或状态标志。
/*
fcntl函数参数：
fd：文件描述符
cmd：设置的命令
第三个参数arg:可有可无，由第二个参数决定，比如get时候没有，set时候有值
*/
//在无法给出所有传递给函数的参数的类型和数目时，可以使用省略号（...）指定函数参数表
/*
如何获取变长参数?
a）定义一个va_list类型的变量，变量是指向参数的指针。
b）va_start初始化刚定义的变量，第二个参数是最后一个显式声明的参数。
c）va_arg返回变长参数的值，第二个参数是该变长参数的类型。
d）va_end将a)定义的变量重置为NULL。

其中:
type是指要获取的参数的类型，比如int，char *等
arg_ptr是指向参数列表的指针（va_list类型）
prev_param是指最后一个显式声明的参数，以用来获取第一个变长参数的位置。
*/
int fcntl(int fildes, int cmd, ...)
{
	HOOK_SYS_FUNC( fcntl );

	if( fildes < 0 )
	{
		return __LINE__;
	}
	va_list arg_list;//arg_list是va_list 类型的对象，存储了有关额外参数和检索状态的信息
	va_start( arg_list,cmd );//第二个参数cmd是变参表前面紧挨着的一个变量,即“...”之前的那个参数

	int ret = -1;
	rpchook_t *lp = get_by_fd( fildes );
	switch( cmd )
	{
		case F_DUPFD://F_DUPFD   返回新的文件描述符
		{
			int param = va_arg(arg_list,int);//va_arg宏返回下一个额外的参数，是一个类型为 type 的表达式。
			ret = g_sys_fcntl_func( fildes,cmd,param );
			break;
		}
		case F_GETFD:// 取得与文件描述符fd联合的close-on-exec标志
		{
			ret = g_sys_fcntl_func( fildes,cmd );
			break;
		}
		case F_SETFD://  设置close-on-exec标志，该标志以参数arg的FD_CLOEXEC位决定
		{
			int param = va_arg(arg_list,int);
			ret = g_sys_fcntl_func( fildes,cmd,param );
			break;
		}
		case F_GETFL://F_GETFL    取得fd的文件状态标志
		{
			ret = g_sys_fcntl_func( fildes,cmd );
			break;
		}
		case F_SETFL:// F_SETFL  设置给arg描述符状态标志
		{
			int param = va_arg(arg_list,int);
			int flag = param;
			if( co_is_enable_sys_hook() && lp )
			{
				flag |= O_NONBLOCK;
			}
			ret = g_sys_fcntl_func( fildes,cmd,flag );
			if( 0 == ret && lp )
			{
				lp->user_flag = param;
			}
			break;
		}
		case F_GETOWN:// 获得异步I/O所有权
		{
			ret = g_sys_fcntl_func( fildes,cmd );
			break;
		}
		case F_SETOWN:// 设置异步I/O所有权
		{
			int param = va_arg(arg_list,int);
			ret = g_sys_fcntl_func( fildes,cmd,param );
			break;
		}
		case F_GETLK://获得记录锁
		{
			struct flock *param = va_arg(arg_list,struct flock *);
			ret = g_sys_fcntl_func( fildes,cmd,param );
			break;
		}
		case F_SETLK://设置记录锁
		{
			struct flock *param = va_arg(arg_list,struct flock *);
			ret = g_sys_fcntl_func( fildes,cmd,param );
			break;
		}
		case F_SETLKW://设置文件锁
		{
			struct flock *param = va_arg(arg_list,struct flock *);
			ret = g_sys_fcntl_func( fildes,cmd,param );
			break;
		}
	}

	va_end( arg_list );//指针清零的操作

	return ret;
}

struct stCoSysEnv_t
{
	char *name;	//name为环境变量名称字符串。
	char *value;//value则为变量内容
};
struct stCoSysEnvArr_t
{
	stCoSysEnv_t *data;
	size_t cnt;//猜测：cnt=count=data的份数
};

//复制
static stCoSysEnvArr_t *dup_co_sysenv_arr( stCoSysEnvArr_t * arr )
{
	stCoSysEnvArr_t *lp = (stCoSysEnvArr_t*)calloc( sizeof(stCoSysEnvArr_t),1 );	
	if( arr->cnt )
	{
		lp->data = (stCoSysEnv_t*)calloc( sizeof(stCoSysEnv_t) * arr->cnt,1 );
		lp->cnt = arr->cnt;
		memcpy( lp->data,arr->data,sizeof( stCoSysEnv_t ) * arr->cnt );
	}
	return lp;
}

static int co_sysenv_comp(const void *a, const void *b)
{
	//strcmp函数是string compare(字符串比较)的缩写，用于比较两个字符串并根据比较结果返回整数。
	//基本形式为strcmp(str1,str2)，若str1=str2，则返回零；若str1<str2，则返回负数；若str1>str2，则返回正数。
	return strcmp(((stCoSysEnv_t*)a)->name, ((stCoSysEnv_t*)b)->name); 
}
static stCoSysEnvArr_t g_co_sysenv = { 0 };

void co_set_env_list( const char *name[],size_t cnt)
{
	if( g_co_sysenv.data )
	{
		return ;
	}
	g_co_sysenv.data = (stCoSysEnv_t*)calloc( 1,sizeof(stCoSysEnv_t) * cnt  );

	for(size_t i=0;i<cnt;i++)
	{
		if( name[i] && name[i][0] )
		{
			//strdup()会先用malloc()配置与参数s 字符串相同的空间大小,然后将参数s 字符串的内容复制到该内存地址,然后把该地址返回。
			g_co_sysenv.data[ g_co_sysenv.cnt++ ].name = strdup( name[i] );
		}
	}
	if( g_co_sysenv.cnt > 1 )
	{
		/*
		函数功能：qsort()函数的功能是对数组进行排序，数组有nmemb个元素，每个元素大小为size。
		参数:
			base: base指向数组的起始地址，通常该位置传入的是一个数组名
			nmemb: nmemb表示该数组的元素个数
			size:size表示该数组中每个元素的大小（字节数）
			(*compar)(const void *, const void *) : 此为指向比较函数的函数指针，决定了排序的顺序。
		如果compar返回值小于0（< 0），那么p1所指向元素会被排在p2所指向元素的左面；
		如果compar返回值等于0（= 0），那么p1所指向元素与p2所指向元素的顺序不确定；
		如果compar返回值大于0（> 0），那么p1所指向元素会被排在p2所指向元素的右面。
		*/
		qsort( g_co_sysenv.data,g_co_sysenv.cnt,sizeof(stCoSysEnv_t),co_sysenv_comp );
		stCoSysEnv_t *lp = g_co_sysenv.data;
		stCoSysEnv_t *lq = g_co_sysenv.data + 1;
		for(size_t i=1;i<g_co_sysenv.cnt;i++)
		{
			if( strcmp( lp->name,lq->name ) )
			{
				++lp;
				if( lq != lp  )
				{
					*lp = *lq;
				}
			}
			++lq;
		}
		g_co_sysenv.cnt = lp - g_co_sysenv.data + 1;
	}

}
//功能：设置name环境变量的值为value，如果name存在且overwrite不为零则更新，否则不变。
int setenv(const char *n, const char *value, int overwrite)
{
	HOOK_SYS_FUNC( setenv )
	if( co_is_enable_sys_hook() && g_co_sysenv.data )
	{
		stCoRoutine_t *self = co_self();
		if( self )
		{	//void *pvEnv;//保存程序系统环境变量的指针，这个环境变量其实是与hook后的setenv，getenv类函数有关
			if( !self->pvEnv )
			{
				self->pvEnv = dup_co_sysenv_arr( &g_co_sysenv );
			}
			stCoSysEnvArr_t *arr = (stCoSysEnvArr_t*)(self->pvEnv);

			stCoSysEnv_t name = { (char*)n,0 };

			/*
			void *bsearch(const void *key, const void *base, size_t nitems, size_t size, int (*compar)(const void *, const void *))
			参数:
				key -- 指向要查找的元素的指针，类型转换为 void*。
				base -- 指向进行查找的数组的第一个对象的指针，类型转换为 void*。
				nitems -- base 所指向的数组中元素的个数。
				size -- 数组中每个元素的大小，以字节为单位。
				compar -- 用来比较两个元素的函数。
			返回值:如果查找成功，该函数返回一个指向数组中匹配元素的指针，否则返回空指针。
			*/
			stCoSysEnv_t *e = (stCoSysEnv_t*)bsearch( &name,arr->data,arr->cnt,sizeof(name),co_sysenv_comp );

			if( e )
			{
				if( overwrite || !e->value  )//可以更改环境变量或者要更改的环境变量为空时
				{
					if( e->value ) free( e->value );
					e->value = ( value ? strdup( value ) : 0 );
				}
				return 0;
			}
		}

	}
	return g_sys_setenv_func( n,value,overwrite );
}
//删除环境变量name的定义,返回值: 成功 0, 失败 -1
int unsetenv(const char *n)
{
	HOOK_SYS_FUNC( unsetenv )
	if( co_is_enable_sys_hook() && g_co_sysenv.data )
	{
		stCoRoutine_t *self = co_self();
		if( self )
		{
			if( !self->pvEnv )
			{
				self->pvEnv = dup_co_sysenv_arr( &g_co_sysenv );
			}
			stCoSysEnvArr_t *arr = (stCoSysEnvArr_t*)(self->pvEnv);

			stCoSysEnv_t name = { (char*)n,0 };

			stCoSysEnv_t *e = (stCoSysEnv_t*)bsearch( &name,arr->data,arr->cnt,sizeof(name),co_sysenv_comp );

			if( e )
			{
				if( e->value )
				{
					free( e->value );
					e->value = 0;
				}
				return 0;
			}
		}

	}
	return g_sys_unsetenv_func( n );
}
char *getenv( const char *n )
{
	HOOK_SYS_FUNC( getenv )
	if( co_is_enable_sys_hook() && g_co_sysenv.data )
	{
		stCoRoutine_t *self = co_self();

		stCoSysEnv_t name = { (char*)n,0 };

		if( !self->pvEnv )
		{
			self->pvEnv = dup_co_sysenv_arr( &g_co_sysenv );
		}
		stCoSysEnvArr_t *arr = (stCoSysEnvArr_t*)(self->pvEnv);

		stCoSysEnv_t *e = (stCoSysEnv_t*)bsearch( &name,arr->data,arr->cnt,sizeof(name),co_sysenv_comp );

		if( e )
		{
			return e->value;
		}

	}
	return g_sys_getenv_func( n );

}
struct hostent* co_gethostbyname(const char *name);
//gethostbyname函数根据域名解析出服务器的ip地址(实现主机名到IP地址的转换)，它返回一个结构体struct hostent 
struct hostent *gethostbyname(const char *name)
{
	HOOK_SYS_FUNC( gethostbyname );

#if defined( __APPLE__ ) || defined( __FreeBSD__ )
	return g_sys_gethostbyname_func( name );
#else
	if (!co_is_enable_sys_hook())
	{
		return g_sys_gethostbyname_func(name);
	}
	return co_gethostbyname(name);
#endif

}

//???
struct res_state_wrap
{
	struct __res_state state;
};
CO_ROUTINE_SPECIFIC(res_state_wrap, __co_state_wrap);

extern "C"
{
	res_state __res_state() 
	{
		HOOK_SYS_FUNC(__res_state);

		if (!co_is_enable_sys_hook()) 
		{
			return g_sys___res_state_func();
		}

		return &(__co_state_wrap->state);
	}
	int __poll(struct pollfd fds[], nfds_t nfds, int timeout)
	{
		return poll(fds, nfds, timeout);
	}
}

struct hostbuf_wrap 
{
	struct hostent host;
	char* buffer;
	size_t iBufferSize;
	int host_errno;
};

CO_ROUTINE_SPECIFIC(hostbuf_wrap, __co_hostbuf_wrap);

#if !defined( __APPLE__ ) && !defined( __FreeBSD__ )
struct hostent *co_gethostbyname(const char *name)
{
	if (!name)
	{
		return NULL;
	}

	if (__co_hostbuf_wrap->buffer && __co_hostbuf_wrap->iBufferSize > 1024)
	{
		free(__co_hostbuf_wrap->buffer);
		__co_hostbuf_wrap->buffer = NULL;
	}
	if (!__co_hostbuf_wrap->buffer)
	{
		__co_hostbuf_wrap->buffer = (char*)malloc(1024);
		__co_hostbuf_wrap->iBufferSize = 1024;
	}

	struct hostent *host = &__co_hostbuf_wrap->host;
	struct hostent *result = NULL;
	int *h_errnop = &(__co_hostbuf_wrap->host_errno);

	int ret = -1;
	while (ret = gethostbyname_r(name, host, __co_hostbuf_wrap->buffer, 
				__co_hostbuf_wrap->iBufferSize, &result, h_errnop) == ERANGE && 
				*h_errnop == NETDB_INTERNAL )
	{
		free(__co_hostbuf_wrap->buffer);
		__co_hostbuf_wrap->iBufferSize = __co_hostbuf_wrap->iBufferSize * 2;
		__co_hostbuf_wrap->buffer = (char*)malloc(__co_hostbuf_wrap->iBufferSize);
	}

	if (ret == 0 && (host == result)) 
	{
		return host;
	}
	return NULL;
}
#endif

/*
通过在用户代码中包含这个函数void co_enable_hook_sys() 
可以把整个co_hook_sys_call.cpp中的符号表导入我们的项目中，这样也可以做到使用我们自己的库去替换系统的库。
*/
void co_enable_hook_sys() 
{
	stCoRoutine_t *co = GetCurrThreadCo();
	if( co )
	{
		co->cEnableSysHook = 1;
	}
}

