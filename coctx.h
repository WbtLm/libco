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
#ifndef __CO_CTX_H__
#define __CO_CTX_H__
#include <stdlib.h>
//函数指针类型coctx_pfn_t
typedef void* (*coctx_pfn_t)( void* s, void* s2 );

// 用于分配coctx_swap两个参数内存区域的结构体，仅32位下使用，64位下两个参数直接由寄存器传递
struct coctx_param_t
{
	const void *s1;
	const void *s2;
};

//用于保存协程执行上下文的 coctx_t 结构
//其中regs就是保存寄存器的值。在32位机器下保存八个寄存器，在64位下保存14个寄存器。
/*
我们知道X86架构下有8个通用寄存器，X64则有16个寄存器，那么为什么64位只使用保存14个寄存器呢？
我们可以在coctx_swap.S中看到64位下缺少了对%r10, %r11寄存器的备份，
x86-64的16个64位寄存器分别是：%rax, %rbx, %rcx, %rdx, %esi, %edi, %rbp, %rsp, %r8-%r15。其中：

%rax 作为函数返回值使用
%rsp栈指针寄存器，指向栈顶
%rdi，%rsi，%rdx，%rcx，%r8，%r9 用作函数参数，依次对应第1参数，第2参数
%rbx，%rbp，%r12，%r13，%14，%15 用作数据存储，遵循被调用者保护规则，简单说就是随便用，调用子函数之前要备份它，以防被修改
%r10，%r11 用作数据存储，遵循调用者保护规则，简单说就是使用之前要先保存原值

调用者保护： 表示这些寄存器上存储的值，需要调用者(父函数)自己想办法先备份好，否则过会子函数直接使用这些寄存器将无情的覆盖。
			如何备份？当然是实现压栈(pushl),等子函数调用完成，再通过栈恢复(popl)
被调用者保护：即表示需要由被调用者(子函数)想办法帮调用者(父函数)进行备份
*/
struct coctx_t//存储协程的上下文
{
#if defined(__i386__)
	void *regs[ 8 ];
#else
	void *regs[ 14 ];
#endif
	// 保存上下文
	size_t ss_size;// 栈的大小
	char *ss_sp;// 栈顶指针esp
	
};

int coctx_init( coctx_t *ctx );
int coctx_make( coctx_t *ctx,coctx_pfn_t pfn,const void *s,const void *s1 );
#endif
