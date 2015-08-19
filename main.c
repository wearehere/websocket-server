/*
 * Copyright (c) 2014 Putilov Andrey
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 *
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <pthread.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <limits.h>

#include "list/list.h"
#include "websocket.h"

//#define PORT 8088
#define BUF_LEN 0xFFFF
#define PACKET_DUMP
#define TASK_FLAG_RUNNING 1
#define TASK_FLAG_IDLE 0

typedef struct{
    char* pauthscript;
    char* pcgiscript;
    int listenport;
}RunOption;


RunOption appoption;

typedef struct{
    int sockfd;
    int runflag;
    int authed;
    char* prunurl;
    enum wsState state;
    pthread_mutex_t sendMsgMutex;
    pthread_t pt;
}ClientTaskInfo;

//void clientWorker(int clientSocket, char* pmsgbuf);
void clientWorker(ClientTaskInfo* pinfo);

void error(const char *msg)
{
    perror(msg);
    exit(EXIT_FAILURE);
}

#define LOG(str) \
    printf("%s:%d:%s\n",__FUNCTION__, __LINE__, str);
    
#define COPYSTR(dest, src) \
    {\
        if(dest != NULL) free(dest); \
        dest = (char*)malloc(strlen(src)+1); \
        strcpy(dest, src); \
    }
    
#define FREE_MEM(ptr) \
    {if(ptr) {free(ptr); ptr=NULL;}}    
    
void LOG_HEX(char* pbuf, int len)
{
    int i = 0;
    for(i=0; i< len; i++){
        printf("%02X ", (unsigned char)pbuf[i]);
    }
    printf("\n");
}    

//---------------TASK relate-----------------

list_t *ptasklist = NULL;
pthread_mutex_t taskListMutex;

void *taskThreadFunc(void* param){
    LOG("taskThreadFunc")
    pthread_detach(pthread_self());
    ClientTaskInfo *pclitask = (ClientTaskInfo *)param;
    //clientWorker(pclitask->sockfd, pclitask->msgbuf);
    clientWorker(pclitask);
    pclitask->runflag = TASK_FLAG_IDLE;
    LOG("taskThreadFunc exit")
}

void runClientThread(int fd){
    LOG("runClientThread")
    ClientTaskInfo *pclitask =  (ClientTaskInfo *)(malloc(sizeof(ClientTaskInfo)));
    memset(pclitask, 0x00, sizeof(ClientTaskInfo));
    pclitask->sockfd = fd;
    pclitask->runflag = 1;
	pclitask->state = WS_STATE_OPENING;
	pthread_mutex_init(&(pclitask->sendMsgMutex),NULL);
    pthread_create(&pclitask->pt, NULL, taskThreadFunc, pclitask);
    list_node_t *pnewnode = list_node_new(pclitask);
	pthread_mutex_lock(&taskListMutex);
    list_rpush(ptasklist, pnewnode);
	pthread_mutex_unlock(&taskListMutex);
}

void checkDeadThread(){
	LOG("checkDeadThread")
	ClientTaskInfo* ptask = NULL;
    list_node_t *node;	
	pthread_mutex_lock(&taskListMutex);
    list_iterator_t *it = list_iterator_new(ptasklist, LIST_HEAD);
    while ((node = list_iterator_next(it))) {
        ptask = (ClientTaskInfo*)(node->val);
        if(ptask->runflag == TASK_FLAG_IDLE){
            LOG("checkDeadThread:remove one Dead thread")
            list_remove(ptasklist, node);
            FREE_MEM(ptask->prunurl)
	        pthread_mutex_destroy(&ptask->sendMsgMutex);
            free(ptask);
        }
    }
    list_iterator_destroy(it);	
	pthread_mutex_unlock(&taskListMutex);
}


char* runShellCommand(char* cmdline){
    FILE* fp = NULL;
    char* presp = NULL;
    char readbuf[256];
    int maxlen = 10240;
    int curlen = 0;
    int status = 0;
    LOG("runShellCommand")
    LOG(cmdline)
    fp = popen(cmdline, "re");
    if(!fp)
        return NULL;
    presp = (char*)malloc(maxlen);
    memset(presp, 0x00, maxlen);
    while(!feof(fp)){
        memset(readbuf, 0x00, sizeof(readbuf));
        fgets(readbuf, sizeof(readbuf), fp);
        LOG(readbuf)
        strcat(presp, readbuf);
        curlen = strlen(presp);
    }
    waitpid(-1, &status, 0);
    pclose(fp);
    return presp;    
}
/*
    max return value is 10240
*/
char* runScript(char* pscript, char* param){
    LOG("runScript")
    char cmdbuf[10240];
    
    if(pscript != NULL){
        memset(cmdbuf, 0x00, sizeof(cmdbuf));
        sprintf(cmdbuf, "%s \'%s\'", pscript, param);
        LOG(cmdbuf)
        return runShellCommand(cmdbuf);
    }
    return NULL;
}

int safeSend(int clientSocket, const uint8_t *buffer, size_t bufferSize)
{
    #ifdef PACKET_DUMP
    printf("out packet:\n");
    //fwrite(buffer, 1, bufferSize, stdout);
    //printf("\n");
    LOG_HEX((char*)buffer, bufferSize);
    #endif
    ssize_t written = send(clientSocket, buffer, bufferSize, 0);
    if (written == -1) {
        close(clientSocket);
        perror("send failed");
        return EXIT_FAILURE;
    }
    if (written != bufferSize) {
        close(clientSocket);
        perror("written not all bytes");
        return EXIT_FAILURE;
    }
    
    return EXIT_SUCCESS;
}



int openingClientState(char* inmsg, int inmsglen,
        char* outmsg, int* outmsglen, enum wsState * state, char** purl){
    struct handshake hs;
    enum wsFrameType frameType = WS_INCOMPLETE_FRAME;
    nullHandshake(&hs);
    
    frameType = wsParseHandshake(inmsg, inmsglen, &hs);
    if(frameType == WS_OPENING_FRAME){
        fprintf(stdout, "[openingClientState hs.resource]:%s\n", hs.resource);
        COPYSTR((*purl), hs.resource)
        wsGetHandshakeAnswer(&hs, outmsg, (size_t*)outmsglen);
        freeHandshake(&hs);
        *state = WS_STATE_NORMAL;
        return 0;
    }
    
    assert(frameType != WS_INCOMPLETE_FRAME);
    
    if(frameType == WS_ERROR_FRAME)
        printf("[openingClientState]error in incoming frame\n");
    else
        printf("[openingClientState]error frametype %02X\n", frameType);
        
    (*outmsglen) = sprintf((char *)outmsg,
                        "HTTP/1.1 400 Bad Request\r\n"
                        "%s%s\r\n\r\n",
                        versionField,
                        version);        
    return -1;
}

int normalClientState(char* inmsg, int inmsglen,
        char* outmsg, int* outmsglen, enum wsState * state){

    uint8_t *data = NULL;
    size_t dataSize = 0;
    enum wsFrameType frameType = WS_INCOMPLETE_FRAME;
    frameType = wsParseInputFrame(inmsg, inmsglen, &data, &dataSize);

    assert(frameType != WS_INCOMPLETE_FRAME);
    if (frameType == WS_TEXT_FRAME) {
        uint8_t *recievedString = NULL;
        recievedString = malloc(dataSize+1);
        assert(recievedString);
        memcpy(recievedString, data, dataSize);
        recievedString[ dataSize ] = 0;
        
        fprintf(stdout, "RECV:%s\n", recievedString);
        if(appoption.pcgiscript == NULL){
            fprintf(stdout, "SCRIPT NULL\n");
            *outmsglen = 0;
        }else{
            char* presp = runScript(appoption.pcgiscript, recievedString);
            if(presp != NULL){
                fprintf(stdout, "SCRIPT RESP:%s\n", presp);
                wsMakeFrame(presp, strlen(presp), outmsg, (size_t*)outmsglen, WS_TEXT_FRAME);
                //wsMakeFrame(recievedString, dataSize, gBuffer, &frameSize, WS_TEXT_FRAME);
                free(presp);
            }else{
                *outmsglen = 0;
            }
        }
        free(recievedString);
        return 0;
    }else if(frameType == WS_CLOSING_FRAME){
        fprintf(stdout, "[normalClientState]WS_CLOSING_FRAME\n");
        wsMakeFrame(NULL, 0, outmsg, (size_t*)outmsglen, WS_CLOSING_FRAME);
        return -1;
    }else{
        if(frameType == WS_ERROR_FRAME)
            printf("[normalClientState]error in incoming frame\n");
        else
            printf("[normalClientState]unsupport frame type %02X\n", frameType);
        wsMakeFrame(NULL, 0, outmsg, (size_t*)outmsglen, WS_CLOSING_FRAME);
        *state = WS_STATE_CLOSING;
        return 0; 
    }    
}

int closingClientState(char* inmsg, int inmsglen,
        char* outmsg, int* outmsglen, enum wsState * state){
    uint8_t *data = NULL;
    size_t dataSize = 0;
            
    enum wsFrameType frameType = WS_INCOMPLETE_FRAME;
    assert(frameType != WS_INCOMPLETE_FRAME);
    fprintf(stdout, "[closingClientState]entry\n");
    frameType = wsParseInputFrame(inmsg, inmsglen, &data, &dataSize);
    
    *outmsglen = 0;
    if(frameType == WS_CLOSING_FRAME){
        fprintf(stdout, "[closingClientState]WS_CLOSING_FRAME\n");
        return -1;
    }else{
        fprintf(stdout, "[closingClientState]ignore other message type\n");
        return 0;
    }
}        

int checkCompleteMessage(int state, char* pmsg, int length){
    enum wsFrameType frameType = WS_INCOMPLETE_FRAME;
    char* pos = NULL;
    if(state == WS_STATE_OPENING){
        pos = strstr(pmsg, "\r\n\r\n");     
        if(pos == NULL)
            return -1;
        return pos -pmsg + 4;
    }else{
        uint8_t payloadFieldExtraBytes = 0;
        size_t payloadLength = getPayloadLength(pmsg, length,
                                                &payloadFieldExtraBytes, &frameType);
        if(payloadLength + 6 + payloadFieldExtraBytes > length)
            return -1;
        return payloadLength + 6 + payloadFieldExtraBytes;
    }
}

void clientWorker(ClientTaskInfo* pinfo){
    
    int clientSocket = pinfo->sockfd;
    size_t readedLength = 0;
    size_t frameSize = BUF_LEN;
    enum wsState *pstate = &pinfo->state;
    int ret = 0;
    int pos = 0;
    int usedbytes = 0;
    
    char* pinmsgbuf = (char*)malloc(BUF_LEN);
    char* poutmsg = (char*)malloc(BUF_LEN);
    int outmsglen = 0;
    int inmsglen = 0;
    
    memset(pinmsgbuf, 0x00, sizeof(BUF_LEN));
    memset(poutmsg, 0x00, sizeof(BUF_LEN));

    while(1){
        
        if(BUF_LEN == readedLength){
            printf("[clientWorker]buffer too small");
            ret = -1;
            break;
        }
        
        ssize_t readed = recv(clientSocket, pinmsgbuf+readedLength, BUF_LEN-readedLength, 0);
        if (readed <= 0) {
            close(clientSocket);
            fprintf(stdout, "[clientWorker]readed <= 0\n");
            perror("recv failed\n");
            return;
        }
        #ifdef PACKET_DUMP
        printf("in packet:%d\n", (int)readed);
        //fwrite(pmsgbuf, 1, readed, stdout);
        LOG_HEX(pinmsgbuf+readedLength, readed);
        //printf("\n");
        #endif
        readedLength+= readed;
        assert(readedLength <= BUF_LEN);
        usedbytes = 0;

        while(1) {
            int completemsglen = checkCompleteMessage(*pstate, pinmsgbuf + usedbytes, readedLength - usedbytes);
            if(completemsglen == -1){
                ret = 0;
                if(usedbytes > 0){
                    memcpy(pinmsgbuf, pinmsgbuf + usedbytes, readedLength - usedbytes);
                    readedLength -= usedbytes;         
                    usedbytes = 0;         
                }
                break;
            }
            
            outmsglen = BUF_LEN;
            if(*pstate == WS_STATE_OPENING){
                ret = openingClientState(pinmsgbuf + usedbytes, completemsglen, poutmsg, &outmsglen, pstate, &(pinfo->prunurl));
            }else if(*pstate == WS_STATE_NORMAL){
                ret = normalClientState(pinmsgbuf + usedbytes, completemsglen, poutmsg, &outmsglen, pstate);
            }else if(*pstate == WS_STATE_CLOSING){
                ret = closingClientState(pinmsgbuf + usedbytes, completemsglen, poutmsg, &outmsglen, pstate);
            }else{
                fprintf(stdout, "[clientWorker]unknown state\n");
                ret = -1;
            }            
            
            usedbytes += completemsglen;
            if(outmsglen > 0){
				int ret = EXIT_FAILURE;

				pthread_mutex_lock(&pinfo->sendMsgMutex);
				ret = safeSend(clientSocket, poutmsg, outmsglen);
				pthread_mutex_unlock(&pinfo->sendMsgMutex);
                if (ret == EXIT_FAILURE){
                    fprintf(stdout, "[clientWorker]safeSend error\n");      
                    ret = -1;                  
                    break;
                }                
            }
            memset(poutmsg, 0x00, sizeof(BUF_LEN)); outmsglen = 0;
            if(ret == -1){
                break;
            }
        }
        
        if(ret == -1){
            break;            
        }
    }
    fprintf(stdout, "[clientWorker]exit\n");
    close(clientSocket);
    free(pinmsgbuf);
    free(poutmsg);
}

void broadcastMsg(char *msg, int size)
{
    ClientTaskInfo* ptask = NULL;
    list_node_t *node;
    list_iterator_t *it;
	char *pmsgbuf = (char *)malloc(BUF_LEN);
	size_t frameSize = BUF_LEN;
	wsMakeFrame(msg, size, pmsgbuf, &frameSize, WS_TEXT_FRAME);
	LOG("run broadcaseMsg")

	pthread_mutex_lock(&taskListMutex);
	it = list_iterator_new(ptasklist, LIST_HEAD);
    while ((node = list_iterator_next(it))) {
        ptask = (ClientTaskInfo*)(node->val);
        if ((ptask->runflag == TASK_FLAG_RUNNING) && (ptask->state == WS_STATE_NORMAL)) {
            LOG("send msg to client");
			pthread_mutex_lock(&ptask->sendMsgMutex);
			safeSend(ptask->sockfd, pmsgbuf, frameSize);
			pthread_mutex_unlock(&ptask->sendMsgMutex);

        }
    }
    list_iterator_destroy(it);
	pthread_mutex_unlock(&taskListMutex);
	if (pmsgbuf)
		free(pmsgbuf);
}

const char *fifoName = "/tmp/webSocketFifo";

void *readWebSockThreadFunc(void* param)
{
	#define PIPEBUFFERSIZE 1024
	int pipeFd = -1;
	char buffer[PIPEBUFFERSIZE + 1] = {0};
	printf("readWebSockThreadFunc\n");
    pthread_detach(pthread_self());
	if (access(fifoName, F_OK) == -1) {
		int res;
		res = mkfifo(fifoName, 0777);
		if (res != 0) {
			//mkfifo error
		}
	}
	int byteRead = 0;
	do {
		pipeFd = open(fifoName, O_RDONLY);
		if (pipeFd != -1) {
			byteRead = read(pipeFd, buffer, PIPEBUFFERSIZE);
			printf("reading %d: %s\n", byteRead, buffer);
			broadcastMsg(buffer, byteRead);
			close(pipeFd);
		}
	}while (1);
    printf("readWebSockThreadFunc exit\n");
}

void printUsage(char* appname){
    fprintf(stdout, "Usage:\n\t%s -p port -a authcommand -r runcommand", appname);
    fprintf(stdout, "\n\tauthcommand, the script accept 2 params, first is user, second is passwd, output true/false");
    fprintf(stdout, "\n\truncommand, the script accept 1 param, json format command. output response");
    fprintf(stdout, "\n");
}

int readOption(int argc, char** argv){
    int ret = 0;
    int ch;
    memset(&appoption, 0x00, sizeof(appoption));
    while((ch = getopt(argc, argv, "p:a:r:h")) != -1){
        switch(ch){
        case 'p':
            appoption.listenport = atoi(optarg);
            break;
        case 'a':
            COPYSTR(appoption.pauthscript, optarg)
            break;
        case 'r':
            COPYSTR(appoption.pcgiscript, optarg)
            break;
        case 'h':
            printUsage(argv[0]);
            break;
        default:
            break;            
        }

    }
    return 0;
}

int main(int argc, char** argv)
{
    readOption(argc, argv);
    if(appoption.listenport == 0x00){
        printUsage(argv[0]);
        return -1;
    }
            
    int listenSocket = socket(AF_INET, SOCK_STREAM, 0);
    if (listenSocket == -1) {
        error("create socket failed");
    }

    int sock_opt = 1;
    if (setsockopt(listenSocket, SOL_SOCKET, SO_REUSEADDR, (void*)&sock_opt, sizeof(sock_opt) ) == -1){
        return -1;
    }        
    
    struct sockaddr_in local;
    memset(&local, 0, sizeof(local));
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = INADDR_ANY;
    local.sin_port = htons(appoption.listenport);
    if (bind(listenSocket, (struct sockaddr *) &local, sizeof(local)) == -1) {
        error("bind failed");
    }
    
    if (listen(listenSocket, 1) == -1) {
        error("listen failed");
    }
    printf("opened %s:%d\n", inet_ntoa(local.sin_addr), ntohs(local.sin_port));
    
    ptasklist = list_new();
	pthread_mutex_init(&taskListMutex,NULL);

	pthread_t broadcaseThreadHandle;
    pthread_create(&broadcaseThreadHandle, NULL, readWebSockThreadFunc, NULL);
    
    while (TRUE) {
        struct sockaddr_in remote;
        socklen_t sockaddrLen = sizeof(remote);
        int clientSocket = accept(listenSocket, (struct sockaddr*)&remote, &sockaddrLen);
        if (clientSocket == -1) {
            error("accept failed");
        }
        
        printf("connected %s:%d\n", inet_ntoa(remote.sin_addr), ntohs(remote.sin_port));
        runClientThread(clientSocket);
        checkDeadThread();
    }
    
    close(listenSocket);
    return EXIT_SUCCESS;
}

