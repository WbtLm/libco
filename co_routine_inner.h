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

struct stCoSpec_t//协程私有数据
{
	void *value;
};

struct stStackMem_t//栈
{
	stCoRoutine_t* occupy_co;// 执行时占用的那个协程实体,也就是这个栈现在是哪个协程在用
	int stack_size;//当前栈上未使用的空间
	char* stack_bp; //stack_buffer + stack_size
	char* stack_buffer;//栈的起始地址,当然对于主协程来说这是堆上的空间

};

// 共享栈中多栈可以使得我们在进程切换的时候减少拷贝次数
struct stShareStack_t
{
	unsigned int alloc_idx;// stack_array中我们在下一次调用中应该使用的那个共享栈的index
	int stack_size;// 共享栈的大小，这里的大小指的是一个stStackMem_t*的大小
	int count;// 共享栈的个数，共享栈可以为多个，所以以下为共享栈的数组
	stStackMem_t** stack_array;// 栈的内容，这里是个数组，元素是stStackMem_t*
};


//struct stCoRoutine_t实际上就是就是协程的主体结构，存储着一个协程相关的数据。
// libco的协程一旦创建之后便和创建它的线程绑定在一起 不支持线程之间的迁移
struct stCoRoutine_t
{
	/*
	env:
	同属于一个线程所有协程的执行环境
	包括了当前运行协程、上次切换挂起的协程、嵌套调用的协程栈，和一个 epoll 的封装结构（TBD），
	运行在同一个线程上的各协程是共享该结构的（一个线程共享的结构）
	*/
	stCoRoutineEnv_t *env;//协程运行的上下文
	pfn_co_routine_t pfn;// 结构为一个函数指针，实际待执行的协程函数 
	void *arg;//pfn的参数
	coctx_t ctx;//用于协程切换时保存的CPU上下文的(context)，包括esp,ebp,eip和其他通用寄存器的值。
	
	//一些状态和标志变量
	char cStart;// 协程是否执行过resume
	char cEnd;//// 是否已结束
	char cIsMain; //是否为主协程 在co_init_curr_thread_env修改
	char cEnableSysHook;//此协程是否hook库函数
	char cIsShareStack;// 是否开启共享栈模式

	void *pvEnv;//保存程序系统环境变量的指针，这个环境变量其实是与hook后的setenv，getenv类函数有关

	//char sRunStack[ 1024 * 128 ];
	//Libco_有栈协程
	//协程运行时的栈内存，固定大小128kb
	stStackMem_t* stack_mem;	//bp保存在这儿

	/*
	实现 stackful 协程（与之相对的还有一种 stackless 协程）的两种技术：
	Separate coroutine stacks 和 Copying the stack（又叫共享栈）。
	实现细节上，前者为每一个协程分配一个单独的、固定大小的栈；
	而后者则仅为正在运行的协程分配栈内存，当协程被调度切换出去时，就把它实际占用的栈内存 copy 保存到一个单独分配的缓冲区；
	当被切出去的协程再次调度执行时，再一次 copy 将原来保存的栈内存恢复到那个共享的、固定大小的栈内存空间。通常情况下，一个协程实际占用的（从 esp 到栈底）栈空间，相比预分配的这个栈大小（比如 libco的 128KB）会小得多；这样一来，copying stack 的实现方案所占用的内存便会少很多。当然，协程切换时拷贝内存的开销有些场景下也是很大的。因此两种方案各有利弊
	而libco 则同时实现了两种方案，默认使用前者，也允许用户在创建协程时指定使用共享栈。
	*/

	//save satck buffer while confilct on same stack_buffer;
	// 当使用共享栈的时候需要用到的一些数据结构
	char* stack_sp; //栈顶指针
	unsigned int save_size;//save_buffer中保存的数据大小
	char* save_buffer;//共享栈的时候，此指针指向临时保存栈内有效数据的内存空间

	stCoSpec_t aSpec[1024];//协程私有数据

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
