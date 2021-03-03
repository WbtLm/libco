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

#include "co_routine.h"
#include "co_routine_inner.h"
#include "co_epoll.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <string>
#include <map>

#include <poll.h>
#include <sys/time.h>
#include <errno.h>

#include <assert.h>

#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <limits.h>

extern "C"
{
	extern void coctx_swap( coctx_t *,coctx_t* ) asm("coctx_swap");
};
using namespace std;
stCoRoutine_t *GetCurrCo( stCoRoutineEnv_t *env );
struct stCoEpoll_t;

// 该线程中第一个协程创建的时候进行初始化,每个线程中都只有一个stCoRoutineEnv_t实例
//线程可以通过该stCoRoutineEnv_t实例了解现在有哪些协程，哪个协程正在运行，以及下一个运行的协程是哪个。
struct stCoRoutineEnv_t
{
	/*
	pCallStack是用来保留程序运行过程中局部变量以及函数调用关系的
	每当启动（resume）一个协程时，就将它的协程控制块 stCoRoutine_t 结构指针保存在 pCallStack 的“栈顶”，
	然后“栈指针”iCallStackSize 加 1，最后切换 context 到待启动协程运行。当协程要让出（yield）CPU 时，
	就将它的 stCoRoutine_t 从 pCallStack 弹出，“栈指针”iCallStackSize 减 1，
	然后切换 context 到当前栈顶的协程（原来被挂起的调用者）恢复执行。
	libco只能支持128层协程的嵌套调用
	*/
	/*
	如果将协程看成一种特殊的函数，那么这个 pCallStack 就时保存这些函数的调用链的栈。
	非对称协程最大特点就是协程间存在明确的调用关系；甚至在有些文献中，启动协程被称作 call，
	挂起协程叫 return。非对称协程机制下的被调协程只能返回到调用者协程，这种调用关系不能乱，
	因此必须将调用链保存下来
	*/
	stCoRoutine_t *pCallStack[ 128 ];// 保存当前栈中的协程
	int iCallStackSize;// 表示当前在运行的协程的下一个位置，即cur_co_runtine_index + 1
	stCoEpoll_t *pEpoll;// epoll的一个封装结构,用于协程时间片切换

	//for copy stack log lastco and nextco
	// 仅在协程共享栈模式的协程切换时候使用
	stCoRoutine_t* pending_co;// 当前协程对象指针
	stCoRoutine_t* occupy_co;// 协程切换目标协程对象指针
};
//int socket(int domain, int type, int protocol);
void co_log_err( const char *fmt,... )
{
}


#if defined( __LIBCO_RDTSCP__) 
static unsigned long long counter(void)
{
	register uint32_t lo, hi;
	register unsigned long long o;
	__asm__ __volatile__ (
			"rdtscp" : "=a"(lo), "=d"(hi)::"%rcx"
			);
	o = hi;
	o <<= 32;
	return (o | lo);

}
static unsigned long long getCpuKhz()
{
	FILE *fp = fopen("/proc/cpuinfo","r");
	if(!fp) return 1;
	char buf[4096] = {0};
	fread(buf,1,sizeof(buf),fp);
	fclose(fp);

	char *lp = strstr(buf,"cpu MHz");
	if(!lp) return 1;
	lp += strlen("cpu MHz");
	while(*lp == ' ' || *lp == '\t' || *lp == ':')
	{
		++lp;
	}

	double mhz = atof(lp);
	unsigned long long u = (unsigned long long)(mhz * 1000);
	return u;
}
#endif

static unsigned long long GetTickMS()
{
#if defined( __LIBCO_RDTSCP__) 
	static uint32_t khz = getCpuKhz();
	return counter() / khz;
#else
	struct timeval now = { 0 };
	gettimeofday( &now,NULL );
	unsigned long long u = now.tv_sec;
	u *= 1000;
	u += now.tv_usec / 1000;
	return u;
#endif
}

static pid_t GetPid()
{
    static __thread pid_t pid = 0;
    static __thread pid_t tid = 0;
    if( !pid || !tid || pid != getpid() )
    {
        pid = getpid();
#if defined( __APPLE__ )
		tid = syscall( SYS_gettid );
		if( -1 == (long)tid )
		{
			tid = pid;
		}
#elif defined( __FreeBSD__ )
		syscall(SYS_thr_self, &tid);
		if( tid < 0 )
		{
			tid = pid;
		}
#else 
        tid = syscall( __NR_gettid );
#endif

    }
    return tid;

}
/*
static pid_t GetPid()
{
	char **p = (char**)pthread_self();
	return p ? *(pid_t*)(p + 18) : getpid();
}
*/
template <class T,class TLink>
void RemoveFromLink(T *ap)
{
	TLink *lst = ap->pLink;
	if(!lst) return ;
	assert( lst->head && lst->tail );

	if( ap == lst->head )
	{
		lst->head = ap->pNext;
		if(lst->head)
		{
			lst->head->pPrev = NULL;
		}
	}
	else
	{
		if(ap->pPrev)
		{
			ap->pPrev->pNext = ap->pNext;
		}
	}

	if( ap == lst->tail )
	{
		lst->tail = ap->pPrev;
		if(lst->tail)
		{
			lst->tail->pNext = NULL;
		}
	}
	else
	{
		ap->pNext->pPrev = ap->pPrev;
	}

	ap->pPrev = ap->pNext = NULL;
	ap->pLink = NULL;
}

template <class TNode,class TLink>
void inline AddTail(TLink*apLink,TNode *ap)
{
	if( ap->pLink )
	{
		return ;
	}
	if(apLink->tail)
	{
		apLink->tail->pNext = (TNode*)ap;
		ap->pNext = NULL;
		ap->pPrev = apLink->tail;
		apLink->tail = ap;
	}
	else
	{
		apLink->head = apLink->tail = ap;
		ap->pNext = ap->pPrev = NULL;
	}
	ap->pLink = apLink;
}
template <class TNode,class TLink>
void inline PopHead( TLink*apLink )
{
	if( !apLink->head ) 
	{
		return ;
	}
	TNode *lp = apLink->head;
	if( apLink->head == apLink->tail )
	{
		apLink->head = apLink->tail = NULL;
	}
	else
	{
		apLink->head = apLink->head->pNext;
	}

	lp->pPrev = lp->pNext = NULL;
	lp->pLink = NULL;

	if( apLink->head )
	{
		apLink->head->pPrev = NULL;
	}
}

template <class TNode,class TLink>
void inline Join( TLink*apLink,TLink *apOther )
{
	//printf("apOther %p\n",apOther);
	if( !apOther->head )
	{
		return ;
	}
	TNode *lp = apOther->head;
	while( lp )
	{
		lp->pLink = apLink;
		lp = lp->pNext;
	}
	lp = apOther->head;
	if(apLink->tail)
	{
		apLink->tail->pNext = (TNode*)lp;
		lp->pPrev = apLink->tail;
		apLink->tail = apOther->tail;
	}
	else
	{
		apLink->head = apOther->head;
		apLink->tail = apOther->tail;
	}

	apOther->head = apOther->tail = NULL;
}

/////////////////for copy stack //////////////////////////
stStackMem_t* co_alloc_stackmem(unsigned int stack_size)
{
	stStackMem_t* stack_mem = (stStackMem_t*)malloc(sizeof(stStackMem_t));
	stack_mem->occupy_co= NULL;
	stack_mem->stack_size = stack_size;
	stack_mem->stack_buffer = (char*)malloc(stack_size);
	stack_mem->stack_bp = stack_mem->stack_buffer + stack_size;
	return stack_mem;
}

stShareStack_t* co_alloc_sharestack(int count, int stack_size)
{
	stShareStack_t* share_stack = (stShareStack_t*)malloc(sizeof(stShareStack_t));
	share_stack->alloc_idx = 0;
	share_stack->stack_size = stack_size;

	//alloc stack array
	share_stack->count = count;
	stStackMem_t** stack_array = (stStackMem_t**)calloc(count, sizeof(stStackMem_t*));
	for (int i = 0; i < count; i++)
	{
		stack_array[i] = co_alloc_stackmem(stack_size);
	}
	share_stack->stack_array = stack_array;
	return share_stack;
}
//在共享栈结构中分配栈——轮询
static stStackMem_t* co_get_stackmem(stShareStack_t* share_stack)
{
	if (!share_stack)
	{
		return NULL;
	}
	int idx = share_stack->alloc_idx % share_stack->count;
	share_stack->alloc_idx++;

	return share_stack->stack_array[idx];
}


// ----------------------------------------------------------------------------
struct stTimeoutItemLink_t;
struct stTimeoutItem_t;

//该结构体重维护了事件循环需要的数据,stCoEpoll_t中主要保存了epoll监听的fd，以及注册在其中的超时事件。
struct stCoEpoll_t
{
	int iEpollFd;//epoll或者kqueue的fd
	static const int _EPOLL_SIZE = 1024 * 10;//作为 epoll_wait() 系统调用的第三个参数，即⼀次 epoll_wait 最多返回的就绪事件个数。

	struct stTimeout_t *pTimeout;//类型为 stTimeout_t 的结构体指针。该结构实际上是⼀个时间轮（Timingwheel）定时器,记录了所有的定时事件

	struct stTimeoutItemLink_t *pstTimeoutList;//指向 stTimeoutItemLink_t 类型的结构体指针。该指针实际上是⼀个链表头。链表用于临时存放超时事件的 item。本轮超时的事件

	struct stTimeoutItemLink_t *pstActiveList;//指向 stTimeoutItemLink_t 类型的结构体指针。也是指向⼀个链表。该链表用于存放 epoll_wait 得到的就绪事件和定时器超时事件。本轮触发的事件

	co_epoll_res *result; //对 epoll_wait()第⼆个参数的封装，即⼀次 epoll_wait 得到的结果集

};
typedef void (*OnPreparePfn_t)( stTimeoutItem_t *,struct epoll_event &ev, stTimeoutItemLink_t *active );
typedef void (*OnProcessPfn_t)( stTimeoutItem_t *);
//
struct stTimeoutItem_t
{

	enum
	{
		eMaxTimeout = 40 * 1000 //40s
	};
	stTimeoutItem_t *pPrev;
	stTimeoutItem_t *pNext;
	stTimeoutItemLink_t *pLink;

	unsigned long long ullExpireTime;

	OnPreparePfn_t pfnPrepare;
	OnProcessPfn_t pfnProcess;

	void *pArg; // routine 
	bool bTimeout;
};
struct stTimeoutItemLink_t
{
	stTimeoutItem_t *head;
	stTimeoutItem_t *tail;

};

//时间轮的数据结构
/*
* 毫秒级的超时管理器
* 使用时间轮实现
* 但是是有限制的，最长超时时间不可以超过iItemSize毫秒
*/
struct stTimeout_t
{
	/*
	   时间轮
	   超时事件数组，总长度为iItemSize,每一项代表1毫秒，为一个链表，代表这个时间所超时的事件。
	   这个数组在使用的过程中，会使用取模的方式，把它当做一个循环数组来使用，虽然并不是用循环链表来实现的
	*/
	stTimeoutItemLink_t *pItems;
	int iItemSize;//数组长度

	unsigned long long ullStart;// 时间轮第一次使用的时间
	long long llStartIdx;// 目前正在使用的下标
};
/*	申请了60*1000个timeoutLink链表
	设置当前时间为起始时间
	设置当前游标为0*/
stTimeout_t *AllocTimeout( int iSize )
{
	stTimeout_t *lp = (stTimeout_t*)calloc( 1,sizeof(stTimeout_t) );	

	lp->iItemSize = iSize;
	lp->pItems = (stTimeoutItemLink_t*)calloc( 1,sizeof(stTimeoutItemLink_t) * lp->iItemSize );

	lp->ullStart = GetTickMS();
	lp->llStartIdx = 0;

	return lp;
}
void FreeTimeout( stTimeout_t *apTimeout )
{
	free( apTimeout->pItems );
	free ( apTimeout );
}
/*
* 将事件添加到定时器中
* @param apTimeout - (ref) 超时管理器
* @param apItem    - (in) 即将插入的超时事件
* @param allNow    - (in) 当前时间
*/
int AddTimeout( stTimeout_t *apTimeout,stTimeoutItem_t *apItem ,unsigned long long allNow )
{
	if( apTimeout->ullStart == 0 )// 当前时间管理器的最早超时时间
	{
		apTimeout->ullStart = allNow;// 设置时间轮的最早时间是当前时间
		apTimeout->llStartIdx = 0;// 设置最早时间对应的index 为 0
	}
	if( allNow < apTimeout->ullStart )// 插入时间小于初始时间肯定是错的
	{
		co_log_err("CO_ERR: AddTimeout line %d allNow %llu apTimeout->ullStart %llu",
					__LINE__,allNow,apTimeout->ullStart);

		return __LINE__;
	}
	if( apItem->ullExpireTime < allNow )// 预期时间小于插入时间也是有问题的
	{
		co_log_err("CO_ERR: AddTimeout line %d apItem->ullExpireTime %llu allNow %llu apTimeout->ullStart %llu",
					__LINE__,apItem->ullExpireTime,allNow,apTimeout->ullStart);

		return __LINE__;
	}
	unsigned long long diff = apItem->ullExpireTime - apTimeout->ullStart;// 计算事件还有多长时间会超时
	// 预期时间到现在不能超过时间轮的大小
	// 其实是可以的，只需要取余放进去并加上一个圈数的成员就可以了
	// 遍历时圈数不为零就说明实际超时时间还有一个时间轮的长度，
	// 遍历完一项以后圈数不为零就减1即可
	if( diff >= (unsigned long long)apTimeout->iItemSize )
	{
		diff = apTimeout->iItemSize - 1;
		co_log_err("CO_ERR: AddTimeout line %d diff %d",
					__LINE__,diff);

		//return __LINE__;
	}
	// 时间轮粒度为1毫秒，即一项代表一毫秒,AddTail实际执行了把stPoll_t插入到时间轮中
	AddTail( apTimeout->pItems + ( apTimeout->llStartIdx + diff ) % apTimeout->iItemSize , apItem );

	return 0;
}

//从时间轮中取出根据目前时间来说已经超时的事件，并插入到timeout链表中
inline void TakeAllTimeout( stTimeout_t *apTimeout,unsigned long long allNow,stTimeoutItemLink_t *apResult )
{	
	if( apTimeout->ullStart == 0 )// 第一次调用是设置初始时间
	{
		apTimeout->ullStart = allNow;
		apTimeout->llStartIdx = 0;
	}

	if( allNow < apTimeout->ullStart )// 当前时间小于初始时间显然是有问题的
	{
		return ;
	}
	int cnt = allNow - apTimeout->ullStart + 1;// 求一个取出事件的有效区间
	if( cnt > apTimeout->iItemSize )
	{
		cnt = apTimeout->iItemSize;
	}
	if( cnt < 0 )
	{
		return;
	}
	for( int i = 0;i<cnt;i++)
	{
		// 把上面求的有效区间过一遍，某一项存在数据的话插入到超时链表中
		int idx = ( apTimeout->llStartIdx + i) % apTimeout->iItemSize;
		Join<stTimeoutItem_t,stTimeoutItemLink_t>( apResult,apTimeout->pItems + idx  );//链表操作
	}
	// 更新时间轮属性
	apTimeout->ullStart = allNow;
	apTimeout->llStartIdx += cnt - 1;
}
static int CoRoutineFunc( stCoRoutine_t *co,void * )
{
	if( co->pfn )
	{
		co->pfn( co->arg );
	}
	co->cEnd = 1;

	stCoRoutineEnv_t *env = co->env;

	co_yield_env( env );

	return 0;
}


/**
 * 功能：
 * (1)设置协程属性值，主要为栈大小;(2)申请内存创建协程对象(CCB)，并设置环境变量和外部传入的协程函数及其参数
 * (3)设定协程的运行栈信息;(4)设定协程的默认标志位信息
 * 参数：
 * env  环境变量;   attr 协程信息
 * pfn  函数指针;   arg  函数参数
*/
struct stCoRoutine_t *co_create_env( stCoRoutineEnv_t * env, const stCoRoutineAttr_t* attr,
		pfn_co_routine_t pfn,void *arg )
{
	// 设置协程属性值，主要为栈大小
	stCoRoutineAttr_t at;
	if( attr )// 如果指定了attr的话就执行拷贝
	{
		memcpy( &at,attr,sizeof(at) );
	}
	// stack_size 有效区间为[0, 1024 * 1024 * 8]
	if( at.stack_size <= 0 )
	{
		at.stack_size = 128 * 1024;
	}
	else if( at.stack_size > 1024 * 1024 * 8 )
	{
		at.stack_size = 1024 * 1024 * 8;
	}
	
	// 4KB对齐,也就是说如果对stacksize取余不为零的时候对齐为4KB
	if( at.stack_size & 0xFFF ) 
	{
		at.stack_size &= ~0xFFF;
		at.stack_size += 0x1000;
	}
	// 申请内存创建协程对象(类似于进程控制块，此处可称之为协程控制块，CCB)
	stCoRoutine_t *lp = (stCoRoutine_t*)malloc( sizeof(stCoRoutine_t) );
		memset( lp,0,(long)(sizeof(stCoRoutine_t))); 

 	// 设定协程环境变量，协程执行函数及其参数
	lp->env = env;
	lp->pfn = pfn;
	lp->arg = arg;

	// 检查协程是否使用共享协程栈
    // 共享协程栈：取共享协程栈
    // 非共享协程栈：为当前协程申请分配内存
	stStackMem_t* stack_mem = NULL;
	if( at.share_stack )// 共享栈模式 栈需要自己指定
	{
		stack_mem = co_get_stackmem( at.share_stack);
		at.stack_size = at.share_stack->stack_size;
	}
	else // 每个协程有一个私有的栈
	{
		stack_mem = co_alloc_stackmem(at.stack_size);
	}
	lp->stack_mem = stack_mem; // 设置协程运行栈空间

	lp->ctx.ss_sp = stack_mem->stack_buffer;// 设置协程运行栈的栈顶指针（相对于bp是低地址）
	lp->ctx.ss_size = at.stack_size;// 未使用大小,与前者相加为esp指针
    
	// 设置协程栈其他标志
    // cStart=0,未启动;
    // cIsMain=0,非主协程
	lp->cStart = 0;
	lp->cEnd = 0;
	lp->cIsMain = 0;
	lp->cEnableSysHook = 0;
	lp->cIsShareStack = at.share_stack != NULL;
	// 共享运行栈时，协程切换需要保存的栈信息
	lp->save_size = 0;
	lp->save_buffer = NULL;

	return lp;
}
/*
功能：创建协程
参数：	ppco: stCoRoutine_t** 类型的指针。输出参数，co_create 内部会为新协程分配⼀个“协程控制块”，是协程的主体结构，存储着一个协程所有的信息,co 将指向这个分配的协程控制块。
		attr: stCoRoutineAttr_t 类型的指针。输⼊参数，用于指定要创建协程的属性，可为 NULL。实际上仅有两个属性：栈⼤小、指向共享栈的指针（使用共享栈模式）。
		void* (routine)(void):void* (*)(void ) 类型的函数指针，指向协程的任务函数，即启动这个协程后要完成什么样的任务。routine 类型为函数指针。
		arg: void 类型指针，传递给任务函数的参数，类似于 pthread 传递给线程的参数。调用 co_create 将协程创建出来后，这时候它还没有启动，也即是说我们传递的routine 函数还没有被调用。
*/
int co_create( stCoRoutine_t **ppco,const stCoRoutineAttr_t *attr,pfn_co_routine_t pfn,void *arg )
{
	// 查看当前线程的协程环境变量是否存在，如果不存在则创建初始化线程协程环境
    // 一个线程共用一个协程环境变量
	if( !co_get_curr_thread_env() ) // 是一个线程私有的变量 
	{
		co_init_curr_thread_env();// 创建初始化线程协程环境，并将主线程作为第一个协程，称之为主协程
	}
	stCoRoutine_t *co = co_create_env( co_get_curr_thread_env(), attr, pfn,arg );// 在当前线程的协程环境变量上创建一个新的协程对象
	*ppco = co;// 返回新创建的协程对象
	return 0;
}
/*
任务结束后要记得调用co_free()或 co_release()销毁这个临时性的协程，否则将引起内存泄漏。
*/
void co_free( stCoRoutine_t *co )
{
    if (!co->cIsShareStack) 
    {    
        free(co->stack_mem->stack_buffer);
        free(co->stack_mem);
    }   
    free( co );
}
void co_release( stCoRoutine_t *co )
{
    co_free( co );
}

void co_swap(stCoRoutine_t* curr, stCoRoutine_t* pending_co);
/*
功能：在调用 co_create 创建协程返回成功后，调用 co_resume 函数启动协程,可以通过 resume 将 CPU 交给任意协程
参数：启动 co 指针指向的协程
*/
void co_resume( stCoRoutine_t *co )
{
	stCoRoutineEnv_t *env = co->env;
	// stCoRoutine_t结构需要我们在我们的代码中自行调用co_release或者co_free
	// 获取当前正在进行的协程主体 
	stCoRoutine_t *lpCurrRoutine = env->pCallStack[ env->iCallStackSize - 1 ];//取当前协程控制块指针
	
	/*if分支：当且仅当协程是第一次启动时才会执行到。
	首次启动协程过程有点特殊，需要调用 coctx_make() 为新协程准备 context（为了让 co_swap() 内能跳转到协程的任务函数）
	并将 cStart 标志变量置 1。*/
	if( !co->cStart )
	{
		//coctx_make所做的事情就是为coctx_swap函数的正确执行去初始化co->ctx中寄存器信息。
		coctx_make( &co->ctx,(coctx_pfn_t)CoRoutineFunc,co,0 );//保存上一个协程的上下文
		co->cStart = 1;//标记了这个协程是否是第一次执行co_resume
	}
	env->pCallStack[ env->iCallStackSize++ ] = co;// 把此次执行的协程控制块放入调用栈中，iCallStackSize自增后是调用栈中目前有的协程数量
	co_swap(lpCurrRoutine, co);					  //调用 co_swap() 切换到 co 指向的新协程上去执行
	/*
	co_swap() 不会就此返回，而是要这次 resume 的 co 协程主动yield 让出 CPU 时才会返回到 co_resume() 中来。
	值得指出的是，这里讲 co_swap() 不会就此返回，不是说这个函数就阻塞在这里等待 co 这个协程 yield 让出 CPU。
	实际上,	后面我们将会看到，co_swap() 内部已经切换了 CPU 执行上下文，奔着 co 协程的代码路径去执行了
	*/
}
/*
功能：协程的挂起, yield 给当前协程的调用者
参数：当前协程的调用者，调用者协程保存在 stCoRoutineEnv_t的 pCallStack 中，因此你只能 yield 给“env”
其实就是在线程独有的调用栈中拿最新的两个协程，正在执行的协程和调用正在执行的协程的协程（有点绕），
然后把它们的上下文切换，即切换协程。
*/
void co_yield_env( stCoRoutineEnv_t *env )
{
	
	stCoRoutine_t *last = env->pCallStack[ env->iCallStackSize - 2 ];// 要切换的协程
	stCoRoutine_t *curr = env->pCallStack[ env->iCallStackSize - 1 ]; // 即当前正在执行的协程

	env->iCallStackSize--;

	co_swap( curr, last);
}

void co_yield_ct()
{

	co_yield_env( co_get_curr_thread_env() );
}
void co_yield( stCoRoutine_t *co )
{
	co_yield_env( co->env );
}

void save_stack_buffer(stCoRoutine_t* occupy_co)//将occupy_co的有效栈内存保存到save_buffer中。
{
	///copy out
	stStackMem_t* stack_mem = occupy_co->stack_mem;
	int len = stack_mem->stack_bp - occupy_co->stack_sp;

	if (occupy_co->save_buffer)
	{
		free(occupy_co->save_buffer), occupy_co->save_buffer = NULL;
	}

	occupy_co->save_buffer = (char*)malloc(len); //malloc buf;
	occupy_co->save_size = len;

	memcpy(occupy_co->save_buffer, occupy_co->stack_sp, len);
}

// 当前准备让出 CPU 的协程叫做 current 协程，把即将调入执行的叫做 pending 协程
void co_swap(stCoRoutine_t* curr, stCoRoutine_t* pending_co)
{
 	stCoRoutineEnv_t* env = co_get_curr_thread_env();//返回地址，即协程恢复时开始执行的位置

	//get curr stack sp，获取esp指针
	char c;
	curr->stack_sp= &c;	//取当前栈顶

	if (!pending_co->cIsShareStack) //若没有使用share stack，就不存在抢地盘的问题。
	{	//见官方注释：for copy stack log lastco and nextco
		env->pending_co = NULL;	 //可能被人家抢了地盘（栈）的协程，没有occupy_co自然就没有被抢的协程
		env->occupy_co = NULL;   //当前占用栈的协程
	}
	else //occupy_co是原来share stack的所有者，pending要抢占share stack  (多个协程争抢同一块share stack)
	{
		env->pending_co = pending_co;//设置好pending_co，下文中会将env->pending_co的栈还原
		//get last occupy co on the same stack mem
		// 获取pending使用的栈空间的执行协程
		stCoRoutine_t* occupy_co = pending_co->stack_mem->occupy_co;//公共栈空间原来的所有者
		//（occupy_co初始化是NULL），第一次执行协程后这里就会把它变为自己。初始化后第一次拿到执行权栈不为空之后，它不再会是NULL。
		//set pending co to occupy thest stack mem;
		pending_co->stack_mem->occupy_co = pending_co; //不管之前是谁占用了这个地盘，pending都会抢占share stack

		env->occupy_co = occupy_co;	//记录下之前是谁在使用share stack
		
		//如果pending要抢占share stack 那原来的所有者
		if (occupy_co && occupy_co != pending_co)//有occupy_co并且不是自己
		{
			save_stack_buffer(occupy_co);//// 如果上一个使用协程不为空,则需要把它的栈内容保存起来，将occupy_co的栈中有效数据保存到occupy->save_buffer中
			//pending_co把occupy_co撵走之后就可以还原自己的栈空间了。
		}
	}
	
	//swap context从本协程切出，CPU就跑去执行pending中的代码了
	coctx_swap(&(curr->ctx),&(pending_co->ctx));
	
	//在协程被其他地方执行co_resume了以后才会继续执行这里（coctx_swap之后的代码）
	//stack buffer may be overwrite, so get again;		//why?难道可以多线程之间串？
	stCoRoutineEnv_t* curr_env = co_get_curr_thread_env();
	stCoRoutine_t* update_occupy_co =  curr_env->occupy_co;
	stCoRoutine_t* update_pending_co = curr_env->pending_co;
	
	if (update_occupy_co && update_pending_co && update_occupy_co != update_pending_co)
	{
		//resume stack buffer
		if (update_pending_co->save_buffer && update_pending_co->save_size > 0)
		{
			// 如果是一个协程执行到一半然后被切换出去然后又切换回来,这个时候需要恢复栈空间
			memcpy(update_pending_co->stack_sp, update_pending_co->save_buffer, update_pending_co->save_size);
		}//pending抢占share stack
	}
}



//int poll(struct pollfd fds[], nfds_t nfds, int timeout);
// { fd,events,revents }
struct stPollItem_t ;
struct stPoll_t : public stTimeoutItem_t 
{
	struct pollfd *fds;// 描述poll中的事件
	nfds_t nfds; // typedef unsigned long int nfds_t;

	stPollItem_t *pPollItems; // 要加入epoll的事件 长度为nfds

	int iAllEventDetach;// 标识是否已经处理过了这个对象了

	int iEpollFd;// epoll fd

	int iRaiseCnt;// 此次触发的事件数


};
struct stPollItem_t : public stTimeoutItem_t
{
	struct pollfd *pSelf;// 对应的poll结构
	stPoll_t *pPoll;// 所属的stPoll_t

	struct epoll_event stEvent;// poll结构所转换的epoll结构
};
/*
 *   EPOLLPRI 		POLLPRI    // There is urgent data to read.  
 *   EPOLLMSG 		POLLMSG
 *
 *   				POLLREMOVE
 *   				POLLRDHUP
 *   				POLLNVAL
 *
 * */
static uint32_t PollEvent2Epoll( short events )
{
	uint32_t e = 0;	
	if( events & POLLIN ) 	e |= EPOLLIN;
	if( events & POLLOUT )  e |= EPOLLOUT;
	if( events & POLLHUP ) 	e |= EPOLLHUP;
	if( events & POLLERR )	e |= EPOLLERR;
	if( events & POLLRDNORM ) e |= EPOLLRDNORM;
	if( events & POLLWRNORM ) e |= EPOLLWRNORM;
	return e;
}
static short EpollEvent2Poll( uint32_t events )
{
	short e = 0;	
	if( events & EPOLLIN ) 	e |= POLLIN;
	if( events & EPOLLOUT ) e |= POLLOUT;
	if( events & EPOLLHUP ) e |= POLLHUP;
	if( events & EPOLLERR ) e |= POLLERR;
	if( events & EPOLLRDNORM ) e |= POLLRDNORM;
	if( events & EPOLLWRNORM ) e |= POLLWRNORM;
	return e;
}

static stCoRoutineEnv_t* g_arrCoEnvPerThread[ 204800 ] = { 0 };
void co_init_curr_thread_env()
{
	pid_t pid = GetPid();	//这里的GetPid()其实是getThreadId()
	g_arrCoEnvPerThread[ pid ] = (stCoRoutineEnv_t*)calloc( 1,sizeof(stCoRoutineEnv_t) );// 创建协程环境变量并赋值给全局变量
	stCoRoutineEnv_t *env = g_arrCoEnvPerThread[ pid ];

	env->iCallStackSize = 0;// 修改"调用栈"顶指针
	struct stCoRoutine_t *self = co_create_env( env, NULL, NULL,NULL );//co_create_env真正实现第一个协程的创建
	self->cIsMain = 1;// 一个线程调用这个函数的肯定是主协程喽

	env->pending_co = NULL;
	env->occupy_co = NULL;

	coctx_init( &self->ctx );// 初始化协程上下文，实际执行的为对象memset为0

	env->pCallStack[ env->iCallStackSize++ ] = self;//// 设置主协程为协程栈的第一个协程

	// 初始化并设置epoll对象
	stCoEpoll_t *ev = AllocEpoll();
	SetEpoll( env,ev );
}
stCoRoutineEnv_t *co_get_curr_thread_env()//这个函数其实也就是在每个线程的第一个协程被创建的时候去初始化
{
	return g_arrCoEnvPerThread[ GetPid() ];//返回一个线程私有的变量
}
/*
回调OnPollProcessEvent:
它的作用是epoll中检测到事件超时或者事件就绪的时候执行的一个回调的，其实也就是执行了co_resume
从这里我们可以看到执行协程切换我们实际上只需要一个co结构而已。
*/
void OnPollProcessEvent( stTimeoutItem_t * ap )
{
	stCoRoutine_t *co = (stCoRoutine_t*)ap->pArg;
	co_resume( co );
}

//OnPollPreparePfn:预处理回调,把epoll事件对应的stPoll_t结构中的值执行一些修改，并把此项插入到active链表中。
void OnPollPreparePfn( stTimeoutItem_t * ap,struct epoll_event &e,stTimeoutItemLink_t *active )
{
	stPollItem_t *lp = (stPollItem_t *)ap;
	// 把epoll此次触发的事件转换成poll中的事件
	lp->pSelf->revents = EpollEvent2Poll( e.events );
	//pSelf是struct pollfd，revents是实际发生的事件（在poll调用里是由内核填充），

	stPoll_t *pPoll = lp->pPoll;
	pPoll->iRaiseCnt++;// 已经触发的事件数加一

	if( !pPoll->iAllEventDetach )// 若此事件还未被触发过
	{
		pPoll->iAllEventDetach = 1;//不可重入

		RemoveFromLink<stTimeoutItem_t,stTimeoutItemLink_t>( pPoll );//将该事件从时间轮中移除,因为事件已经触发了，肯定不能再超时了

		AddTail( active,pPoll );//将该事件添加到active列表中

	}
}

/*
功能：主协程事件循环,主协程执行的函数,不停的轮询等待其他协程注册的事件成立
首先分配co_epoll_res结构用于epoll_wait。
根据epoll_wait返回的事件，获取对应stTimeoutItem_t对象
若定义了pfnPrepare函数，调用，否则，将item加入active链表之中。
接下来，获取当前时间，转动时间轮盘，获取所有超时的item，将他们加入timeout链表
设置超时flag为true。将timeout链表中所有元素加入active链表之中。
遍历active链表，没有超时的加入时间轮，调用每个结点的pfnProcess函数，唤醒对应协程。

*/
/*
* libco的核心调度
* 在此处调度三种事件：
* 1. 被hook的io事件，该io事件是通过co_poll_inner注册进来的
* 2. 超时事件
* 3. 用户主动使用poll的事件
* 所以，如果用户用到了三种事件，必须得配合使用co_eventloop
*
* @param ctx epoll管理器
* @param pfn 每轮事件循环的最后会调用该函数
* @param arg pfn的参数

具体步骤如下：
调用epoll_wait等待监听的事件
将stTimeout_t中的timeout链表清空
若epoll中有数据，则将对应的事件加入到stTimeout_t的active链表中；同时将timeout数组链表中删除本事件的超时事件
遍历timout数组链表，将已经超时的事件加入到timeout链表中
将timeout链表中的所有事件置为超时事件，需要后续特殊处理；同时将timeout链表合并到active链表
遍历active链表，对超时事件且当前时间未超过超时时间的，重新将其加入到timeout数组链表中，这就解决了上面超时时间超过60s的问题；对其他的事件进行处理
*/
void co_eventloop( stCoEpoll_t *ctx,pfn_co_eventloop_t pfn,void *arg )
{
	if( !ctx->result )	// 给结果集分配空间
	{
		ctx->result =  co_epoll_res_alloc( stCoEpoll_t::_EPOLL_SIZE );// epoll结果集大小
	}
	co_epoll_res *result = ctx->result;


	for(;;)
	{
		// 最大超时时间设置为 1 ms,所以最长1ms，epoll_wait就会被唤醒
		int ret = co_epoll_wait( ctx->iEpollFd,result,stCoEpoll_t::_EPOLL_SIZE, 1 );//调用 epoll_wait() 等待 I/O 就绪事件，为了配合时间轮⼯作，这里的 timeout设置为 1 毫秒。

		// 不使用局部变量的原因是epoll循环并不是元素的唯一来源.例如条件变量相关(co_routine.cpp stCoCondItem_t)
		stTimeoutItemLink_t *active = (ctx->pstActiveList);//active 指针指向当前执⾏环境的 pstActiveList 队列，注意这里面可能已经有“活跃”的待处理事件
		stTimeoutItemLink_t *timeout = (ctx->pstTimeoutList);//timeout 指针指向 pstTimeoutList 列表，其实这个 timeout 全是个临时性的链表

		memset( timeout,0,sizeof(stTimeoutItemLink_t) );
		
		//处理就绪的⽂件描述符，处理poll事件
		for(int i=0;i<ret;i++)
		{
			stTimeoutItem_t *item = (stTimeoutItem_t*)result->events[i].data.ptr;// 获取在co_poll_inner放入epoll_event中的stTimeoutItem_t结构体
			if( item->pfnPrepare ) // 如果用户设置预处理回调
			{
				item->pfnPrepare( item,result->events[i],active );//调用pfnPrepare 做预处理,实际上，pfnPrepare() 预处理函数内部也会将就绪 item 加⼊ active 队列
			}
			else
			{
				AddTail( active,item );//否则直接将就绪事件 item 加⼊ active 队列
			}
		}

		//从时间轮上取出已超时的事件
		unsigned long long now = GetTickMS();
		// 以当前时间为超时截止点, 从时间轮中取出超时的时间放入到timeout中
		TakeAllTimeout( ctx->pTimeout,now,timeout );
		//遍历 active 队列，调用⼯作协程设置的 pfnProcess() 回调函数 resume挂起的⼯作协程，处理对应的 I/O 或超时事件。这就是主协程的事件循环工作过程
		stTimeoutItem_t *lp = timeout->head;
		while( lp ) // 遍历超时链表,设置超时标志,并加入active链表
		{
			//printf("raise timeout %p\n",lp);
			lp->bTimeout = true;
			lp = lp->pNext;
		}

		Join<stTimeoutItem_t,stTimeoutItemLink_t>( active,timeout );// 把timeout合并到active中

		// 开始遍历active链表
		lp = active->head;
		while( lp )
		{
			// 在链表不为空的时候删除active的第一个元素 如果删除成功,那个元素就是lp
			PopHead<stTimeoutItem_t,stTimeoutItemLink_t>( active );
            if (lp->bTimeout && now < lp->ullExpireTime) 
			{// 一种排错机制,在超时和所等待的时间内已经完成只有一个条件满足才是正确的
				int ret = AddTimeout(ctx->pTimeout, lp, now);
				if (!ret) //插入成功
				{
					lp->bTimeout = false;
					lp = active->head;
					continue;
				}
			}
			if( lp->pfnProcess )
			{// 默认为OnPollProcessEvent 会切换协程
				lp->pfnProcess( lp );
			}

			lp = active->head;
		}
		if( pfn )// 每次事件循环结束以后执行该函数, 用于终止协程
		{
			if( -1 == pfn( arg ) )
			{
				break;
			}
		}

	}
}
void OnCoroutineEvent( stTimeoutItem_t * ap )
{
	stCoRoutine_t *co = (stCoRoutine_t*)ap->pArg;
	co_resume( co );
}


stCoEpoll_t *AllocEpoll()
{
	stCoEpoll_t *ctx = (stCoEpoll_t*)calloc( 1,sizeof(stCoEpoll_t) );

	ctx->iEpollFd = co_epoll_create( stCoEpoll_t::_EPOLL_SIZE );
	ctx->pTimeout = AllocTimeout( 60 * 1000 );
	
	ctx->pstActiveList = (stTimeoutItemLink_t*)calloc( 1,sizeof(stTimeoutItemLink_t) );
	ctx->pstTimeoutList = (stTimeoutItemLink_t*)calloc( 1,sizeof(stTimeoutItemLink_t) );


	return ctx;
}

void FreeEpoll( stCoEpoll_t *ctx )
{
	if( ctx )
	{
		free( ctx->pstActiveList );
		free( ctx->pstTimeoutList );
		FreeTimeout( ctx->pTimeout );
		co_epoll_res_free( ctx->result );
	}
	free( ctx );
}

stCoRoutine_t *GetCurrCo( stCoRoutineEnv_t *env )
{
	return env->pCallStack[ env->iCallStackSize - 1 ];
}
stCoRoutine_t *GetCurrThreadCo( )
{
	stCoRoutineEnv_t *env = co_get_curr_thread_env();
	if( !env ) return 0;
	return GetCurrCo(env);
}



typedef int (*poll_pfn_t)(struct pollfd fds[], nfds_t nfds, int timeout);
/*
co_poll_inner函数主要有三个作用：
1. 将poll的相关事件转换为epoll相关事件，并注册到当前线程的epoll中。
2. 注册超时事件，到当前的epoll中
3. 调用co_yield_ct, 让出该协程。
添加监听事件的步骤：
第一步，先将epoll结构转换成poll结构
第二步，将poll结构加入到epoll的监听事件中
第三步，添加timeout事件
*/
int co_poll_inner( stCoEpoll_t *ctx,struct pollfd fds[], nfds_t nfds, int timeout, poll_pfn_t pollfunc)
{	//poll_pfn_t pollfunc真正的函数
    if (timeout == 0)// 超时时间为零 直接执行系统调用 
	{
		return pollfunc(fds, nfds, timeout);
	}
	if (timeout < 0)
	{
		timeout = INT_MAX;
	}
	int epfd = ctx->iEpollFd;	//epoll池子
	stCoRoutine_t* self = co_self();// 获取当前线程正在运行的协程

	//1.struct change
	stPoll_t& arg = *((stPoll_t*)malloc(sizeof(stPoll_t)));	//stPoll_t结构体
	//以下代码为stPoll_t结构体初始化。这里代码作者使用的是引用&（感觉这个源码风格不统一可能是多人协作）
	memset( &arg,0,sizeof(arg) );

	arg.iEpollFd = epfd;//来自定时器
	arg.fds = (pollfd*)calloc(nfds, sizeof(pollfd));//struct pollfd 申请了nfds个
	arg.nfds = nfds;

	stPollItem_t arr[2]; //临时内存池，不一定会使用到它，（取栈内存比向os申请快得多）。
	if( nfds < sizeof(arr) / sizeof(arr[0]) && !self->cIsShareStack) // 如果poll中监听的描述符只有1个或者0个， 并且目前的不是共享栈模型
	{//第一个条件判断了一下数组开的是不是够大，第二个条件检查share Stack的许可。
		//（这里为什么要检查share shack许可啊？难道是因为arr无法持久？）
		arg.pPollItems = arr;//若够大且允许，就使用数组。
	}	
	else//否则还是老老实实地跟os申请
	{
		arg.pPollItems = (stPollItem_t*)malloc( nfds * sizeof( stPollItem_t ) );
	}//pPollItems最终结果是指向了恰好是nfds*sizeof(stPollItem_t)大小的连续内存
	memset( arg.pPollItems,0,nfds * sizeof(stPollItem_t) );//内存清零  还是初始化

	// 在eventloop中调用的处理函数,功能是唤醒pArg中的协程,也就是这个调用poll的协程
	arg.pfnProcess = OnPollProcessEvent;
	//pfnProcess是定时器成员，OnPollProcessEvent函数指针，此函数作用是将ap中pArg保存的stCoRoutine_t*取出，赋予执行权。（回到本函数）
	arg.pArg = GetCurrCo( co_get_curr_thread_env() );//参数为当前Env指针
	//上面两句使eventloop可以很容易定位并回到co_poll_inner继续执行。
	
	
	//2. add epoll
	for(nfds_t i=0;i<nfds;i++)//epoll的处理
	{
		arg.pPollItems[i].pSelf = arg.fds + i;//取第i个struct pollfd，就是pSelf
		arg.pPollItems[i].pPoll = &arg;

		arg.pPollItems[i].pfnPrepare = OnPollPreparePfn;// 在eventloop中调用的处理函数,功能是唤醒pArg中的协程,也就是这个调用poll的协程
		struct epoll_event &ev = arg.pPollItems[i].stEvent;

		if( fds[i].fd > -1 )
		{
			ev.data.ptr = arg.pPollItems + i;
			ev.events = PollEvent2Epoll( fds[i].events );//co_poll用的事件类型转为epoll用的类型。

			// 把事件加入poll中的事件进行封装以后加入epoll
			int ret = co_epoll_ctl( epfd,EPOLL_CTL_ADD, fds[i].fd, &ev );
			if (ret < 0 && errno == EPERM && nfds == 1 && pollfunc != NULL)
			{// 把事件加入poll中的事件进行封装以后加入epoll
				if( arg.pPollItems != arr )
				{
					free( arg.pPollItems );
					arg.pPollItems = NULL;
				}
				free(arg.fds);
				free(&arg);
				return pollfunc(fds, nfds, timeout);
			}
		}
		//if fail,the timeout would work
	}

	//3.add timeout

	unsigned long long now = GetTickMS();//获取当前时间
	arg.ullExpireTime = now + timeout;//设置超时时间点。
	int ret = AddTimeout( ctx->pTimeout,&arg,now );//往时间轮中加计时器
	int iRaiseCnt = 0;
	if( ret != 0 ) //错误处理一下。
	{
		co_log_err("CO_ERR: AddTimeout ret %d now %lld timeout %d arg.ullExpireTime %lld",
				ret,now,timeout,arg.ullExpireTime);
		errno = EINVAL;
		iRaiseCnt = -1;

	}
    else
	{
		co_yield_env( co_get_curr_thread_env() );
	    //交出执行权，这里会等待eventloop被调用，由eventloop将co_poll事件处理完毕会重新将执行权归还本函数。
		iRaiseCnt = arg.iRaiseCnt;
	}

    {//收尾工作。
		//clear epoll status and memory，将该项从时间轮中删除
		RemoveFromLink<stTimeoutItem_t,stTimeoutItemLink_t>( &arg );
		// 将此次poll中涉及到的时间全部从epoll中删除 
		// 这意味着有一些事件没有发生就被终止了 
		// 比如poll中3个事件,实际触发了两个,最后一个在这里就被移出epoll了
		for(nfds_t i = 0;i < nfds;i++)
		{
			int fd = fds[i].fd;
			if( fd > -1 )
			{
				co_epoll_ctl( epfd,EPOLL_CTL_DEL,fd,&arg.pPollItems[i].stEvent );
			}//del
			fds[i].revents = arg.fds[i].revents;
		}

		//释放指针
		if( arg.pPollItems != arr )
		{
			free( arg.pPollItems );
			arg.pPollItems = NULL;
		}

		free(arg.fds);
		free(&arg);
	}

	return iRaiseCnt;
}

int	co_poll( stCoEpoll_t *ctx,struct pollfd fds[], nfds_t nfds, int timeout_ms )
{
	return co_poll_inner(ctx, fds, nfds, timeout_ms, NULL);
}

void SetEpoll( stCoRoutineEnv_t *env,stCoEpoll_t *ev )
{
	env->pEpoll = ev;
}
stCoEpoll_t *co_get_epoll_ct()
{
	if( !co_get_curr_thread_env() )
	{
		co_init_curr_thread_env();
	}
	return co_get_curr_thread_env()->pEpoll;
}
struct stHookPThreadSpec_t
{
	stCoRoutine_t *co;
	void *value;

	enum 
	{
		size = 1024
	};
};
void *co_getspecific(pthread_key_t key)
{
	stCoRoutine_t *co = GetCurrThreadCo();
	if( !co || co->cIsMain )
	{
		return pthread_getspecific( key );
	}
	return co->aSpec[ key ].value;
}
int co_setspecific(pthread_key_t key, const void *value)
{
	stCoRoutine_t *co = GetCurrThreadCo();
	if( !co || co->cIsMain )
	{
		return pthread_setspecific( key,value );
	}
	co->aSpec[ key ].value = (void*)value;
	return 0;
}



void co_disable_hook_sys()
{
	stCoRoutine_t *co = GetCurrThreadCo();
	if( co )
	{
		co->cEnableSysHook = 0;
	}
}
bool co_is_enable_sys_hook()
{
	// co_enable_sys_hook 会把cEnableSysHook设置成1
	stCoRoutine_t *co = GetCurrThreadCo();
	return ( co && co->cEnableSysHook );
}

stCoRoutine_t *co_self()
{
	return GetCurrThreadCo();
}

//co cond
struct stCoCond_t;
struct stCoCondItem_t 
{
	stCoCondItem_t *pPrev;
	stCoCondItem_t *pNext;
	stCoCond_t *pLink;// 所属链表

	stTimeoutItem_t timeout;//在poll中时一个stTimeoutItem_t代表一个poll事件
};
struct stCoCond_t// 条件变量的实体
{
	stCoCondItem_t *head;
	stCoCondItem_t *tail;
};
static void OnSignalProcessEvent( stTimeoutItem_t * ap )
{
	stCoRoutine_t *co = (stCoRoutine_t*)ap->pArg;
	co_resume( co );
}

stCoCondItem_t *co_cond_pop( stCoCond_t *link );
//查看链表是否有元素，有的话从链表中删除，然后加入到epoll的active链表，在下一次epoll_wait中遍历active时会触发回调，然后CPU执行权切换到执行co_cond_timedwait的地方
int co_cond_signal( stCoCond_t *si )
{
	stCoCondItem_t * sp = co_cond_pop( si );
	if( !sp ) 
	{
		return 0;
	}
	RemoveFromLink<stTimeoutItem_t,stTimeoutItemLink_t>( &sp->timeout );// 从时间轮中移除

	AddTail( co_get_curr_thread_env()->pEpoll->pstActiveList,&sp->timeout );// 加到active队列中
	// 所以单线程运行生产者消费者我们在signal以后还需要调用阻塞类函数转移CPU控制权,例如poll
	return 0;
}
int co_cond_broadcast( stCoCond_t *si )
{
	for(;;)
	{
		stCoCondItem_t * sp = co_cond_pop( si );
		if( !sp ) return 0;

		RemoveFromLink<stTimeoutItem_t,stTimeoutItemLink_t>( &sp->timeout );

		AddTail( co_get_curr_thread_env()->pEpoll->pstActiveList,&sp->timeout );
	}

	return 0;
}

// 条件变量的实体;超时时间
int co_cond_timedwait( stCoCond_t *link,int ms )
{
	stCoCondItem_t* psi = (stCoCondItem_t*)calloc(1, sizeof(stCoCondItem_t));
	psi->timeout.pArg = GetCurrThreadCo();
	// 实际还是执行resume,进行协程切换
	psi->timeout.pfnProcess = OnSignalProcessEvent;

	if( ms > 0 )
	{
		unsigned long long now = GetTickMS();
		// 定义超时时间
		psi->timeout.ullExpireTime = now + ms;

		// 加入时间轮
		int ret = AddTimeout( co_get_curr_thread_env()->pEpoll->pTimeout,&psi->timeout,now );
		if( ret != 0 )
		{
			free(psi);
			return ret;
		}
	}
	// 相当于timeout为负的话超时时间无限，此时条件变量中有一个事件在等待，也就是一个协程待唤醒
	AddTail( link, psi);

	co_yield_ct();// 切换CPU执行权,在epoll中触发peocess回调以后回到这里

	// 这个条件要么被触发,要么已经超时,从条件变量实体中删除
	RemoveFromLink<stCoCondItem_t,stCoCond_t>( psi );
	free(psi);

	return 0;
}
stCoCond_t *co_cond_alloc()
{
	return (stCoCond_t*)calloc( 1,sizeof(stCoCond_t) );
}
int co_cond_free( stCoCond_t * cc )
{
	free( cc );
	return 0;
}


stCoCondItem_t *co_cond_pop( stCoCond_t *link )
{
	stCoCondItem_t *p = link->head;
	if( p )
	{
		PopHead<stCoCondItem_t,stCoCond_t>( link );
	}
	return p;
}
