#include "co_routine.h"
 
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <sys/time.h>
#include <stack>
 
#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/un.h>
#include <fcntl.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <errno.h>
#include <sys/wait.h>
 
#include <sys/epoll.h>
#ifdef __FreeBSD__
#include <cstring>
#include <sys/types.h>
#include <sys/wait.h>
#endif
 
#include<iostream>
#include<queue>
#include"co_routine.cpp"
using namespace std;
 
template<typename dataT>struct link_t;
template<typename dataT>struct linkNode_t;
 
struct task_t
{
    stCoRoutine_t *co;//协程控制字
    int workNumb;//epoll中的socket个数
    int addNumb;//新来的数目
    link_t<int> *acLink;
    linkNode_t<task_t*> *dptLinkNode;//在调度器链表中的位置
    int taskNumb;//协程号
};
 
 
template<typename dataT>
struct linkNode_t
{
    struct link_t<dataT> *pLink;
    struct linkNode_t *pNext;
    struct linkNode_t *pPrev;
    dataT data;
};
template<typename dataT>
struct link_t
{
    linkNode_t<dataT> *head;
    linkNode_t<dataT> *tail;
};
typedef struct dpt_t
{
    link_t<task_t*>taskLink;
    int coActivNumb;
}dpt_t;
 
 
#define CoNumber 128
#define EpNumber 1024 //一共CoNumber*EpNumber
//#define MinAlloc 100    //协程epoll最小处理个数（如果只有一个worker则不限制）
 
 
 
#define AGAINTIMES 3
#define MSGHeadLen 6
#define MSGBodyLen 1024
 
int const DPTTime=1;    //调度器调度最小周期，可以调频
int const AcTimeOut=10;  //socket Ac 频率
 
void debug(char const *p)
{
 //  return;
 //   printf("%s\n",p);
    return;
}
 
typedef int (*valueFunc_t)(void*,int);
template<int maxSize,valueFunc_t valueFunc> //，size，值计算函数，下标外部从0开始，内部从1开始
class segmentTree
{
 
    #define lsonrt rt<<1
    #define rsonrt rt<<1|1
    void *ori;//原数组
    int tree[maxSize<<2];
    int ip;
    int value(int idx)
    {
        return (*valueFunc)(ori,idx-1);
    }
    inline void pushup(int rt)
    {
        tree[rt]=min(tree[rt<<1],tree[rt<<1|1]);
    }
public:
    bool empty()
    {
        return !ori;
    }
    void build(int l,int r,int rt)
    {
        if(l==r)
        {
            tree[rt]=value(++ip);
            return;
        }
        build(l,(l+r)>>1,rt<<1);
        build(((l+r)>>1)+1,r,rt<<1|1);
        pushup(rt);
    }
    segmentTree()
    {
        ori=NULL;
        ip=0;
    }
    bool setOri(void *ori)  //注意，ori数组下标从0开始。
    {
        if(ori==NULL)
        {
            debug("segment tree.ori==NULL");
            exit(1);
        }
        if(this->ori)
            return false;
        this->ori=ori;
        ip=0;
        build(1,maxSize,1);
        return true;
    }
    int queryMin()//返回最小值
    {
        if(ori==0)
        {
            cout<<"segmentTree.oir==NULL"<<endl;
            exit(1);
        }
        return tree[1];
    }
    int queryIdx()//返回最小值的下标
    {
        if(ori==NULL)
        {
            cout<<"segmentTree.oir==NULL"<<endl;
            exit(1);
        }
        int rt=1;
        int l=1,r=maxSize;
        while(l<r)
        {
            if(tree[lsonrt]<tree[rsonrt])
            {
                rt=lsonrt;
                r=(l+r)>>1;
            }
            else
            {
                rt=rsonrt;
                l=((l+r)>>1)+1;
            }
        }
        return l-1;
    }
    void update(int idx)
    {
        idx++;
 
        int rt=1;
        int l=1,r=maxSize;
        int mid;
        while(l<r)  //先找到idx的rt
        {
            mid=(l+r)>>1;
            if(idx<=mid)
            {
                rt=lsonrt;
                r=(l+r)>>1;
            }
            else
            {
                rt=rsonrt;
                l=((l+r)>>1)+1;
            }
        }
        //再递归地修改
        tree[rt]=value(l);
        while(rt)
        {
            rt>>=1;
            pushup(rt);
        }
    }
};
int valueFunc(void *arr,int idx)
{
    return ((task_t*)arr)[idx].addNumb+((task_t*)arr)[idx].workNumb;
}
segmentTree<CoNumber,valueFunc>coWoker;
link_t<int> acQue;
task_t _coWoker[CoNumber]={0};//----------------------------------全局变量-----------------
dpt_t dpt;
 
 
 
int myErrorOperate(char const * const error_str,int error_line,int error_exit=1)
{
    perror(error_str);
    printf("%d\n",error_line);
    if(error_exit==1)
        exit(1);
    return 0;
}
template<typename TLink,typename TNode>
void AddHead(TLink * apLink,TNode *ap)
{
    if(apLink==NULL || ap==NULL)
    {
        myErrorOperate("AddHead.apLink==NULL || ap==NULL",__LINE__,1);
    }
    if(apLink->head)
    {
        apLink->head->pPrev=(TNode*)ap;
        ap->pPrev=NULL;
        ap->pNext=apLink->head;
        apLink->head=ap;
    }
    else
    {
        apLink->head=apLink->tail=ap;
        ap->pNext=ap->pPrev=NULL;
    }
    ap->pLink=apLink;
}
 
namespace mempool_namespace
{
class memPool;
map<int,memPool*>memPoolMap;
class memPool{
private:
    int unitSize;//单位大小
    typedef unsigned int uint;
    long topPtr;//指向一个空闲内存的块号
    ///31~21共11位表示node号，20~0共21位表示块内编号，块内最多盛放2M个。
 
    #define OffsetBits 21
    #define MaskOffsetNumb 07777777  //块内编号掩码为：07777777
 
    #define InitNumInNode 2
    #define MaxOffset (1024*1024)
    #define MaxNodeNum 127    //127  这个值的最大值跟node编号位数有关(2*1024-1)，但是大了影响效率
    #define MaxAllocMem 128*1024*1024  //(128*1024*1024) //单位B     //内存node内存上限（实际要大一个单位用来内存对齐浪费）
    #define EnableAutoClean 1   //允许自动删除小节点以提高速度 ，注意是自动。。。。如果此定义为0，用户仍然可以手动调用clean来完成这个功能
    #define AutoCleanSize (maxNumInNode>>4)    //自动清理的上限,默认16倍 (inclusive)
    struct  memNode
    {
        char *memory;
        char *head;
        bool deleted;
        uint size;
        uint used;
        uint *ptrArr;
        uint top;
        uint nptr;//下一个node的节点号
        uint fptr;//上一个node的节点号
    };
    uint nodeNum;//当前nodeNum
    uint deletedNum;
    uint MaxAllocNum;
    uint maxNumInNode;
    memNode **nodeArr=NULL;
    bool status;    //pool is available?
    void freeMemNode(int idx) //删除被标记为deleted的node
    {
        if(__builtin_expect(MaxNodeNum<idx,0))
        {
            myErrorOperate("fatal error ! freeMemNode.nodeNumidx.",__LINE__,1);
            return;
        }
        if(__builtin_expect(!nodeArr[idx],0))
        {
            myErrorOperate("freeMemNode.node is already NULL.",__LINE__,0);
            return;
        }
        if(!__builtin_expect(nodeArr[idx]->deleted,1))
        {
            myErrorOperate("freeMemNode.deleted must be true.",__LINE__,0);
            return;
        }
        int fptr=nodeArr[idx]->fptr;
        int nptr=nodeArr[idx]->nptr;
        nodeArr[fptr]->nptr=nptr;
        if(nptr<=MaxNodeNum)
        {
            nodeArr[nptr]->fptr=fptr;
        }
 
        free(nodeArr[idx]->memory);
        free(nodeArr[idx]->ptrArr);
        free(nodeArr[idx]);
        nodeArr[idx]=NULL;
        deletedNum--;
    }
    int createNewNode(uint num)//返回值是新开辟的位置
    {
        if(__builtin_expect(nodeNum+deletedNum>=MaxNodeNum,0))
        {
            myErrorOperate("memPool.nodes are too many.",__LINE__,0);
            return -1;
        }
        if(num<=1)
        {
            myErrorOperate("createNewNode.num must >1",__LINE__,1);
        }
        int i;
        for(i=0;i<MaxNodeNum && nodeArr[i]!=NULL;i++);//ok 寻找一个空节点
        memNode *node=(memNode*)malloc(sizeof(memNode));
        if(__builtin_expect(node==NULL,0))
            return -1;
     //   memset(node,0,sizeof(memNode));
        nodeArr[i]=node;
        node->used=0;
        node->deleted=0;
        node->memory=(char*)malloc(unitSize*(num+1));
        if(__builtin_expect(node->memory==NULL,0))
            return -1;
        MaxAllocNum=max(num,MaxAllocNum);
        node->head=node->memory+unitSize-((long long)node->memory%unitSize);//内存对齐
        node->size=num;
        node->ptrArr=(uint*)malloc(num*sizeof(uint*));
        if(__builtin_expect(node->ptrArr==NULL,0))
        {
            return -1;
        }
        node->fptr=i-1;
        node->nptr=nodeArr[i-1]->nptr;
        nodeArr[i-1]->nptr=i;
        if(node->nptr < MaxNodeNum)
        {
            nodeArr[node->nptr]->fptr=i;
        }
 
        for(int j=1;j<=num;j++)//ok
        {
            node->ptrArr[j-1]=j;
        }
        node->top=0;
        nodeNum++;
        return i;
    }
public:
    ~memPool()
    {
        for(int i=0;i<MaxNodeNum;i++)
        {
            if(nodeArr[i])
            {
                free(nodeArr[i]->memory);
                free(nodeArr[i]->ptrArr);
                free(nodeArr[i]);
            }
        }
        free(nodeArr);
    }
    void memPoolInit()
    {
        int s=sizeof(memNode*)*(MaxNodeNum+1);
        nodeArr=(memNode**)malloc(s);
        if(__builtin_expect(nodeArr==NULL,0))
        {
            myErrorOperate("memPool.nodeArr.create fail",__LINE__,0);
            return;
        }
        memset(nodeArr,0,s);
        //初始化头结点
        memNode *node=(memNode*)malloc(sizeof(memNode));
        if(__builtin_expect(node==NULL,0))
        {
            myErrorOperate("memPool.create head node fail",0);
            return;
        }
        nodeArr[0]=node;
        node->used=0;
        node->deleted=0;
        node->memory=NULL;
        node->head=NULL;
        node->size=0;
        node->ptrArr=NULL;
        node->fptr=0;
        node->nptr=MaxNodeNum+1;
        node->top=-1;
 
        //创建第一个节点
        int ret=createNewNode(maxNumInNode);
        if(ret<0)
        {
            myErrorOperate("memPool.createNewNode.fail",__LINE__,1);
        }
        topPtr=ret<<OffsetBits;//指向新开辟的node
        status=1;
    }
    memPool(int nodeSize,int initNum=InitNumInNode)
    {
        status=0;
        unitSize=nodeSize;
        topPtr=-1;
        nodeNum=0;
        deletedNum=0;
        maxNumInNode=initNum;
        MaxAllocNum=MaxAllocMem/nodeSize;
        if(MaxAllocMem<16)
        {
            myErrorOperate("memPool.MaxAllocMem is too small.",__LINE__,1); //开发阶段就很容易找到的错误
            return;
        }
 
        if(maxNumInNode>MaxAllocNum)
        {
            maxNumInNode=MaxAllocNum;
        }
        memPoolInit();
    }
    void* allocMemFromPool()
    {
        if(__builtin_expect(status==0,0))
        {
            memPoolInit();
            if(status==0)
            {
                return malloc(unitSize);
            }
        }
        if(topPtr<=-1)
        {
            maxNumInNode=min(maxNumInNode<<1,MaxAllocNum);
            int ret=createNewNode(maxNumInNode);
            topPtr=ret<<OffsetBits;
            if(ret==-1)
            {
                topPtr=-1;
                myErrorOperate("allocMemFromPool.there is too many unit. So . Have to call to malloc.",__LINE__,0);
                return malloc(unitSize);
            }
        }
        #define alloc_head(idx) (node->head+idx*unitSize)
        #define alloc_ptr(idx) (node->ptrArr+idx)
        uint nodeIdx=topPtr>>OffsetBits;
        uint offsetIdx=topPtr & MaskOffsetNumb;
        if(nodeArr[nodeIdx]==NULL || nodeArr[nodeIdx]->deleted)      //如果当前节点不存在 or 被删除了
        {
            for(nodeIdx=0;nodeIdx<=MaxNodeNum;)
            {
                if(__builtin_expect(nodeArr[nodeIdx]==NULL,0))
                    nodeIdx++;
                else if(nodeArr[nodeIdx]->deleted==1 || nodeIdx==0)
                {
                    nodeIdx=nodeArr[nodeIdx]->nptr;
                }
                else
                    break;
            }
            if(nodeIdx>MaxNodeNum)
            {
                topPtr=-1;
                return allocMemFromPool();
            }
            offsetIdx=nodeArr[nodeIdx]->top;
        }
        memNode *node=nodeArr[nodeIdx];
 
        void*retPtr=alloc_head(offsetIdx);
        if(__builtin_expect(node->used >= node->size,0)) //当前节点容纳不下
        {
            int i;
            for(i=nodeArr[0]->nptr;i<=MaxNodeNum;)
            {
                if(__builtin_expect(nodeArr[i]==NULL,0))
                    i++;
                else if(nodeArr[i]->deleted==1 || nodeArr[i]->used >= nodeArr[i]->size)
                {
                    i=nodeArr[i]->nptr;
                }
                else //能容得下
                {
                    node=nodeArr[i];
                    retPtr=node->head+node->top;
                    node->used++;
 
                    node->top=*alloc_ptr(node->top);
                    topPtr=i<<OffsetBits;
                    topPtr|=node->top;
                    return retPtr;
                }
            }
            //当前容纳不下且也没有节点能容纳下
            topPtr=-1;
            return allocMemFromPool();  //一层递归
        }
        else
        {
            node->used++;
            node->top=*alloc_ptr(offsetIdx);
            topPtr&=~MaskOffsetNumb;
            topPtr|=node->top;
        }
        return retPtr;
    }
    void freeMemToPool(void *p)
    {
        #define free_end(idx) (nodeArr[idx]->head+unitSize*nodeArr[idx]->size)
        #define free_head(idx) (nodeArr[idx]->head)
        int i;
        char *ptr=(char*)p;
        for(i=nodeArr[0]->nptr;i<=MaxNodeNum;)
        {
            if(__builtin_expect(nodeArr[i]==NULL,0))
                i++;
            else if(!(p>=free_head(i) && p<free_end(i)))
            {
                i=nodeArr[i]->nptr;
            }
            else
            {
                memNode *node=nodeArr[i];
                uint idx=(ptr-node->head)/unitSize;
                *alloc_ptr(idx)=node->top;
                node->top=idx;
                topPtr=i<<OffsetBits;
                topPtr|=node->top;
                node->used--;
                #if EnableAutoClean
                if(__builtin_expect(nodeArr[i]->deleted == 0 && nodeArr[i]->size <= AutoCleanSize,0))
                {
                    nodeArr[i]->deleted=1;
                    nodeNum--;
                    deletedNum++;
                }
                #endif
                if(!nodeArr[i]->used && nodeArr[i]->deleted)
                {
                    //如果打了deleted标记的node已经没有成员了，就删除
                    freeMemNode(i);
                }
                return;
            }
        }
        myErrorOperate("freeMemToPool.mem.Release mem to OS instand of to pool.",__LINE__,0);
        free(p);
    }
//    void debug()
//    {
//        cout<<"maxNumInNode="<<maxNumInNode<<endl;
//        cout<<"AutoCleanSize="<<AutoCleanSize<<endl;
//        cout<<"topPrt=";
//        printf("%x\n",topPtr);
//        for(int i=0;i<=MaxNodeNum;i++)
//        {
//            cout<<"["<<i<<"]"<<"deleted=";
//            if(nodeArr[i]==NULL)
//            {
//                cout<<"NULL"<<endl;
//            }
//            else
//            {
//                cout<<nodeArr[i]->deleted<<" ";
//                printf("%x\t",nodeArr[i]->head);
//                cout<<"fptr="<<nodeArr[i]->fptr<<" nptr="<<nodeArr[i]->nptr;
//                cout<<" used/size="<<nodeArr[i]->used<<"/"<<nodeArr[i]->size<<endl;
//       //         for(int j=0;j<nodeArr[i]->size;j++)
//       //         {
//       //             cout<<nodeArr[i]->ptrArr[j]<<" ";
//        //        }
//       //         cout<<endl;
//            }
//          //  return;
//
//        }
//        return;
//    }
    int clean(long long size=-1)
    {
        if(size<0)
        {
            size=AutoCleanSize;
        }
        if(size>=maxNumInNode)
        {
            return -1;  //把最大的都删了就不合适了
        }
        int cnt=0;
        for(int i=1;i<=MaxNodeNum;i++)
        {
            if(nodeArr[i] && nodeArr[i]->size<=AutoCleanSize && !nodeArr[i]->deleted)
            {
                nodeArr[i]->deleted=1;
                cnt++;
            }
        }
        nodeNum-=cnt;
        deletedNum+=cnt;
        return cnt;
    }
};
int do_createMemPool(int nodeSize,int initNum,map<int,memPool*>&memPoolMap)//初始个数
{
    if(memPoolMap.count(nodeSize))//不允许重复建立
    {
        myErrorOperate("不允许重复建立",__LINE__,1);
        return 1;
    }
    memPool *pool=new memPool(nodeSize,initNum);
    memPoolMap[nodeSize]=pool;
}
template<typename T>
int createMemPool(int initNum=InitNumInNode)
{
    return do_createMemPool(sizeof(T),initNum,memPoolMap);
}
}
template<typename T>
void freeLinkNode(T* p)
{
    using namespace mempool_namespace;
    if(memPoolMap.count(sizeof(T)))
    {
        memPool *pool=memPoolMap[sizeof(T)];
        pool->freeMemToPool(p);
        if(memPoolMap.count(sizeof(T))==0)
        {
            myErrorOperate("free.err",__LINE__);
        }
    }
    else
        free(p);
    return;
}
#define MEMPOOLON 1
template<typename T>
T* mallocLinkNode()
{
    #ifdef MEMPOOLON
    using namespace mempool_namespace;
    if(__builtin_expect(!memPoolMap.count(sizeof(T)),0))
    {
        createMemPool<T>();
    }
    memPool &pool=*memPoolMap[sizeof(T)];
    return (T*)pool.allocMemFromPool();
    #else
    return malloc(sizeof(T));
    #endif
}
template<typename T>
T* callocLinkNode(int n)
{
    return (T*)calloc(n,sizeof(T));
}
char* mallocMSGBodyMem()
{
    return (char*)malloc(MSGBodyLen);
}
void freeMSGBodyMem(char* p)
{
    free(p);
}
 
using namespace mempool_namespace;
 
int co_accept(int fd, struct sockaddr *addr, socklen_t *len );
 
void queAcor()  //将acQue成员分配给worker们
{
    if(acQue.tail==NULL || coWoker.empty())
    {
        return;
    }
    int lst;//空间
    linkNode_t<int> *ptr;
    int i=0;
    for(;acQue.head;)
    {
        if(coWoker.empty())
            break;
        int oldIdx=coWoker.queryIdx();
        task_t *old=&_coWoker[oldIdx];
        if(old->addNumb+old->workNumb==0)
        {
            dpt.coActivNumb++;
        }
        else  if(old->addNumb+old->workNumb==EpNumber)
        {
            myErrorOperate("Acor:too many users err.Refuse connect request.",__LINE__,0);
            ptr=acQue.head;
            RemoveFromLink<linkNode_t<int>,link_t<int> >(ptr);
            close(ptr->data);   //拒绝请求
            freeLinkNode(ptr);
        /*    cout<<"0号协程:" <<_coWoker[0].addNumb+_coWoker[0].workNumb<<endl;
            cout<<"1号协程:" <<_coWoker[1].addNumb+_coWoker[1].workNumb<<endl;
            cout<<"超限协程:" <<old->addNumb+old->workNumb<<endl;
            cout<<"协程号"<<old->taskNumb<<endl;
            cout<<"oldIdx:"<<oldIdx<<endl;
            cout<<"debug ends"<<endl;*/
            break;
        }
        task_t &top=*(old);
        lst=EpNumber - top.workNumb - top.addNumb;
        if(lst>0)
        {
            ptr=acQue.tail;
            for(i=0;i<lst && ptr!=NULL;i++,ptr=acQue.tail)
            {
                RemoveFromLink<linkNode_t<int>,link_t<int> >(ptr);
                AddTail(top.acLink,ptr);
                top.addNumb++;
            }
        }
        linkNode_t<task_t*> *tmp=top.dptLinkNode;
        //在task中加入socket之后，将它在dpt的位置移动到头部（主要是当它在coLstIdx之后的时候，把它移到前面活跃起来。）
        RemoveFromLink<linkNode_t<task_t*>,link_t<task_t*> >(tmp);
        AddHead(&dpt.taskLink,tmp);
        coWoker.update(oldIdx);
    }
}
 
static void* dispatcher(void* arg)//协程调度器
{
    static int ActiveCnt=-1;
    task_t *taskQue=(task_t*)arg;
    link_t<task_t*> &link=dpt.taskLink;
    //init
    cout<<"dpt.init"<<endl;
    for(int i=0;i<CoNumber;i++){
        linkNode_t<task_t*> *nodeTmp=mallocLinkNode<linkNode_t<task_t* > >();
        nodeTmp->pLink=NULL;
        nodeTmp->data=&taskQue[i];
        nodeTmp->data->dptLinkNode=nodeTmp;
        AddTail(&link,nodeTmp);
    }
    cout<<"dpt.init.end"<<endl;
    linkNode_t<task_t*> *nodeTmp=NULL;
    for(;;)
    {
        linkNode_t<task_t*> *ptr=link.head;
        if(ptr!=NULL)
        {
 
            int tmp=dpt.coActivNumb;
      //      cout<<"工作协程号：";
            for(int i=0;ptr && i<tmp;i++)
            {
                if(ptr->data->workNumb+ptr->data->addNumb)//如果这个协程有工作
                {
                    co_resume(ptr->data->co);
         //           cout<<ptr->data->taskNumb<<' ';
                    ptr=ptr->pNext;
                }
                else//没有工作就把它取出，然后加到后边
                {
          //          cout<<"没有工作："<<ptr->data->taskNumb<<endl;
                  //  dpt.coActivNumb--;
                    nodeTmp=ptr->pNext;
                    RemoveFromLink<linkNode_t<task_t*>,link_t<task_t*> >(ptr);
                    AddTail(&dpt.taskLink,ptr);
                    if(ptr->pNext==nodeTmp && nodeTmp)
                    {
       //                 cout<<nodeTmp<<endl;
                        myErrorOperate("bug****",__LINE__,1);
                    }
                    ptr=nodeTmp;
                }
            }
      //      cout<<endl;
        }
        if(ActiveCnt!=dpt.coActivNumb)
        {
            ActiveCnt=dpt.coActivNumb;
   //         printf("dispatcher:Active worker numb : %d\n",dpt.coActivNumb);
        }
        struct pollfd pf={0};//不关心epoll时间，只关心时间轮超时事件。
        co_poll(co_get_epoll_ct(),&pf,1,DPTTime);//1ms执行一次
    }
    myErrorOperate("dpt:exit err",__LINE__,1);
}
 
static void* mcoListen(void *arg_co)
{
    co_enable_hook_sys();
    int lsEpFd;
    lsEpFd=co_epoll_create(128);
    if(lsEpFd<0)
    {
        myErrorOperate("create listen_epfd err",__LINE__);
    }
    int lsSocketFd;
    if((lsSocketFd=socket(AF_INET,SOCK_STREAM,0))<0)
    {
        close(lsEpFd);
        myErrorOperate("create listen_socket fd err.",__LINE__);
    }
    //set socket opt
    int ret,val=1;
    ret=setsockopt(lsSocketFd,SOL_SOCKET,SO_REUSEADDR,(void*)&val,sizeof(val));
    //reuse addr
    if(ret<0)
    {
        myErrorOperate("set SO_REUSEADDR err.",__LINE__,0);
    }
    struct sockaddr_in saddr;
    memset(&saddr,0,sizeof(sockaddr_in));
    saddr.sin_family=AF_INET;
    saddr.sin_addr.s_addr=INADDR_ANY;
    saddr.sin_port=htons(8001);
    ret=bind(lsSocketFd,(struct sockaddr*)&saddr,sizeof(struct sockaddr_in));
    if(ret<0)
    {
        myErrorOperate("lsten socket bind err.",__LINE__,1);
    }
    ret=listen(lsSocketFd,1024);//backlog
    if(ret<0)
    {
        myErrorOperate("listen err.",__LINE__,1);
    }
 
    socklen_t saddrLen;
    ret=-1;
    for(;;)
    {
        saddrLen=sizeof(saddr);
        ret=co_accept(lsSocketFd,(struct sockaddr*)&saddr,&saddrLen);
       if(ret<0)//每次poll超时后都需要重新加入。
        {
      //      cout<<"acor"<<endl;
            queAcor();//将ac列表中的socket分配给coWoker
      //      cout<<"endAcor"<<endl;
            struct pollfd pf={0};//不关心epoll时间，只关心时间轮超时事件。
            pf.fd=lsSocketFd;
            pf.events=(EPOLLERR|EPOLLHUP|EPOLLIN);
            co_poll(co_get_epoll_ct(),&pf,1,AcTimeOut);//yield   同时关心epoll事件，和1000ms的超时事件
        }
        else if(ret>=0)
        {
            linkNode_t<int> *t=mallocLinkNode<linkNode_t<int> >();
            t->pLink=NULL;
            if(t==NULL)
            {
                close(ret);//若超出了处理范围则close
                myErrorOperate("socket acQue has too many node.",__LINE__,0);
            }
            t->data=ret;//fd
            AddTail(&acQue,t);
        }
    }
}
void * echoFunc(void *args)
{
    co_enable_hook_sys();
    for(;;)
    {
        cout<<"time 0.5"<<endl;
        struct pollfd pf={0};//不关心epoll时间，只关心时间轮超时事件。
        co_poll(co_get_epoll_ct(),&pf,1,500);//500ms打印一次
    }
}
 
void workerAddNew(task_t &taskSelf,int epFd)
{
    int ret;
    linkNode_t<int> *ptr=taskSelf.acLink->tail;
    struct epoll_event *ep;
    for(int i=0;i<taskSelf.addNumb;i++)
    {
        ep=mallocLinkNode<struct epoll_event>();
        ep->events=(EPOLLERR | EPOLLIN | EPOLLHUP);
        ep->data.ptr=(void*)ptr;
        ret=epoll_ctl(epFd,EPOLL_CTL_ADD,ptr->data,ep);
        if(ret<0)
        {
            myErrorOperate("worker:epoll_ctl err.",__LINE__,0);
            co_yield_ct();
            i--;
            freeLinkNode(ep);
            continue;
        }
        ptr=ptr->pPrev;
    }
    taskSelf.workNumb+=taskSelf.addNumb;
    taskSelf.addNumb=0;
}
int sendToSocket(int fd,char* buff,int len,bool co=1)
{
    int idx=0;
    int ret;
    int again=AGAINTIMES+1;
    errno=0;
    while(len>idx && again)
    {
        ret=send(fd,buff+idx,len-idx,0);
        if(ret<=0)
        {
            if(errno==EINTR || errno==EAGAIN)
            {
                if(errno==EAGAIN)
                {
                    again--;
                }
                if(co)
                {
                    co_yield_ct();
                }
                continue;
            }
            return ret;
        }
        idx+=ret;
    }
    printf("发送：\n");
    for(int i=0;i<len;i++){
        printf("%c",buff[i]);
    }
    printf("\n");
    return len;
}
int recvFromSocket(int fd,char*buff,int len,int line,bool co=1)
{
 
    memset(buff , 0 , len);
    errno=0;
    int idx=0;
    int ret;
    int again=AGAINTIMES+1;
    while(len>idx && again)
    {
        ret=recv(fd,buff+idx,len-idx,MSG_WAITALL);
     //   perror("rcv form socket:");
        if(ret==0)
        {
 
        }
        if(ret<=0)
        {
      //      cout<<"ret=="<<ret<<endl;
      //      perror("rcv.");
            if(errno==EINTR || errno==EAGAIN)
            {
     //           cout<<"rcv err"<<errno<<endl;
                if(errno==EAGAIN)
                {
                    again--;
                }
                if(co)
                {
                    co_yield_ct();
                }
                continue;
            }
            return ret;
        }
        idx+=ret;
    }
    if(idx<len || again==0)
    {
        return -1;
    }
    printf("接受到：\n");
    for(int i=0;i<len;i++){
        printf("%c",buff[i]);
    }
    printf("\n");
    return len;
}
bool checkSocketConnect(task_t &taskSelf,linkNode_t<int>*ptr,int ret,int line)
{
    if(ret<=0)
    {
        if(ret==0)
        {
            debug("client closed");
        }
        else
        {
            debug("client err");
        }
        RemoveFromLink<linkNode_t<int>,link_t<int> >(ptr);
        close(ptr->data);
        freeLinkNode(ptr);
        taskSelf.workNumb--;
        coWoker.update(taskSelf.taskNumb);
        return 1;
    }
    return 0;
}
void *wokerRoutine(void *_coWoker) //系统默认协程栈大小为128KB
{
 
    co_enable_hook_sys();
    link_t<int> *linkTmp = mallocLinkNode<link_t<int> >();
    memset(linkTmp,0,sizeof(link_t<int>));
    static struct epoll_event evtOut[EpNumber];//因为每次运行时协程都会重新取出epoll中active事件，所以此缓冲区可以共享。
    task_t &taskSelf=*(task_t*)((task_t*)_coWoker);
    //接受数据，发送数据。
    int epFd=-1;
    int ret;
    for(;;)
    {
        if(taskSelf.workNumb+taskSelf.addNumb==0)
        {
            co_yield_ct();
            continue;
        }
        if(epFd==-1)
        {
            epFd=epoll_create(100);
            if(epFd<0)
            {
                myErrorOperate("WokerEp epoll_create err.With CoNumber=",__LINE__,0);
                co_yield_ct();
                continue;
            }
        }
        //将新的加入
        workerAddNew(taskSelf,epFd);
        //处理事件
        int activeNumb;
        errno=0;
        activeNumb=epoll_wait(epFd,evtOut,taskSelf.workNumb,0);//这里的延时时间写0不知道有没有问题？？？？？？？？？？？？？？？？？？？？？？？？？？？？？？？？、
        if(activeNumb<0)
        {
            myErrorOperate("epoll_wait err",__LINE__,0);
            co_yield_ct();
            continue;
        }
        char msgHead[MSGHeadLen+1];//如果是static可能会非协程安全。
        msgHead[MSGHeadLen]=0;//结束符
        for(int i=0;i<activeNumb;i++)
        {
            linkNode_t<int> * ptr=(linkNode_t<int> *)evtOut[i].data.ptr;//ptr是workLink中的fd对应节点。该节点data域为fd
            int sFd=ptr->data;
            int evt=evtOut[i].events;
            if(evt&EPOLLERR || evt&EPOLLHUP )
            {
                if(evt&EPOLLERR)
                {
                    myErrorOperate("user socket err.user exit",__LINE__,0);
                }
                else
                {
        //            printf("user exit\n");
                }
                checkSocketConnect(taskSelf,ptr,0,__LINE__);
                continue;
            }
            else if(evt&EPOLLIN)
            {
                int len;
                errno=0;
                ret=recvFromSocket(sFd,msgHead,MSGHeadLen,__LINE__);//读长度信息
                if(checkSocketConnect(taskSelf,ptr,ret,__LINE__))
                    continue;
                ret=sendToSocket(sFd,msgHead,MSGHeadLen);//发送长度信息
                if(checkSocketConnect(taskSelf,ptr,ret,__LINE__))
                {
                    continue;
                }
                int msgLen=atoi(msgHead);
                char *msgBody=mallocMSGBodyMem();
                len=0;
                int sdLen;
                bool cls=0;
                while(len<msgLen)
                {
                    errno=0;
                    ret=recvFromSocket(sFd,msgBody,min(MSGBodyLen,msgLen-len),__LINE__);//接受一段body信息
                    len+=ret;
                    if(checkSocketConnect(taskSelf,ptr,ret,__LINE__))
                    {
                        cls=1;
                        break;
                    }
                    sdLen=ret;
                    ret=sendToSocket(sFd,msgBody,sdLen);                       //将这一段body发送出去
                    if(checkSocketConnect(taskSelf,ptr,ret,__LINE__))
                    {
                        cls=1;
                        break;
                    }
                }
                freeMSGBodyMem(msgBody);
                if(cls)
                    continue;
            }
            else
            {
                myErrorOperate("unkown err",__LINE__,0);
            }
        }
        if(taskSelf.addNumb+taskSelf.workNumb==0)
        {
            dpt.coActivNumb--;
        }
        co_yield_ct();
    }
    return NULL;
}
 
void main_init()
{
    acQue.head=acQue.tail=NULL;
 
    struct sigaction sa;    //这三行:设置向已关闭socket发送信息时默认行为。防止向已关闭socket发送数据导致服务器崩溃
    sa.sa_handler = SIG_IGN;
    sigaction( SIGPIPE, &sa, 0 );
}
int main() {
    cout<<"服务器初始化..."<<endl;
    main_init();
    task_t coLs,coDpt;
    cout<<"创建调度..."<<endl;
    co_create(&(coDpt.co),NULL,dispatcher,(void*)_coWoker);//init _coWoker.dpt
    cout<<"第一个内存池初始化"<<endl;
    link_t<int> *acLinkMem=callocLinkNode<link_t<int> >(CoNumber);
    cout<<"协程池初始化..."<<endl;
    for(int i=0;i<CoNumber;i++)
    {
        _coWoker[i].taskNumb=i;//引用线段树，协程调度输出标识
        _coWoker[i].acLink=acLinkMem+i;
        co_create(&(_coWoker[i].co),NULL,wokerRoutine,&_coWoker[i]);
        co_resume(_coWoker[i].co);
    }
    cout<<"segment tree init"<<endl;
    coWoker.setOri((void*)&_coWoker);
    cout<<"创建listen..."<<endl;
    co_create(&(coLs.co),NULL,mcoListen,&coLs);
 
    cout<<"启动listen..."<<endl;
    co_resume(coLs.co);//启动ac
    cout<<"启动调度..."<<endl;
    co_resume(coDpt.co);
 
    cout<<"服务器初始化完毕，启动服务."<<endl;
    co_eventloop(co_get_epoll_ct(),0,0);
    return 0;
}
