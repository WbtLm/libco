#include <stdio.h>
#include <time.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdlib.h>
int main(int argc,char**args)
{
    if(argc<2){
        printf("argc<2");
        return 0;
    }
        struct sockaddr_in server_sockaddr;
        memset(&server_sockaddr,0,sizeof(server_sockaddr));
        server_sockaddr.sin_family=AF_INET;
        server_sockaddr.sin_port=htons(atoi(args[1]));
        server_sockaddr.sin_addr.s_addr=htonl(INADDR_ANY);
        //创建一个socket
        int sockid=socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);
        const int on=1;
        //设置重复利用端口，如果不设置在频繁调试时会出现端口被占用情况，导致bind失败
        setsockopt(sockid,SOL_SOCKET,SO_REUSEADDR,&on,sizeof(on));
        //bind
        if(bind(sockid,(struct sockaddr *)&server_sockaddr,sizeof(server_sockaddr))<0)
        {
                printf("bind\n");
                return 0;
        }
        //listen
        if(listen(sockid,SOMAXCONN)<0)
        {
                printf("listen\n");
                return 0;
        }
        //获得连接的客户端信息
        struct sockaddr_in client_sockaddr;
        socklen_t client_socklen=sizeof(client_sockaddr);
        //accept
        int client_sock=accept(sockid,(struct sockaddr *)&client_sockaddr,&client_socklen);
        //打印所连接客户端ip及端口
        printf("ip=%s,port=%d\n",inet_ntoa(client_sockaddr.sin_addr),ntohs(client_sockaddr.sin_port));
        char receive[100]={0};
        int  r_size=0;
        while(1)
        {
                //获取时间，即服务器接收数据的时间
                time_t timep;
                time(&timep);
                memset(receive,0,sizeof(receive));
                //读取数据，如果没有读到就等待，因为此时read阻塞，如果客户端关闭read立即返回0
                if((r_size=read(client_sock,receive,sizeof(receive)))==0)
                {
                        break;
                }
                fputs(ctime(&timep),stdout);
                fputs(receive,stdout);
                printf("\n");
                //将读取到的数据在发给客户端
                write(client_sock,receive,r_size);
        }
        close(client_sock);
        close(sockid);
        return 0;
}
