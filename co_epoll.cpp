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

#include "co_epoll.h"
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>

#if !defined( __APPLE__ ) && !defined( __FreeBSD__ )

/*
int epoll_wait(int epfd,struct epoll_event * events,int maxevents,int timeout) 
功能：该函数用于轮询I/O事件的发生
参数： 
epfd:由epoll_create 生成的epoll专用的文件描述符； 
epoll_event:用于回传代处理事件的数组； 
maxevents:每次能处理的事件数； 
timeout:等待I/O事件发生的超时值，-1相当于阻塞，0相当于非阻塞。
*/
int	co_epoll_wait( int epfd,struct co_epoll_res *events,int maxevents,int timeout )
{
	return epoll_wait( epfd,events->events,maxevents,timeout );
}
/*
int epoll_ctl(int epfd, int op, int fd, struct epoll_event *ev) 
功能：该函数用于控制某个epoll文件描述符上的事件，可以注册事件，修改事件，删除事件。 
参数： 
epfd：由 epoll_create 生成的epoll专用的文件描述符； 
op：要进行的操作例如注册事件，可能的取值EPOLL_CTL_ADD 注册、EPOLL_CTL_MOD 修 改、EPOLL_CTL_DEL 删除 
fd：关联的文件描述符； 
ev：指向epoll_event的指针，如果调用成功返回0,不成功返回-1 
*/
int	co_epoll_ctl( int epfd,int op,int fd,struct epoll_event * ev )
{
	return epoll_ctl( epfd,op,fd,ev );
}
/*
int epoll_create(int size) 
功能：生成一个epoll专用的文件描述符，它其实是在内核申请一空间，用来存放你想关注的socket fd上是否发生以及发生了什么事件。
参数：size就是你在这个epoll fd上能关注的最大socket fd数。
*/
int	co_epoll_create( int size )
{
	return epoll_create( size );
}

//给epoll结果集申请空间
struct co_epoll_res *co_epoll_res_alloc( int n )
{
	struct co_epoll_res * ptr = 
		(struct co_epoll_res *)malloc( sizeof( struct co_epoll_res ) );

	ptr->size = n;
	ptr->events = (struct epoll_event*)calloc( 1,n * sizeof( struct epoll_event ) );

	return ptr;

}

//释放epoll结果集的空间
void co_epoll_res_free( struct co_epoll_res * ptr )
{
	if( !ptr ) return;
	if( ptr->events ) free( ptr->events );
	free( ptr );
}

#else
class clsFdMap // million of fd , 1024 * 1024 
{
private:
	static const int row_size = 1024;
	static const int col_size = 1024;

	void **m_pp[ 1024 ];
public:
	clsFdMap()//构造函数
	{
		memset( m_pp,0,sizeof(m_pp) );//清零m_pp数组
	}
	~clsFdMap()//析构函数
	{
		for(int i=0;i<sizeof(m_pp)/sizeof(m_pp[0]);i++)//释放内存空间
		{
			if( m_pp[i] ) 
			{
				free( m_pp[i] );
				m_pp[i] = NULL;
			}
		}
	}
	inline int clear( int fd )
	{
		set( fd,NULL );
		return 0;
	}
	inline int set( int fd,const void * ptr )
	{
		int idx = fd / row_size;
		if( idx < 0 || idx >= sizeof(m_pp)/sizeof(m_pp[0]) )
		{
			assert( __LINE__ == 0 );
			return -__LINE__;
		}
		if( !m_pp[ idx ] ) 
		{
			m_pp[ idx ] = (void**)calloc( 1,sizeof(void*) * col_size );
		}
		m_pp[ idx ][ fd % col_size ] = (void*)ptr;
		return 0;
	}
	inline void *get( int fd )
	{
		int idx = fd / row_size;
		if( idx < 0 || idx >= sizeof(m_pp)/sizeof(m_pp[0]) )
		{
			return NULL;
		}
		void **lp = m_pp[ idx ];
		if( !lp ) return NULL;

		return lp[ fd % col_size ];
	}
};

__thread clsFdMap *s_fd_map = NULL;

static inline clsFdMap *get_fd_map()
{
	if( !s_fd_map )
	{
		s_fd_map = new clsFdMap();
	}
	return s_fd_map;
}

struct kevent_pair_t
{
	int fire_idx;
	int events;
	uint64_t u64;
};
int co_epoll_create( int size )
{
	return kqueue();// 返回kqueue句柄
}

/*
kqueue有三个主要的东西：struct kevent结构体，EV_SET宏以及kevent函数。

1、struct kevent结构体是用于调度的事件
struct kevent {
    uintptr_t ident;// 该事件关联的文件描述符，如socket中的fd句柄
    int16_t filter;  //可以指定监听类型 , 如EVFILT_READ=读，EVFILT_WRITE=写，EVFILT_TIMER=定时器事件，EVFILT_SIGNAL=信号，EVFILT_USER=用户自定义事件
    uint16_t        flags;//操作方式,EV_ADD 添加，EV_DELETE 删除，EV_ENABLE 激活，EV_DISABLE 不激活
    uint32_t        fflags; //第二种操作方式
    intptr_t        data;   //int 型的用户数据，socket 里面它是可读写的数据长度 
    void            *udata; //指针类型的数据，你可以携带任何想携带的附加数据。比如对象 
};

2、EV_SET 是用于初始化kevent结构的便利宏
EV_SET(&kev, ident, filter, flags, fflags, data, udata);

3、kevent函数
int kevent(int kq, 					 // kqueue的句柄
    const struct kevent *changelist, // 是 kevent 的数组，就是一次可以添加多个事件
    int nchanges, // 是 changelist 数组长度
    struct kevent *eventlist, // 是待接收事件的数组，里面是空的，准备给 kqueue 放数据的
    int nevents, // 是 eventlist 数组长度，传了 eventlist参数后，kevent() 将会阻塞等待事件发生才返回，返回的全部事件在 eventlist 数组里面。
    const struct timespec *timeout); // 是阻塞超时时间，超过这个时间就不阻塞了，直接返回
*/
int co_epoll_wait( int epfd,struct co_epoll_res *events,int maxevents,int timeout )
{
	struct timespec t = { 0 };
	if( timeout > 0 )
	{
		t.tv_sec = timeout;
	}
	//监听事件的发生
	int ret = kevent( epfd, 
					NULL, 0, //register null
					events->eventlist, maxevents,//just retrival
					( -1 == timeout ) ? NULL : &t );
	int j = 0;
	for(int i=0;i<ret;i++)
	{
		struct kevent &kev = events->eventlist[i];
		struct kevent_pair_t *ptr = (struct kevent_pair_t*)kev.udata;
		struct epoll_event *ev = events->events + i;
		if( 0 == ptr->fire_idx )
		{
			ptr->fire_idx = i + 1;
			memset( ev,0,sizeof(*ev) );
			++j;
		}
		else
		{
			ev = events->events + ptr->fire_idx - 1;
		}
		if( EVFILT_READ == kev.filter )
		{
			ev->events |= EPOLLIN;
		}
		else if( EVFILT_WRITE == kev.filter )
		{
			ev->events |= EPOLLOUT;
		}
		ev->data.u64 = ptr->u64;
	}
	for(int i=0;i<ret;i++)
	{
		(( struct kevent_pair_t* )(events->eventlist[i].udata) )->fire_idx = 0;
	}
	return j;
}
int co_epoll_del( int epfd,int fd )
{

	struct timespec t = { 0 };
	struct kevent_pair_t *ptr = ( struct kevent_pair_t* )get_fd_map()->get( fd );
	if( !ptr ) return 0;
	if( EPOLLIN & ptr->events )
	{
		struct kevent kev = { 0 };
		kev.ident = fd;
		kev.filter = EVFILT_READ;
		kev.flags = EV_DELETE;
		kevent( epfd,&kev,1, NULL,0,&t );
	}
	if( EPOLLOUT & ptr->events )
	{
		struct kevent kev = { 0 };
		kev.ident = fd;
		kev.filter = EVFILT_WRITE;
		kev.flags = EV_DELETE;
		kevent( epfd,&kev,1, NULL,0,&t );
	}
	get_fd_map()->clear( fd );
	free( ptr );
	return 0;
}
int co_epoll_ctl( int epfd,int op,int fd,struct epoll_event * ev )
{
	if( EPOLL_CTL_DEL == op )
	{
		return co_epoll_del( epfd,fd );
	}

	const int flags = ( EPOLLIN | EPOLLOUT | EPOLLERR | EPOLLHUP );
	if( ev->events & ~flags ) 
	{
		return -1;
	}

	if( EPOLL_CTL_ADD == op && get_fd_map()->get( fd ) )
	{
		errno = EEXIST;
		return -1;
	}
	else if( EPOLL_CTL_MOD == op && !get_fd_map()->get( fd ) )
	{
		errno = ENOENT;
		return -1;
	}

	struct kevent_pair_t *ptr = (struct kevent_pair_t*)get_fd_map()->get( fd );
	if( !ptr )
	{
		ptr = (kevent_pair_t*)calloc(1,sizeof(kevent_pair_t));
		get_fd_map()->set( fd,ptr );
	}

	int ret = 0;
	struct timespec t = { 0 };

	// printf("ptr->events 0x%X\n",ptr->events);

	if( EPOLL_CTL_MOD == op )
	{
		//1.delete if exists
		if( ptr->events & EPOLLIN ) 
		{
			struct kevent kev = { 0 };
			EV_SET( &kev,fd,EVFILT_READ,EV_DELETE,0,0,NULL );
			kevent( epfd, &kev,1, NULL,0, &t );
		}	
		//1.delete if exists
		if( ptr->events & EPOLLOUT ) 
		{
			struct kevent kev = { 0 };
			EV_SET( &kev,fd,EVFILT_WRITE,EV_DELETE,0,0,NULL );
			ret = kevent( epfd, &kev,1, NULL,0, &t );
			// printf("delete write ret %d\n",ret );
		}
	}

	do
	{
		if( ev->events & EPOLLIN )
		{
			
			//2.add
			struct kevent kev = { 0 };
			EV_SET( &kev,fd,EVFILT_READ,EV_ADD,0,0,ptr );
			ret = kevent( epfd, &kev,1, NULL,0, &t );
			if( ret ) break;
		}
		if( ev->events & EPOLLOUT )
		{
				//2.add
			struct kevent kev = { 0 };
			EV_SET( &kev,fd,EVFILT_WRITE,EV_ADD,0,0,ptr );
			ret = kevent( epfd, &kev,1, NULL,0, &t );
			if( ret ) break;
		}
	} while( 0 );
	
	if( ret )
	{
		get_fd_map()->clear( fd );
		free( ptr );
		return ret;
	}

	ptr->events = ev->events;
	ptr->u64 = ev->data.u64;
	 

	return ret;
}

struct co_epoll_res *co_epoll_res_alloc( int n )
{
	struct co_epoll_res * ptr = 
		(struct co_epoll_res *)malloc( sizeof( struct co_epoll_res ) );

	ptr->size = n;
	ptr->events = (struct epoll_event*)calloc( 1,n * sizeof( struct epoll_event ) );
	ptr->eventlist = (struct kevent*)calloc( 1,n * sizeof( struct kevent) );

	return ptr;
}

void co_epoll_res_free( struct co_epoll_res * ptr )
{
	if( !ptr ) return;
	if( ptr->events ) free( ptr->events );
	if( ptr->eventlist ) free( ptr->eventlist );
	free( ptr );
}

#endif


