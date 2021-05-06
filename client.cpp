#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <stdlib.h>
#include "co_routine.h"
int main(int argc,char** args)
{
    if(argc<2){
        printf("argc<2");
        return 0;
    }
    printf("port=%d\n",atoi(args[1]));
    printf("sdfds\n");
        struct sockaddr_in sock;
        memset(&sock,0,sizeof(sock));
        sock.sin_family=AF_INET;
        sock.sin_port=htons(atoi(args[1]));
        // sock.sin_addr.s_addr=inet_addr("127.0.0.1");//192.144.239.87
        sock.sin_addr.s_addr=inet_addr("192.144.239.87");//192.144.239.87
        //创建一个socket
    printf("socket...\n");
        int sockid=socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);
        socklen_t socklen=sizeof(sock);
        //connect
    printf("connect..\n.");
        connect(sockid,(struct sockaddr *)&sock,socklen);

        char send[100]={0};
        char receive[100]={0};
        while(1)
        {
                printf("发送： ");
                //从标准输入获取一段数据
                fgets(send,sizeof(send),stdin);
                //将这段数据发送给服务器
                write(sockid,send,strlen(send));
                printf("接受： ");
                //读取来自服务器传回来的数据
                read(sockid,receive,sizeof(receive));
                fputs(receive,stdout);
                //将这两个数组清零
                memset(receive,0,strlen(receive));
                memset(send,0,strlen(send));
        }
        close(sockid);

        return 0;
}