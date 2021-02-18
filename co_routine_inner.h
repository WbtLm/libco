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


#ifndef __CO_ROUTINE_INNER_H__

#include "co_routine.h"
#include "coctx.h"
struct stCoRoutineEnv_t;
struct stCoSpec_t
{
	void *value;
};

struct stStackMem_t
{
	stCoRoutine_t* occupy_co;
	int stack_size;
	char* stack_bp; //stack_buffer + stack_size
	char* stack_buffer;

};

struct stShareStack_t
{
	unsigned int alloc_idx;
	int stack_size;
	int count;
	stStackMem_t** stack_array;
};



struct stCoRoutine_t
{
	/*libco 的协程一旦创建之后便跟创建时的那个线程绑定了的，
	是不支持在不同线程间迁移（migrate）的。
	这个 env，即同属于一个线程所有协程的执行环境，
	包括了当前运行协程、上次切换挂起的协程、嵌套调用的协程栈，和一个 epoll 的封装结构（TBD）*/
	stCoRoutineEnv_t *env;
	pfn_co_routine_t pfn;//实际待执行的协程函数
	void *arg;//实际待执行的协程函数的参数
	coctx_t ctx;//coctx_t类型的结构，用于协程切换时保存的CPU上下文的(context)，包括esp,ebp,eip和其他通用寄存器的值。
	
	//状态和标志变量
	char cStart;
	char cEnd;
	char cIsMain;
	char cEnableSysHook;
	char cIsShareStack;

	void *pvEnv;//保存程序系统环境变量的指针

	//char sRunStack[ 1024 * 128 ];
	stStackMem_t* stack_mem;	//bp保存在这儿//协程运行时的栈内存，固定大小128kb

	/*
	实现 stackful 协程（与之相对的还有一种 stackless 协程）的两种技术：
	Separate coroutine stacks 和 Copying the stack（又叫共享栈）。
	实现细节上，前者为每一个协程分配一个单独的、固定大小的栈；
	而后者则仅为正在运行的协程分配栈内存，当协程被调度切换出去时，就把它实际占用的栈内存 copy 保存到一个单独分配的缓冲区；
	当被切出去的协程再次调度执行时，再一次 copy 将原来保存的栈内存恢复到那个共享的、固定大小的栈内存空间。通常情况下，一个协程实际占用的（从 esp 到栈底）栈空间，相比预分配的这个栈大小（比如 libco的 128KB）会小得多；这样一来，copying stack 的实现方案所占用的内存便会少很多。当然，协程切换时拷贝内存的开销有些场景下也是很大的。因此两种方案各有利弊
	而libco 则同时实现了两种方案，默认使用前者，也允许用户在创建协程时指定使用共享栈。
	*/
	//save satck buffer while confilct on same stack_buffer;
	char* stack_sp; //栈顶指针
	unsigned int save_size;//栈内有效数据大小（len）
	char* save_buffer;//指向用来临时存放栈内有效数据的地方的指针（share stack策略）

	stCoSpec_t aSpec[1024];

};



//1.env
void 				co_init_curr_thread_env();
stCoRoutineEnv_t *	co_get_curr_thread_env();

//2.coroutine
void    co_free( stCoRoutine_t * co );
void    co_yield_env(  stCoRoutineEnv_t *env );

//3.func



//-----------------------------------------------------------------------------------------------

struct stTimeout_t;
struct stTimeoutItem_t ;

stTimeout_t *AllocTimeout( int iSize );
void 	FreeTimeout( stTimeout_t *apTimeout );
int  	AddTimeout( stTimeout_t *apTimeout,stTimeoutItem_t *apItem ,uint64_t allNow );

struct stCoEpoll_t;
stCoEpoll_t * AllocEpoll();
void 		FreeEpoll( stCoEpoll_t *ctx );

stCoRoutine_t *		GetCurrThreadCo();
void 				SetEpoll( stCoRoutineEnv_t *env,stCoEpoll_t *ev );

typedef void (*pfnCoRoutineFunc_t)();

#endif

#define __CO_ROUTINE_INNER_H__
