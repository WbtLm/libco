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

/* 
切换上下文的函数——coctx_swap函数，是 C 风格的函数
函数原型如下：void coctx_swap(coctx_t* curr, coctx_t* pending) asm(“coctx_swap”);
其中，第一个参数 curr 为当前协程的 coctx_t结构指针，其实是个输出参数，函数调用过程中会将当前协程的 context 保存在这个参数指向的内存里
第二个参数 pending，即待切入的协程的 coctx_t 指针，是个输入参数，coctx_swap 从这里取上次保存的 context，恢复各寄存器的值。
coctx_t 结构，就是用于保存各寄存器值（context）的。
该函数调用之前还处于第一个协程的环境，该函数返回后，则当前运行的协程就已经完全是第二个协程了。
这个函数要直接操作寄存器，所以要使用汇编编程。此处用 AT&T 汇编指令，即如果指令有两个操作数，那么第一个是源操作数，第二个即为目的操作数。
*/
.globl coctx_swap
#if !defined( __APPLE__ ) && !defined( __FreeBSD__ )
.type  coctx_swap, @function
#endif
coctx_swap:
/*用 4(%esp) 便可以取到第一个参数（当前协程 context 指针）
8(%esp)可以取到第二个参数（待切入运行协程的 context 指针）
当前栈顶的内容，(%esp) 则保存了 coctx_swap 的返回地址*/
#if defined(__i386__)
	leal 4(%esp), %eax /*LEA 指令即 Load Effective Address 的缩写。这条指令有效地址保存到 eax 寄存器  //sp*/ 
	movl 4(%esp), %esp  /*4(%esp) 内正是指向 current 协程 coctx_t 的指针，这里把它塞到 esp 寄存器*/
	leal 32(%esp), %esp //parm a : &regs[7] + sizeof(void*)  /*将 coctx_t 指针指向的地址加上 32 个字节的内存位置加载到 esp 中*/
	/*最终：esp 寄存器实际上指向了当前协程 coctx_t 结构的 ss_size 成员位置，在它之下有个名为 regs 的数组，刚好是用来保存 8 个寄存器值的。*/
	pushl %eax //esp ->parm a /*将 eax 寄存器保存到了 coctx_t->regs[7] 的位置,将当前协程栈顶保存起来，以备下次调度回来时恢复栈地址*/

	/*保存各通用寄存器的值，到 coctx_t 结构的 regs[1]~regs[6] 的位置。*/
	pushl %ebp
	pushl %esi
	pushl %edi
	pushl %edx
	pushl %ecx
	pushl %ebx
	pushl -4(%eax)/*将 coctx_swap 的返回地址给保存起来了，放在coctx_t->regs[0] 的位置*/

	 /* 以MOVL 指令为界。第一部分是用于保存 current 协程的各个寄存器，第二部分则是恢复 pending 协程的寄存器。*/
	movl 4(%eax), %esp //parm b -> &regs[0]   /*将栈指针切到 pending 协程的 coctx_t 结构体开始，即 regs[0] 的位置*/ 

	popl %eax /*弹出 regs[0] 的值到 eax 寄存器,regs[0]保存的是coctx_swap的返回地址 //ret func addr*/
	/*从 regs[1]~regs[6] 恢复各寄存器的值*/
	popl %ebx  
	popl %ecx
	popl %edx
	popl %edi
	popl %esi
	popl %ebp
	popl %esp
	pushl %eax //set ret func addr

	xorl %eax, %eax   //清零eax寄存器
	ret   //执行返回指令

#elif defined(__x86_64__)
	leaq 8(%rsp),%rax
	leaq 112(%rdi),%rsp
	pushq %rax
	pushq %rbx
	pushq %rcx
	pushq %rdx

	pushq -8(%rax) //ret func addr

	pushq %rsi
	pushq %rdi
	pushq %rbp
	pushq %r8
	pushq %r9
	pushq %r12
	pushq %r13
	pushq %r14
	pushq %r15
	
	movq %rsi, %rsp
	popq %r15
	popq %r14
	popq %r13
	popq %r12
	popq %r9
	popq %r8
	popq %rbp
	popq %rdi
	popq %rsi
	popq %rax //ret func addr
	popq %rdx
	popq %rcx
	popq %rbx
	popq %rsp
	pushq %rax
	
	xorl %eax, %eax
	ret
#endif
