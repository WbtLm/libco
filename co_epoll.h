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

//finish
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
	EPOLLIN = 0X001,//读事件
	EPOLLPRI = 0X002,//带外数据，与select的异常事件集合对应
	EPOLLOUT = 0X004,//写事件

	EPOLLERR = 0X008,//错误事件
	EPOLLHUP = 0X010,

    EPOLLRDNORM = 0x40,
    EPOLLWRNORM = 0x004,
};
#define EPOLL_CTL_ADD 1//对fd描述符注册event事件
#define EPOLL_CTL_DEL 2//对fd描述符的event事件进行修改
#define EPOLL_CTL_MOD 3//删除已注册的event事件
typedef union epoll_data
{
	void *ptr;
	int fd;
	uint32_t u32;
	uint64_t u64;

} epoll_data_t;

struct epoll_event
{
	uint32_t events;
	epoll_data_t data;
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


