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

#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <queue>
#include "co_routine.h"
using namespace std;
struct stTask_t
{
	int id;
};
struct stEnv_t
{
	stCoCond_t* cond;
	queue<stTask_t*> task_queue;
};
void* Producer(void* args)
{
	co_enable_hook_sys();
	stEnv_t* env=  (stEnv_t*)args;
	int id = 0;
	while (true)
	{
		stTask_t* task = (stTask_t*)calloc(1, sizeof(stTask_t));
		task->id = id++;
		env->task_queue.push(task);
		printf("%s:%d produce task %d\n", __func__, __LINE__, task->id);
		co_cond_signal(env->cond);
		// poll中数字的调整可以调整交替速度,因为这个数字代表了在epoll中的超时时间,也就是什么时候生产者执行
		// 可以简单的理解为生产者的生产速度,timeout越大,生产速度越慢
		poll(NULL, 0, 1000);
	}
	return NULL;
}
void* Consumer(void* args)
{
	co_enable_hook_sys();
	stEnv_t* env = (stEnv_t*)args;
	while (true)
	{
		if (env->task_queue.empty())
		{
			co_cond_timedwait(env->cond, -1);
			continue;
		}
		// 操作队列的时候没有加锁
		stTask_t* task = env->task_queue.front();
		env->task_queue.pop();
		printf("%s:%d consume task %d\n", __func__, __LINE__, task->id);
		free(task);
	}
	return NULL;
}
/*
 * 主协程是跟 stCoRoutineEnv_t 一起创建的。主协程也无需调用 resume 来启动，
 * 它就是程序本身，就是 main 函数。主协程是一个特殊的存在
 * 
 * 在程序首次调用 co_create() 时，此函数内部会判断当前进程（线程）的 stCoRoutineEnv_t 结构是否已分配，
 * 如果未分配则分配一个，同时分配一个 stCoRoutine_t 结构，并将 pCallStack[0] 指向主协程。
 * 此后如果用 co_resume() 启动协程，又会将 resume 的协程压入 pCallStack 栈
 */
int main()
{
	stEnv_t* env = new stEnv_t;
	env->cond = co_cond_alloc();

	stCoRoutine_t* consumer_routine;//一个协程的结构
	// 协程的创建函数于pthread_create很相似
	//1.指向线程表示符的指针,设置线程的属性(栈大小和指向共享栈的指针,使用共享栈模式),线程运行函数的其实地址,运行时函数的参数
	co_create(&consumer_routine, NULL, Consumer, env);// 创建一个协程
	// 协程在创建以后并没有运行 使用resume运行
	co_resume(consumer_routine);

	stCoRoutine_t* producer_routine;
	co_create(&producer_routine, NULL, Producer, env);
	co_resume(producer_routine);
	
	co_eventloop(co_get_epoll_ct(), NULL, NULL);
	return 0;
}
