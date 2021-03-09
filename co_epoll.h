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


#ifndef __CO_EPOLL_H__
#define __CO_EPOLL_H__
#include <stdint.h>
#include <stdlib.h>
#include <assert.h>
#include <string.h>
#include <sys/types.h>
#include <time.h>
#include <time.h>
//__APPLE__和__FreeBSD__是操作系统
#if !defined( __APPLE__ ) && !defined( __FreeBSD__ )

#include <sys/epoll.h>

struct co_epoll_res
{
	int size;
	struct epoll_event *events;
	struct kevent *eventlist;
};
//epoll用到的三个函数
int 	co_epoll_wait( int epfd,struct co_epoll_res *events,int maxevents,int timeout );
int 	co_epoll_ctl( int epfd,int op,int fd,struct epoll_event * );
int 	co_epoll_create( int size );
struct 	co_epoll_res *co_epoll_res_alloc( int n );
void 	co_epoll_res_free( struct co_epoll_res * );

#else

#include <sys/event.h>
enum EPOLL_EVENTS//epoll_event的事件
{
	EPOLLIN = 0X001,//表示对应的文件描述符可以读
	EPOLLPRI = 0X002,//表示对应的文件描述符有紧急的数可读
	EPOLLOUT = 0X004,//表示对应的文件描述符可以写

	EPOLLERR = 0X008,//表示对应的文件描述符发生错误
	EPOLLHUP = 0X010,//表示对应的文件描述符被挂起

    EPOLLRDNORM = 0x40,
    EPOLLWRNORM = 0x004,
};
#define EPOLL_CTL_ADD 1//对fd描述符注册event事件
#define EPOLL_CTL_DEL 2//删除已注册的event事件
#define EPOLL_CTL_MOD 3//对fd描述符的event事件进行修改

typedef union epoll_data//联合体
{
	//一般用法是直接把socket赋给fd即可，但是，有了这个void* ptr的指针，我们就可以在注册socket的时候，传进我们想要的参数。
	void *ptr;
	int fd;
	uint32_t u32;
	uint64_t u64;

} epoll_data_t;//保存触发事件的某个文件描述符相关的数据

struct epoll_event
{
	uint32_t events;//epoll事件
	epoll_data_t data;//用户数据变量
};

struct co_epoll_res
{
	int size;
	struct epoll_event *events;
	struct kevent *eventlist;
};
int 	co_epoll_wait( int epfd,struct co_epoll_res *events,int maxevents,int timeout );
int 	co_epoll_ctl( int epfd,int op,int fd,struct epoll_event * );
int 	co_epoll_create( int size );
struct 	co_epoll_res *co_epoll_res_alloc( int n );
void 	co_epoll_res_free( struct co_epoll_res * );

#endif
#endif


