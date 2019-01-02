#include "rip.h"
TRtEntry *g_pstRouteEntry = NULL;//Route Table
TRipPkt *ripSendReqPkt = NULL;
TRipPkt *ripSendUpdPkt = NULL;
TRipPkt *ripReceivePkt = NULL;
TRipPkt *ripResponsPkt = NULL;

struct pcLocalIFState{
    unsigned int interCount;
    struct in_addr pcLocalAddr[10];//存储本地接口ip地址
    char pcLocalName[10][IF_NAMESIZE];//存储本地接口的接口名
    struct in_addr pcLocalMask[10];
} ;

struct pcLocalIFState currentState;

void* IFDetect(void* arg);
pthread_mutex_t mutex;
int directConnect(struct in_addr a, struct in_addr b, struct in_addr mask)
{
    return (a.s_addr & mask.s_addr) == (b.s_addr & mask.s_addr);
}


void requestpkt_Encapsulate()
{
    printf("%s\n", "Encapsulate the request package");
    //封装请求包  command =1,version =2,family =0,metric =16
    ripSendReqPkt->ucCommand = RIP_REQUEST;
    ripSendReqPkt->ucVersion = RIP_VERSION;
    ripSendReqPkt->usZero = 0;
    ripSendReqPkt->RipEntries[0].usFamily = 0;
    ripSendReqPkt->RipEntries[0].uiMetric = htonl(RIP_INFINITY);
    ripSendReqPkt->ripEntryCount = 1;
}


/*****************************************************
*Func Name:    rippacket_Receive********************/

int receivefd;

void rippacket_Receive()
{
    struct sockaddr_in local_addr;


    char buff[1500];

    memset(buff, 0, sizeof(buff));
    unsigned int addrLength = sizeof(struct sockaddr_in);
    memset(&local_addr, 0, addrLength);

    while (1)
    {
        //接收rip报文   存储接收源ip地址
        int recvLength = 0;
        printf("Waiting to receive...\n\033[0m");
        if((recvLength= (int) recvfrom(receivefd, buff, sizeof(buff), 0, (struct sockaddr*)&local_addr, &addrLength)) < 0)
        {
            perror("recv error\n");
        }
        printf("Start receive...\033[0m\n");
        printf("    received %d sized ripPacket from %s\n", recvLength, inet_ntoa(local_addr.sin_addr));
        if(recvLength > RIP_MAX_PACKET)
        {
            printf("recv length is too large\n");
            continue;
        }
        //判断command类型，request 或 response
        ripReceivePkt = (TRipPkt *)buff;
        ripReceivePkt->ripEntryCount = (recvLength-4)/20;
        printf("    command is %d, rip version is %d\n",ripReceivePkt->ucCommand, ripReceivePkt->ucVersion);
        //接收到的信息存储到全局变量里，方便request_Handle和response_Handle处理
        if(ripReceivePkt->ucCommand == RIP_RESPONSE)
        {
            response_Handle(local_addr.sin_addr);
        }else if(ripReceivePkt->ucCommand == RIP_REQUEST && ripReceivePkt->RipEntries[0].usFamily == 0 && htonl(ripReceivePkt->RipEntries[0].uiMetric) == RIP_INFINITY)
        {
            request_Handle(local_addr.sin_addr);
        }else
        {
            printf("Neighther RIP_RESPONSE nor RIP_REQUEST\n");
        }
        int x=0;
        if(x)
            break;
    }
}


/*****************************************************
*Func Name:    rippacket_Send  
*Description:  向接收源发送响应报文
*Input:        
*	  1.stSourceIp    ：接收源的ip地址，用于发送目的ip设置
*Output: 
*
*Ret  ：
*
*******************************************************/
void rippacket_Send(struct in_addr stSourceIp, struct in_addr pcLocalAddr)
{
    printf("            Send response packet from %s ", inet_ntoa(pcLocalAddr));
    printf("to %s\n", inet_ntoa(stSourceIp));
    int sendfd;
    struct sockaddr_in local_addr, peer_addr;
    //本地ip设置
    local_addr.sin_family = AF_INET;
    local_addr.sin_addr = pcLocalAddr;
    local_addr.sin_port = htons(RIP_PORT);
    //发送目的ip设置
    peer_addr.sin_family = AF_INET;
    peer_addr.sin_addr = stSourceIp;
    peer_addr.sin_port = htons(RIP_PORT);
    //防止绑定地址冲突，仅供参考
    sendfd = socket(AF_INET, SOCK_DGRAM, 0);
    if(sendfd < 0)
    {
        perror("create socket fail!\n");
        exit(-1);
    }
    //设置地址重用
    int  iReUseddr = 1;
    if (setsockopt(sendfd,SOL_SOCKET ,SO_REUSEADDR,(const char*)&iReUseddr,sizeof(iReUseddr))<0)
    {
        perror("setsockopt\n");
        return ;
    }
    //设置端口重用
    int  iReUsePort = 1;
    if (setsockopt(sendfd,SOL_SOCKET ,SO_REUSEPORT,(const char*)&iReUsePort,sizeof(iReUsePort))<0)
    {
        perror("setsockopt\n");
        return ;
    }
    //把本地地址加入到组播中//←_← what the hell

    //创建并绑定socket

    if (bind(sendfd, (struct sockaddr*)&local_addr, sizeof(local_addr))<0)
    {
        perror("bind error\n");
        exit(-1);
    }
    //发送
    short dataLength = (short)(4 + ripResponsPkt->ripEntryCount * 20);
    if (sendto(sendfd, ripResponsPkt, (size_t)dataLength, 0, (struct sockaddr*)&peer_addr, sizeof(peer_addr))<0)
    {
        perror("send request error\n");
        exit(-1);
    }

    close(sendfd);
}

/*****************************************************
*Func Name:    rippacket_Multicast  
*Description:  组播请求报文
*Input:        
*	  1.pcLocalAddr   ：本地ip地址
*Output: 
*
*Ret  ：
*
*******************************************************/
void rippacket_Multicast(struct in_addr pcLocalAddr, struct RipPacket * ripPacket)
{
    printf("    Multicast packet from %s\n", inet_ntoa(pcLocalAddr));
    int sendfd;
    struct sockaddr_in local_addr, peer_addr;
    //本地ip设置
    local_addr.sin_family = AF_INET;
    local_addr.sin_addr = pcLocalAddr;
    local_addr.sin_port = htons(RIP_PORT);
    //目的ip设置
    peer_addr.sin_family = AF_INET;
    peer_addr.sin_addr.s_addr = inet_addr(RIP_GROUP);
    peer_addr.sin_port = htons(RIP_PORT);

    //创建并绑定socket
    sendfd = socket(AF_INET, SOCK_DGRAM, 0);
    if(sendfd < 0)
    {
        perror("create socket fail!\n");
        return;
    }
    //防止绑定地址冲突，仅供参考
    //设置地址重用
    int  iReUseddr = 1;
    if (setsockopt(sendfd,SOL_SOCKET ,SO_REUSEADDR,(const char*)&iReUseddr,sizeof(iReUseddr))<0)
    {
        perror("reuse addr error\n");
        return;
    }
    //设置端口重用
    int  iReUsePort = 1;
    if (setsockopt(sendfd,SOL_SOCKET ,SO_REUSEPORT,(const char*)&iReUsePort,sizeof(iReUsePort))<0)
    {
        perror("reuse port error\n");
        return;
    }
    if (bind(sendfd, (struct sockaddr*)&local_addr, sizeof(local_addr))<0)
    {
        perror("bind error\n");
        return;
    }

    //把本地地址加入到组播中
    struct ip_mreq mreq;
    mreq.imr_multiaddr.s_addr = inet_addr(RIP_GROUP);
    mreq.imr_interface = pcLocalAddr;
    if (setsockopt(sendfd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq,sizeof (struct ip_mreq)) == -1)
    {
        perror ("join in multicast error\n");
        exit (-1);
    }

    //防止组播回环的参考代码

    //0 禁止回环  1开启回环
    int loop = 0;
    int err = setsockopt(sendfd,IPPROTO_IP, IP_MULTICAST_LOOP,&loop, sizeof(loop));
    if(err < 0)
    {
        perror("setsockopt():IP_MULTICAST_LOOP");
    }
    //

    //发送
    short dataLength = (short)(4 + ripPacket->ripEntryCount * 20);
    if (sendto(sendfd, ripPacket, (size_t)dataLength, 0,
               (struct sockaddr*)&peer_addr, sizeof(peer_addr))<0)
    {
        perror("send request error\n");
        exit(-1);
    }

    close(sendfd);
}

/*****************************************************
*Func Name:    request_Handle  
*Description:  响应request报文
*Input:        
*	  1.stSourceIp   ：接收源的ip地址
*Output: 
*
*Ret  ：
*
*******************************************************/
void request_Handle(struct in_addr stSourceIp)
{

    pthread_mutex_lock(&mutex);
    printf("        I need to response someone\n");
    //处理request报文
    //遵循水平分裂算法
    //回送response报文，command置为RIP_RESPONSE
    TRtEntry *currentEntry = g_pstRouteEntry->pstNext;
    while(currentEntry != NULL)
    {
        ripResponsPkt->ucCommand = RIP_RESPONSE;
        ripResponsPkt->ucVersion = RIP_VERSION;
        ripResponsPkt->usZero = 0;
        ripResponsPkt->ripEntryCount = 0;
        while(currentEntry != NULL && ripResponsPkt->ripEntryCount < RIP_MAX_ENTRY)
        {
            ripResponsPkt->RipEntries[ripResponsPkt->ripEntryCount].stAddr = currentEntry->stIpPrefix;
            ripResponsPkt->RipEntries[ripResponsPkt->ripEntryCount].stNexthop = currentEntry->stNexthop;
            ripResponsPkt->RipEntries[ripResponsPkt->ripEntryCount].uiMetric = currentEntry->uiMetric;
            ripResponsPkt->RipEntries[ripResponsPkt->ripEntryCount].stPrefixLen = currentEntry->uiPrefixLen;
            ripResponsPkt->RipEntries[ripResponsPkt->ripEntryCount].usFamily = htons(2);
            ripResponsPkt->RipEntries[ripResponsPkt->ripEntryCount].usTag = 0;
            ripResponsPkt->ripEntryCount ++;
            currentEntry = currentEntry->pstNext;
        }
        if(ripResponsPkt->ripEntryCount)
        {
            for(int i = 0; i < currentState.interCount ; i ++)
                if(directConnect(currentState.pcLocalAddr[i], stSourceIp, currentState.pcLocalMask[i]))
                {
                    //同一网段
                    rippacket_Send(stSourceIp, currentState.pcLocalAddr[i]);
                }
        }
    }

    pthread_mutex_unlock(&mutex);
}

/*****************************************************
*Func Name:    response_Handle  
*Description:  响应response报文
*Input:        
*	  1.stSourceIp   ：接收源的ip地址
*Output: 
*
*Ret  ：
*
*******************************************************/
void response_Handle(struct in_addr stSourceIp)
{

    pthread_mutex_lock(&mutex);
    printf("[response_Handle]\n");
    struct timeval tv;
    gettimeofday(&tv,NULL);

    int needUpdate = 0;
    for(int i = 0; i < ripReceivePkt->ripEntryCount; i ++)
    {
        int exist = 0;
        uint32_t newMetric = ntohl(ripReceivePkt->RipEntries[i].uiMetric) + 1;
        TRtEntry *currentEntry = g_pstRouteEntry->pstNext;
        while(currentEntry != NULL)
        {
            if(currentEntry->stIpPrefix.s_addr == ripReceivePkt->RipEntries[i].stAddr.s_addr &&
               currentEntry->uiPrefixLen.s_addr==ripReceivePkt->RipEntries[i].stPrefixLen.s_addr)
            {
                //already exists
                exist = 1;
                uint32_t currentMetric = ntohl(currentEntry->uiMetric);

                if(currentEntry->stNexthop.s_addr == stSourceIp.s_addr) {
                    //message from current nexthop
                    if(currentMetric != newMetric)
                    {
                        needUpdate = 1;
                    }

                    currentEntry->uiMetric = htonl(newMetric);
                    currentEntry->isValid = ROUTE_VALID;
                    if(newMetric >= RIP_INFINITY)// not useful
                    {
                        currentEntry->isValid = ROUTE_NOTVALID;
                    }
                    currentEntry->lastUpdataTime = tv.tv_sec;
                }
                else if(currentMetric > newMetric && newMetric<RIP_INFINITY) {
                    //from others : whether to optimize?
                    int optimize=0;
                    if(currentEntry->stNexthop.s_addr != stSourceIp.s_addr)
                    {
                        needUpdate = 1;
                        optimize = 1;
                        printf("\033[1;35m[delete Route]a better route found! delete old one.\033[0m\n");
                        route_SendForward(DelRoute,currentEntry);
                    }


                    currentEntry->uiMetric = htonl(newMetric);
                    currentEntry->stNexthop = stSourceIp;
                    currentEntry->isValid = ROUTE_VALID;
                    //find send port
                    for(int ifc = 0; ifc < currentState.interCount; ifc ++) {
                        if (directConnect(stSourceIp, currentState.pcLocalAddr[ifc], currentState.pcLocalMask[ifc])) {
                            //found
                            strcpy(currentEntry->pcIfname, (const char *) currentState.pcLocalName[ifc]);
                            break;
                        }
                    }
                    if(optimize==1) {
                        route_SendForward(AddRoute,currentEntry);
                    }
                    currentEntry->lastUpdataTime = tv.tv_sec;
                }
                //the rest:sent from other ports and not better than current route
            }
            currentEntry = currentEntry->pstNext;
        }
        if(!exist &&  newMetric< RIP_INFINITY)
        {
            //append a new route entry
            needUpdate = 1;
            currentEntry = (TRtEntry *) malloc(sizeof(TRtEntry));
            currentEntry->stIpPrefix = ripReceivePkt->RipEntries[i].stAddr;
            currentEntry->stNexthop = stSourceIp;
            currentEntry->uiPrefixLen = ripReceivePkt->RipEntries[i].stPrefixLen;
            currentEntry->uiMetric = htonl(newMetric);
            currentEntry->isValid = ROUTE_VALID;
            currentEntry->lastUpdataTime = tv.tv_sec;

            //find send port
            for(int ifc = 0; ifc < currentState.interCount; ifc ++)
                if(directConnect(stSourceIp, currentState.pcLocalAddr[ifc], currentState.pcLocalMask[ifc]))
                {
                    //found
                    printf("[found]%s from %s\n",inet_ntoa(ripReceivePkt->RipEntries[i].stAddr),currentState.pcLocalName[ifc]);
                    currentEntry->pcIfname = (char *) malloc(IF_NAMESIZE);
                    strcpy(currentEntry->pcIfname, (const char *) currentState.pcLocalName[ifc]);
                    break;
                }
            route_SendForward(AddRoute,currentEntry);
            currentEntry->pstNext = g_pstRouteEntry->pstNext;
            g_pstRouteEntry->pstNext = currentEntry;
        }
    }
    //pstRouteEntry the route table is already updated
    TRtEntry *pstRouteEntry = g_pstRouteEntry->pstNext;
    while(pstRouteEntry != NULL)
    {
        printf("\tipPrefix=%16s ", inet_ntoa(pstRouteEntry->stIpPrefix));
        printf("nextHop=%16s ", inet_ntoa(pstRouteEntry->stNexthop));
        printf("prefixLen=%16s metric=%2d\n", inet_ntoa(pstRouteEntry->uiPrefixLen), ntohl(pstRouteEntry->uiMetric));
        pstRouteEntry=pstRouteEntry->pstNext;
    }

    pthread_mutex_unlock(&mutex);

    if(needUpdate)
    {
        printf("Update triggered! need to multicast\n");
        send_update_to_neighbour();
    }

}

/*****************************************************
*Func Name:    route_SendForward  
*Description:  响应response报文
*Input:        
*	  1.uiCmd        ：插入命令
*	  2.pstRtEntry   ：路由信息
*Output: 
*
*Ret  ：
*
*******************************************************/
void route_SendForward(unsigned int uiCmd, TRtEntry *pstRtEntry)
{
    //建立tcp短连接，发送插入、删除路由表项信息到转发引擎
    printf("responseHandle:route_SendForward...\n");
    if(uiCmd == AddRoute)
    {
        printf("    Add route!\n");
    }else
    {
        printf("    Delete route!\n");
    }
    printf("\tipPrefix=%16s\n",inet_ntoa(pstRtEntry->stIpPrefix));
    printf("\tnextHop=%16s\n", inet_ntoa(pstRtEntry->stNexthop));
    printf("\tprefixLen=%16s\n", inet_ntoa(pstRtEntry->uiPrefixLen));
    printf("\tpcIfname=%16s\n", pstRtEntry->pcIfname);
    printf("\tpcIfIndex=%d\n", if_nametoindex(pstRtEntry->pcIfname));
    printf("\tMetric=%d\n", ntohl(pstRtEntry->uiMetric));

    int sendfd;
    int sendlen=0;
    int tcpcount=0;
    struct sockaddr_in dst_addr;
    char buf[sizeof(struct selfroute)];

    memset(buf, 0, sizeof(buf));
    struct selfroute *selfrt;
    selfrt = (struct selfroute *)&buf;
    selfrt->selfprefixlen = 24;
    selfrt->selfprefix	= pstRtEntry->stIpPrefix;
    selfrt->selfifindex	= if_nametoindex(pstRtEntry->pcIfname);
    selfrt->selfnexthop	= pstRtEntry->stNexthop;
    selfrt->cmdnum        = uiCmd;

    memset(&dst_addr, 0, sizeof(struct sockaddr_in));
    dst_addr.sin_family = AF_INET;
    dst_addr.sin_port   = htons(800);
    dst_addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    if ((sendfd = socket(AF_INET, SOCK_STREAM,0 )) == -1)
    {
        printf("self route sendfd socket error!!\n");
        exit(-1);
    }

    while(tcpcount <6)
    {
        if(connect(sendfd,(struct sockaddr*)&dst_addr,sizeof(dst_addr))<0)
        {
            tcpcount++;
        }
        else {
            break;
        }
        //sleep(1);
    }
    if(tcpcount<6)
    {
        sendlen = send(sendfd, buf, sizeof(buf), 0); //struct sockaddr *)&dst_addr, sizeof(struct sockaddr_in));
        if (sendlen <= 0)
        {
            printf("self route sendto() error!!!\n");
            exit(-1);
        }
        if(sendlen >0)
        {
            printf("send ok!!!\n");
        } else{
            sleep(1);
        }
        close(sendfd);
    } else{
        printf("\033[1;36m timeout! \033[0m\n");
    }
}

void rippacket_Update(struct in_addr pcLocalAddr)
{
    //遍历rip路由表，封装更新报文
    TRtEntry *entry = g_pstRouteEntry->pstNext;
    struct timeval tv;
    gettimeofday(&tv,NULL);
    while(entry != NULL)
    {
        ripSendUpdPkt->ucCommand = RIP_RESPONSE;
        ripSendUpdPkt->ucVersion = RIP_VERSION;
        ripSendUpdPkt->usZero = 0;
        ripSendUpdPkt->ripEntryCount = 0;
        while(entry != NULL && ripSendUpdPkt->ripEntryCount < RIP_MAX_ENTRY)
        {
            if(!directConnect(entry->stIpPrefix, pcLocalAddr, entry->uiPrefixLen))
            {
                if(tv.tv_sec - entry->lastUpdataTime > ROUTE_MAX_INTERVAL && entry->uiMetric != htonl(1))
                {
                    printf("%ld %ld\n", tv.tv_sec, entry->lastUpdataTime);
                    entry->isValid = ROUTE_NOTVALID;
                }
                ripSendUpdPkt->RipEntries[ripSendUpdPkt->ripEntryCount].stAddr = entry->stIpPrefix;
                ripSendUpdPkt->RipEntries[ripSendUpdPkt->ripEntryCount].stNexthop = entry->stNexthop;
                if(entry->isValid==ROUTE_NOTVALID||directConnect(pcLocalAddr, entry->stNexthop, entry->uiPrefixLen)){
                    //poison reverse
                    ripSendUpdPkt->RipEntries[ripSendUpdPkt->ripEntryCount].uiMetric=htonl(RIP_INFINITY);
                } else{
                    ripSendUpdPkt->RipEntries[ripSendUpdPkt->ripEntryCount].uiMetric=entry->uiMetric;
                }
                ripSendUpdPkt->RipEntries[ripSendUpdPkt->ripEntryCount].stPrefixLen = entry->uiPrefixLen;
                ripSendUpdPkt->RipEntries[ripSendUpdPkt->ripEntryCount].usFamily = htons(2);
                ripSendUpdPkt->RipEntries[ripSendUpdPkt->ripEntryCount].usTag = 0;
                ripSendUpdPkt->ripEntryCount ++;
            }
            entry = entry->pstNext;
        }
        if(ripSendUpdPkt->ripEntryCount>0)
        {
            rippacket_Multicast(pcLocalAddr, ripSendUpdPkt);
        }
    }
    //注意水平分裂算法
}
void routeTableDelete()//Delete not valid item
{
    TRtEntry *entry = g_pstRouteEntry;
    TRtEntry *tmp ;
    struct timeval tv;
    gettimeofday(&tv,NULL);
    while(entry->pstNext)
    {
        if(entry->pstNext->isValid==ROUTE_NOTVALID || ntohl(entry->pstNext->uiMetric)>RIP_INFINITY){
            printf("\033[1;35m[delete Route]Delete not valid route.\033[0m\n");
            route_SendForward(DelRoute,entry->pstNext);
            tmp=entry->pstNext;
            entry->pstNext=entry->pstNext->pstNext;
            free(tmp->pcIfname);
            free(tmp);
        } else if(tv.tv_sec-entry->pstNext->lastUpdataTime>3*UPDATE_INTERVAL
        && entry->pstNext->stNexthop.s_addr!=INADDR_ANY) {
            printf("\033[1;35m[TIMEOUT DELETE] %s from %s(M=%u)\033[0m\n",
                   inet_ntoa(entry->pstNext->stIpPrefix),
                   inet_ntoa(entry->pstNext->stNexthop),
                   entry->pstNext->uiMetric);
            printf("\033[1;35m[delete Route]Delete TimeOut route.\033[0m\n");
            route_SendForward(DelRoute,entry->pstNext);
            tmp=entry->pstNext;
            entry->pstNext=entry->pstNext->pstNext;
            free(tmp->pcIfname);
            free(tmp);
        } else{
            entry=entry->pstNext;
        }
    }
}

void send_update_to_neighbour()
{
    pthread_mutex_lock(&mutex);
    printf("[update to neighbour]\n");
    for(int i = 0; i < currentState.interCount ; i++)
    {
        rippacket_Update(currentState.pcLocalAddr[i]);
    }
    routeTableDelete();

    //multicast done
    printf("[current rip table]\n");
    TRtEntry *entry = g_pstRouteEntry->pstNext;
    while(entry != NULL)
    {
        printf(">>\tipPrefix=%16s\n", inet_ntoa(entry->stIpPrefix));
        printf("\tnextHop=%16s\n", inet_ntoa(entry->stNexthop));
        printf("\tprefixLen=%16s\n", inet_ntoa(entry->uiPrefixLen));
        printf("\tmetric=%2d\n", ntohl(entry->uiMetric));
        printf("\tt=%ld\n", entry->lastUpdataTime);
        printf("\tname=%s\n", entry->pcIfname);
        entry=entry->pstNext;
    }
    pthread_mutex_unlock(&mutex);
}
void* update_thread(void * arg)
{
    while(1)
    {
        sleep(UPDATE_INTERVAL);
        printf("\033[1;32mIt's time to update...\033[0m\n");
        send_update_to_neighbour();
        printf("\033[1;32mupdate done...\033[0m\n");
    }
}

void* send_Request()
{
    sleep(1);
    requestpkt_Encapsulate();
    for (int i = 0; i < currentState.interCount; i ++)
    {
        rippacket_Multicast(currentState.pcLocalAddr[i], ripSendReqPkt);
    }
}
void ripdaemon_Start()
{
    //创建更新线程，30s更新一次,向组播地址更新Update包
    pthread_t tid;
    int ret = pthread_create(&tid, NULL, update_thread, NULL);
    if (ret != 0)
    {
        perror("new thread error!\n");
        exit(-1);
    }

    //封装请求报文，并组播
    pthread_t tid2;
    int ret2 = pthread_create(&tid2, NULL, send_Request, NULL);
    if(ret2 != 0)
    {
        perror("new thread error!\n");
        exit(-1);
    }
    //接收rip报文
    rippacket_Receive();
}

void routentry_Insert()
{
    struct sockaddr_in local_addr;
    pthread_mutex_lock(&mutex);


    //将本地接口表添加到rip路由表里
    printf("%s\n","Add interfaces into Rip Route Table");
    struct timeval tv;
    gettimeofday(&tv,NULL);
    TRtEntry *entry ;
    for(int i = 0; i < currentState.interCount;i ++)
    {
        entry = (TRtEntry *) malloc(sizeof(TRtEntry));
        entry->stIpPrefix.s_addr = currentState.pcLocalAddr[i].s_addr & currentState.pcLocalMask->s_addr;
        entry->stNexthop.s_addr = inet_addr("0.0.0.0");
        entry->uiPrefixLen = currentState.pcLocalMask[i];
        entry->uiMetric = htonl(1);
        entry->pstNext = NULL;
        entry->lastUpdataTime = tv.tv_sec;
        entry->isValid = ROUTE_VALID;
        entry->pcIfname = (char *) malloc(sizeof(IF_NAMESIZE));
        strcpy(entry->pcIfname, (const char *) currentState.pcLocalName[i]);
        route_SendForward(AddRoute,entry);

        entry->pstNext=g_pstRouteEntry->pstNext;
        g_pstRouteEntry->pstNext=entry;
    }

    pthread_mutex_unlock(&mutex);

    //接收ip设置

    memset(&local_addr, 0, sizeof(struct sockaddr_in));
    local_addr.sin_family = AF_INET;
    local_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    local_addr.sin_port = htons(RIP_PORT);  //注意网络序转换

    //创建并绑定socket

    receivefd = socket(AF_INET, SOCK_DGRAM, 0);
    if(receivefd < 0)
    {
        perror("create socket fail!\n");
        exit(-1);
    }

    //防止绑定地址冲突，仅供参考
    //设置地址重用
    int  iReUseddr = 1;
    if (setsockopt(receivefd,SOL_SOCKET ,SO_REUSEADDR,(const char*)&iReUseddr,sizeof(iReUseddr))<0)
    {
        perror("reuse addr error\n");
        return ;
    }
    //设置端口重用
    int  iReUsePort = 1;
    if (setsockopt(receivefd,SOL_SOCKET ,SO_REUSEPORT,(const char*)&iReUsePort,sizeof(iReUsePort))<0)
    {
        perror("reuse port error\n");
        return ;
    }

    if(bind(receivefd,
            (struct sockaddr*)&local_addr,
            sizeof(struct sockaddr_in))<0)
    {
        perror("bind error\n");
        exit(-1);
    }

    //把本地地址加入到组播中
    for(int i = 0; i < currentState.interCount; i ++)
    {
        struct ip_mreq mreq;
        mreq.imr_multiaddr.s_addr=inet_addr(RIP_GROUP);
        mreq.imr_interface=currentState.pcLocalAddr[i];
        /* 把本机加入组播地址，即本机网卡作为组播成员，只有加入组才能收到组播消息 */
        if (setsockopt(receivefd,
                       IPPROTO_IP,
                       IP_ADD_MEMBERSHIP,
                       &mreq,
                       sizeof (struct ip_mreq)) == -1)
        {
            perror ("join in multicast error\n");
            continue;
        }
    }

    //防止组播回环的参考代码

    //0 禁止回环  1开启回环
    int loop = 0;
    int err = setsockopt(receivefd,IPPROTO_IP, IP_MULTICAST_LOOP,&loop, sizeof(loop));
    if(err < 0)
    {
        perror("setsockopt():IP_MULTICAST_LOOP");
    }
}

void localinterf_GetInfo()
{
    struct ifaddrs *pstIpAddrStruct = NULL;
    struct ifaddrs *pstIpAddrStCur = NULL;
    void *pAddrPtr = NULL;
    const char *pcLo = "127.0.0.1";

    getifaddrs(&pstIpAddrStruct); //linux系统函数
    pstIpAddrStCur = pstIpAddrStruct;
    printf("%s\n", "Get local interfaces");
    int i = 0;
    while (pstIpAddrStruct != NULL)
    {
        if (pstIpAddrStruct->ifa_addr->sa_family == AF_INET)
        {

            pAddrPtr = &((struct sockaddr_in *) pstIpAddrStruct->ifa_addr)->sin_addr;
            char cAddrBuf[INET_ADDRSTRLEN];
            memset(&cAddrBuf, 0, sizeof(INET_ADDRSTRLEN));
            inet_ntop(AF_INET, pAddrPtr, cAddrBuf, INET_ADDRSTRLEN);
            if (strcmp((const char *) &cAddrBuf, pcLo) != 0)
            {
                currentState.pcLocalAddr[i] = ((struct sockaddr_in *) pstIpAddrStruct->ifa_addr)->sin_addr;
                currentState.pcLocalMask[i] = ((struct sockaddr_in *)(pstIpAddrStruct->ifa_netmask))->sin_addr;
                strcpy(currentState.pcLocalName[i], (const char *) pstIpAddrStruct->ifa_name);
                printf("    %d: addr = %16s name = %s ", i, inet_ntoa(currentState.pcLocalAddr[i]), currentState.pcLocalName[i]);
                printf("mask = %s\n",inet_ntoa(currentState.pcLocalMask[i]));

                char pingCmd[30];
                struct in_addr tem;
                tem.s_addr=currentState.pcLocalAddr[i].s_addr&currentState.pcLocalMask[i].s_addr;
                sprintf(pingCmd,"ping %s -b -c 1 -w 1",inet_ntoa(tem));
                printf("\033[1;34m%s\033[0m\n",pingCmd);
                system(pingCmd);
                system("arp -a");

                i++;
                currentState.interCount++;
            }
        }
        pstIpAddrStruct = pstIpAddrStruct->ifa_next;
    }
    freeifaddrs(pstIpAddrStCur);//linux系统函数
}
void* IFDetect(void* arg){
    struct pcLocalIFState newState;
    const char *pcLo = "127.0.0.1";
    struct ifaddrs *pstIpAddrStruct = NULL;
    struct ifaddrs *pstIpAddrStCur = NULL;
    void *pAddrPtr = NULL;

    while(1){
        getifaddrs(&pstIpAddrStruct); //linux系统函数
        pstIpAddrStCur = pstIpAddrStruct;
        newState.interCount = 0;
        while (pstIpAddrStruct != NULL)
        {
            if (pstIpAddrStruct->ifa_addr->sa_family == AF_INET)
            {
                pAddrPtr = &((struct sockaddr_in *) pstIpAddrStruct->ifa_addr)->sin_addr;
                char cAddrBuf[INET_ADDRSTRLEN];
                memset(&cAddrBuf, 0, sizeof(INET_ADDRSTRLEN));
                inet_ntop(AF_INET, pAddrPtr, cAddrBuf, INET_ADDRSTRLEN);
                if (strcmp((const char *) &cAddrBuf, pcLo) != 0)
                {
                    newState.pcLocalAddr[newState.interCount] = ((struct sockaddr_in *) pstIpAddrStruct->ifa_addr)->sin_addr;
                    newState.pcLocalMask[newState.interCount] = ((struct sockaddr_in *)(pstIpAddrStruct->ifa_netmask))->sin_addr;
                    strcpy(newState.pcLocalName[newState.interCount], (const char *) pstIpAddrStruct->ifa_name);



                    newState.interCount++;
                }
            }
            pstIpAddrStruct = pstIpAddrStruct->ifa_next;
        }
        freeifaddrs(pstIpAddrStCur);//linux系统函数
        //detect change
        for(int i=0;i<newState.interCount;i++){
            //new connect
            int found=-1;
            for(int j=0;j<currentState.interCount;j++){
                if(currentState.pcLocalAddr[j].s_addr==newState.pcLocalAddr[i].s_addr){
                    found=j;
                    break;
                }
            }
            if(found==-1){
                struct timeval tv;
                gettimeofday(&tv,NULL);

                pthread_mutex_lock(&mutex);
                printf("\033[1;32m[Detect New IF]:\n");
                printf("    %d: addr = %16s name = %s ", i, inet_ntoa(newState.pcLocalAddr[i]), newState.pcLocalName[i]);
                printf("mask = %s\n",inet_ntoa(newState.pcLocalMask[i]));
                printf("\033[0m\n");

                char pingCmd[30];
                struct in_addr tem;
                tem.s_addr=newState.pcLocalAddr[i].s_addr&newState.pcLocalMask[i].s_addr;
                sprintf(pingCmd,"ping %s -b -c 2 -W 2",inet_ntoa(tem));
                printf("\033[1;34m%s\033[0m\n",pingCmd);
                system(pingCmd);
                sleep(1);
                system(pingCmd);
                system("arp -a");


                TRtEntry* entry = (TRtEntry *) malloc(sizeof(TRtEntry));
                entry->stIpPrefix.s_addr = newState.pcLocalAddr[i].s_addr & newState.pcLocalMask[i].s_addr;
                entry->stNexthop.s_addr = inet_addr("0.0.0.0");
                entry->uiPrefixLen = newState.pcLocalMask[i];
                entry->uiMetric = htonl(1);
                entry->pstNext = NULL;
                entry->lastUpdataTime = tv.tv_sec;
                entry->isValid = ROUTE_VALID;
                entry->pcIfname = (char *) malloc(sizeof(IF_NAMESIZE));
                strcpy(entry->pcIfname, (const char *) newState.pcLocalName[i]);


                //查找是否完全相同长度掩码包含在该子网内的表项

                TRtEntry* TmpEntry=g_pstRouteEntry;
                while(TmpEntry->pstNext){
                    if(TmpEntry->pstNext->uiPrefixLen.s_addr==entry->uiPrefixLen.s_addr
                    && TmpEntry->pstNext->stIpPrefix.s_addr==entry->stIpPrefix.s_addr){
                        printf("\033[1;35m[Detect NEW IF]Delete Old Route\033[0m\n");
                        route_SendForward(DelRoute,TmpEntry->pstNext);

                        TRtEntry* tmp=TmpEntry->pstNext;
                        TmpEntry->pstNext=tmp->pstNext;
                        free(tmp->pcIfname);
                        free(tmp);
                    } else{
                        TmpEntry=TmpEntry->pstNext;
                    }
                }

                //通知转发引擎
                route_SendForward(AddRoute,entry);
                //路由表更新
                entry->pstNext=g_pstRouteEntry->pstNext;
                g_pstRouteEntry->pstNext=entry;

                currentState.pcLocalAddr[currentState.interCount].s_addr= newState.pcLocalAddr[i].s_addr;
                currentState.pcLocalMask[currentState.interCount].s_addr= newState.pcLocalMask[i].s_addr;
                strcpy(currentState.pcLocalName[currentState.interCount],newState.pcLocalName[i]);

                struct ip_mreq mreq;
                mreq.imr_multiaddr.s_addr=inet_addr(RIP_GROUP);
                mreq.imr_interface=currentState.pcLocalAddr[currentState.interCount];
                /* 把本机加入组播地址，即本机网卡作为组播成员，只有加入组才能收到组播消息 */
                if (setsockopt(receivefd,
                               IPPROTO_IP,
                               IP_ADD_MEMBERSHIP,
                               &mreq,
                               sizeof (struct ip_mreq)) == -1)
                {
                    perror ("join in multicast error\n");
                }


                currentState.interCount++;

                pthread_mutex_unlock(&mutex);
                printf("\033[1;32m[Detect NEW IF DONE]\033[0m\n");
            }
        }
        //detect delete
        for(int i=0;i<currentState.interCount;i++) {
            //old connect
            int found = -1;
            for (int j = 0; j < newState.interCount; j++) {
                if (newState.pcLocalAddr[j].s_addr == currentState.pcLocalAddr[i].s_addr) {
                    found = j;
                    break;
                }
            }
            if (found == -1) {
                struct timeval tv;
                gettimeofday(&tv, NULL);

                pthread_mutex_lock(&mutex);
                printf("\033[1;32m[Detect IF Removed]:\n");
                printf("    %d: addr = %16s name = %s ", i, inet_ntoa(currentState.pcLocalAddr[i]), currentState.pcLocalName[i]);
                printf("mask = %s\n", inet_ntoa(currentState.pcLocalMask[i]));
                printf("\033[0m\n");
                system("arp -a");


                //delete entry
                TRtEntry *entry = g_pstRouteEntry;

                while (entry->pstNext != NULL) {
                    if (entry->pstNext->stNexthop.s_addr == INADDR_ANY) {
                        if ((strcmp(entry->pstNext->pcIfname, currentState.pcLocalName[i])==0)) {
                            //delete
                            printf("\033[1;32m[delete route]%s\033[0m\n",inet_ntoa(entry->pstNext->stIpPrefix));
                            TRtEntry *tmp = entry->pstNext;
                            entry->pstNext = tmp->pstNext;

                            printf("\033[1;33m[delete Route]Delete shutdown device!\033[0m\n");
                            route_SendForward(DelRoute, tmp);

                            free(tmp->pcIfname);
                            free(tmp);
                        } else {
                            entry = entry->pstNext;
                        }
                    } else{
                        entry = entry->pstNext;
                    }
                }


                //delete currentState entry
                currentState.interCount--;
                currentState.pcLocalAddr[i].s_addr = currentState.pcLocalAddr[currentState.interCount].s_addr;
                currentState.pcLocalMask[i].s_addr = currentState.pcLocalMask[currentState.interCount].s_addr;
                strcpy(currentState.pcLocalName[i], currentState.pcLocalName[currentState.interCount]);

                pthread_mutex_unlock(&mutex);
                printf("\033[1;32m[Detect IF Removed DONE]\033[0m\n");

            }
        }

        int j=0;
        if(j==1) {
            break;
        }
    }
}


int main(int argc, char *argv[])
{
    pthread_mutex_init(&mutex,NULL);
    g_pstRouteEntry = (TRtEntry *) malloc(sizeof(TRtEntry));
    ripSendReqPkt = (struct RipPacket *) malloc(sizeof(struct RipPacket));
    ripSendUpdPkt = (struct RipPacket *) malloc(sizeof(struct RipPacket));
    ripResponsPkt = (struct RipPacket *) malloc(sizeof(struct RipPacket));
    localinterf_GetInfo();
    routentry_Insert();
    pthread_t tid;
    int ret = pthread_create(&tid, NULL, IFDetect, NULL);
    if (ret != 0)
    {
        perror("new thread error!\n");
        exit(-1);
    }
    ripdaemon_Start();
    return 0;
}

