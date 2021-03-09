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


#include "coctx.h"
#include <string.h>


#define ESP 0
#define EIP 1
#define EAX 2
#define ECX 3
// -----------
#define RSP 0
#define RIP 1
#define RBX 2
#define RDI 3
#define RSI 4

#define RBP 5
#define R12 6
#define R13 7
#define R14 8
#define R15 9
#define RDX 10
#define RCX 11
#define R8 12
#define R9 13


//----- --------
// 32 bit
// | regs[0]: ret |
// | regs[1]: ebx |
// | regs[2]: ecx |
// | regs[3]: edx |
// | regs[4]: edi |
// | regs[5]: esi |
// | regs[6]: ebp |
// | regs[7]: eax |  = esp
enum
{
	kEIP = 0,
	kESP = 7,
};

//-------------
// 64 bit
//low | regs[0]: r15 |
//    | regs[1]: r14 |
//    | regs[2]: r13 |
//    | regs[3]: r12 |
//    | regs[4]: r9  |
//    | regs[5]: r8  | 
//    | regs[6]: rbp |
//    | regs[7]: rdi |
//    | regs[8]: rsi |
//    | regs[9]: ret |  //ret func addr
//    | regs[10]: rdx |
//    | regs[11]: rcx | 
//    | regs[12]: rbx |
//hig | regs[13]: rsp |
enum
{
	kRDI = 7,
	kRSI = 8,
	kRETAddr = 9,
	kRSP = 13,
};

//64 bit
extern "C"
{
	extern void coctx_swap( coctx_t *,coctx_t* ) asm("coctx_swap");
};

#if defined(__i386__)//i386是32位微处理器的统称
int coctx_init( coctx_t *ctx )   // 将整个结构体初始化0
{
	memset( ctx,0,sizeof(*ctx));
	return 0;
}

/*
typedef void* (*coctx_pfn_t)( void* s, void* s2 );
功能：
先给coctx_pfn_t函数预留2个参数的大小，并4位地址对齐，将参数填入到预存的参数中
regs[kEIP]中保存了pfn的地址，regs[kESP]中则保存了栈顶指针 - 4个字节的大小的地址。
这预留的4个字节用于保存return address。
*/
int coctx_make( coctx_t *ctx,coctx_pfn_t pfn,const void *s,const void *s1 )
{
	//make room for coctx_param
	// 此时sp其实就是esp指向的地方 其中ss_size感觉像是这个栈上目前剩余的空间
	//ctx->ss_sp 对应的空间是在堆上分配的，地址是从低到高的增长，而堆栈是往低地址方向增长的，
	//所以要使用这一块人为改变的栈帧区域，首先地址要调到最高位，即ss_sp + ss_size的位置
	char *sp = ctx->ss_sp + ctx->ss_size - sizeof(coctx_param_t);
	sp = (char*)((unsigned long)sp & -16L);// 字节对齐，16L是一个magic number，GCC默认的堆对齐设置的就是16字节

	// param用来给我们预留下来的参数区设置值
	coctx_param_t* param = (coctx_param_t*)sp ;
	param->s1 = s;//即将切换到的协程
	param->s2 = s1;//切换出的协程

	memset(ctx->regs, 0, sizeof(ctx->regs));

	ctx->regs[ kESP ] = (char*)(sp) - sizeof(void*);//用于保存返回地址
	ctx->regs[ kEIP ] = (char*)pfn;

	return 0;
}
#elif defined(__x86_64__)//64位机

//协程上下文的初始化
int coctx_make( coctx_t *ctx,coctx_pfn_t pfn,const void *s,const void *s1 )
{
	char *sp = ctx->ss_sp + ctx->ss_size;
	sp = (char*) ((unsigned long)sp & -16LL  );

	memset(ctx->regs, 0, sizeof(ctx->regs));

	ctx->regs[ kRSP ] = sp - 8;

	ctx->regs[ kRETAddr] = (char*)pfn;

	ctx->regs[ kRDI ] = (char*)s;
	ctx->regs[ kRSI ] = (char*)s1;
	return 0;
}

int coctx_init( coctx_t *ctx )
{
	memset( ctx,0,sizeof(*ctx));
	return 0;
}

#endif

