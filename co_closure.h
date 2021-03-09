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
libco中提供了一份闭包的实现，根据这份实现，我们不但可以把这个闭包用于线程，也可以用于协程。
因为libco中的函数表示通常使用函数指针，而不是std::function，所以没办法使用C++自带的闭包机制
*/
#ifndef __CO_CLOSURE_H__
#define __CO_CLOSURE_H__

//基类
//这段代码本质上定义了Closure的基类，闭包在调用时，最终会调用exec()函数
struct stCoClosure_t 
{
public:
	virtual void exec() = 0;
};

//1.base 
//-- 1.1 comac_argc
/*
__VA_ARGS__：代表全部的可变参数，相当于一个宏替换。
下面定义的6个宏主要分为以下两类：
comac_argc宏主要用于求出可变宏参数的个数。 注意：在这份实现中，最多支持7个宏参数求长度。
comc_join宏主要用于将两个参数拼接为一个参数。
*/
/*
comac_argc的参数中为什么要在__VA_ARGS__前加##？
在gcc中，前缀##有一个特殊约定，即当##arg前面是逗号(,)时，如果arg为空，则##之前的逗号(,)将会被自动省去。
因此，当comac_argc()不填写任何参数时，宏将会被按照以下方式展开：
comac_argc( ) -> comac_get_args_cnt( 0, 7,6,5,4,3,2,1,0 ) -> comac_arg_n( 0, 7,6,5,4,3,2,1,0 ) -> 0
但是，对于C++11(-std=c++11)，如果##arg中的arg为空，则##arg将会被默认转换为空字符串("")，此时，宏将会按照下面的方式展开：
comac_argc( ) -> comac_get_args_cnt( 0, “”, 7,6,5,4,3,2,1,0 ) -> comac_arg_n( 0, “”, 7,6,5,4,3,2,1,0 ) -> 1
*/
#define comac_get_args_cnt( ... ) comac_arg_n( __VA_ARGS__ )
#define comac_arg_n( _0,_1,_2,_3,_4,_5,_6,_7,N,...) N   //取第九位为N
#define comac_args_seqs() 7,6,5,4,3,2,1,0
#define comac_join_1( x,y ) x##y
#define comac_argc( ... ) comac_get_args_cnt( 0,##__VA_ARGS__,comac_args_seqs() )
#define comac_join( x,y) comac_join_1( x,y )


//-- 1.2 repeat
//根据宏参数个数调用对应的repeat_x宏
//这一部分的功能是对闭包传入的参数的类型声明，根据不同的参数数量调用不同的函数，其实是一个递归的过程
//这些宏主要用于定义重复操作。
#define repeat_0( fun,a,... ) 
#define repeat_1( fun,a,... ) fun( 1,a,__VA_ARGS__ ) repeat_0( fun,__VA_ARGS__ )
#define repeat_2( fun,a,... ) fun( 2,a,__VA_ARGS__ ) repeat_1( fun,__VA_ARGS__ )
#define repeat_3( fun,a,... ) fun( 3,a,__VA_ARGS__ ) repeat_2( fun,__VA_ARGS__ )
#define repeat_4( fun,a,... ) fun( 4,a,__VA_ARGS__ ) repeat_3( fun,__VA_ARGS__ )
#define repeat_5( fun,a,... ) fun( 5,a,__VA_ARGS__ ) repeat_4( fun,__VA_ARGS__ )
#define repeat_6( fun,a,... ) fun( 6,a,__VA_ARGS__ ) repeat_5( fun,__VA_ARGS__ )

#define repeat( n,fun,... ) comac_join( repeat_,n )( fun,__VA_ARGS__)

//2.implement
//宏中再去调用类型推断,这一部分的函数主要与参数的类型相关，可以根据传入的参数自动生成对类型的推导，可以用于函数参数的设定
//__cplusplus 宏用于获取 C++ 标准的版本号
#if __cplusplus <= 199711L
//typeof:用来判断变量类型
//decl_typeof：主要用于获取变量a的类型。
#define decl_typeof( i,a,... ) typedef typeof( a ) typeof_##a;
#else
#define decl_typeof( i,a,... ) typedef decltype( a ) typeof_##a;
#endif
//impl_typeof：主要用于创建一个和变量a的类型相同的引用。
#define impl_typeof( i,a,... ) typeof_##a & a;
//impl_typeof_cpy：主要用于创建一个和变量a类型相同的变量。
#define impl_typeof_cpy( i,a,... ) typeof_##a a;
// con_param_typeof：用于生成类构造函数入参。
#define con_param_typeof( i,a,... ) typeof_##a & a##r,
// param_init_typeof：用于生成类构造函数初始化列表。
#define param_init_typeof( i,a,... ) a(a##r),

//2.1 reference
//co_ref所做的事情其实就是根据闭包传入的参数生成一个类，这个类持有了对于所有参数的引用，并可以推导出参数的数量。
//这段宏定义，主要用于产生协程入参
#define co_ref( name,... )\
repeat( comac_argc(__VA_ARGS__) ,decl_typeof,__VA_ARGS__ )\
class type_##name\
{\
public:\
	repeat( comac_argc(__VA_ARGS__) ,impl_typeof,__VA_ARGS__ )\
	int _member_cnt;\
	type_##name( \
		repeat( comac_argc(__VA_ARGS__),con_param_typeof,__VA_ARGS__ ) ... ): \
		repeat( comac_argc(__VA_ARGS__),param_init_typeof,__VA_ARGS__ ) _member_cnt(comac_argc(__VA_ARGS__)) \
	{}\
} name( __VA_ARGS__ ) ;


//2.2 function
//这个宏创建一个协程，co_func经过宏展开后，生成了一个名称为f的类。只要创建这个类的实例，然后调用exec()方法，即可运行协程。
#define co_func(name,...)\
repeat( comac_argc(__VA_ARGS__) ,decl_typeof,__VA_ARGS__ )\
class name:public stCoClosure_t\
{\
public:\
	repeat( comac_argc(__VA_ARGS__) ,impl_typeof_cpy,__VA_ARGS__ )\
	int _member_cnt;\
public:\
	name( repeat( comac_argc(__VA_ARGS__),con_param_typeof,__VA_ARGS__ ) ... ): \
		repeat( comac_argc(__VA_ARGS__),param_init_typeof,__VA_ARGS__ ) _member_cnt(comac_argc(__VA_ARGS__))\
	{}\
	void exec()

#define co_func_end }


#endif

