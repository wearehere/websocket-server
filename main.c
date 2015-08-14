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
    pthread_t pt;
    uint8_t *msgbuf;
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
    pclitask->msgbuf = (char*)malloc(BUF_LEN);
    pthread_create(&pclitask->pt, NULL, taskThreadFunc, pclitask);
    list_node_t *pnewnode = list_node_new(pclitask);
    list_rpush(ptasklist, pnewnode);
}

void checkDeadThread(){
    LOG("checkDeadThread")
    ClientTaskInfo* ptask = NULL;
    list_node_t *node;
    list_iterator_t *it = list_iterator_new(ptasklist, LIST_HEAD);
    while ((node = list_iterator_next(it))) {
        ptask = (ClientTaskInfo*)(node->val);
        if(ptask->runflag == TASK_FLAG_IDLE){
            LOG("checkDeadThread:remove one Dead thread")
            list_remove(ptasklist, node);
            FREE_MEM(ptask->prunurl)
            FREE_MEM(ptask->msgbuf)
            free(ptask);
            break;            
        }
    }
    list_iterator_destroy(it);
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

//void clientWorker(int clientSocket, char* pmsgbuf)
void clientWorker(ClientTaskInfo* pinfo)
{
    int clientSocket = pinfo->sockfd;
    char* pmsgbuf = pinfo->msgbuf;
    
    memset(pmsgbuf, 0, BUF_LEN);
    size_t readedLength = 0;
    size_t frameSize = BUF_LEN;
    enum wsState state = WS_STATE_OPENING;
    uint8_t *data = NULL;
    size_t dataSize = 0;
    enum wsFrameType frameType = WS_INCOMPLETE_FRAME;
    struct handshake hs;
    nullHandshake(&hs);
    
    #define prepareBuffer frameSize = BUF_LEN; memset(pmsgbuf, 0, BUF_LEN);
    #define initNewFrame frameType = WS_INCOMPLETE_FRAME; readedLength = 0; memset(pmsgbuf, 0, BUF_LEN);
    
    while (frameType == WS_INCOMPLETE_FRAME) {
        ssize_t readed = recv(clientSocket, pmsgbuf+readedLength, BUF_LEN-readedLength, 0);
        if (readed <= 0) {
            close(clientSocket);
            fprintf(stdout, "[clientWorker]readed <= 0\n");
            perror("recv failed\n");
            return;
        }
        #ifdef PACKET_DUMP
        printf("in packet:%d\n", (int)readed);
        //fwrite(pmsgbuf, 1, readed, stdout);
        LOG_HEX(pmsgbuf, readed);
        //printf("\n");
        #endif
        readedLength+= readed;
        assert(readedLength <= BUF_LEN);
        
        if (state == WS_STATE_OPENING) {
            frameType = wsParseHandshake(pmsgbuf, readedLength, &hs);
        } else {
            frameType = wsParseInputFrame(pmsgbuf, readedLength, &data, &dataSize);
        }
        
        if ((frameType == WS_INCOMPLETE_FRAME && readedLength == BUF_LEN) || frameType == WS_ERROR_FRAME) {
            if (frameType == WS_INCOMPLETE_FRAME)
                printf("buffer too small");
            else
                printf("error in incoming frame\n");
            
            if (state == WS_STATE_OPENING) {
                prepareBuffer;
                frameSize = sprintf((char *)pmsgbuf,
                                    "HTTP/1.1 400 Bad Request\r\n"
                                    "%s%s\r\n\r\n",
                                    versionField,
                                    version);
                safeSend(clientSocket, pmsgbuf, frameSize);
                fprintf(stdout, "[clientWorker]WS_INCOMPLETE_FRAME  && WS_STATE_OPENING\n");
                break;
            } else {
                prepareBuffer;
                wsMakeFrame(NULL, 0, pmsgbuf, &frameSize, WS_CLOSING_FRAME);
                if (safeSend(clientSocket, pmsgbuf, frameSize) == EXIT_FAILURE){
                    fprintf(stdout, "[clientWorker]WS_INCOMPLETE_FRAME\n");
                    break;                    
                }
                state = WS_STATE_CLOSING;
                initNewFrame;
            }
        }
        
        if (state == WS_STATE_OPENING) {
            assert(frameType == WS_OPENING_FRAME);
            if (frameType == WS_OPENING_FRAME) {
                // if resource is right, generate answer handshake and send it
                // if (strcmp(hs.resource, "/echo") != 0) {
                //     frameSize = sprintf((char *)pmsgbuf, "HTTP/1.1 404 Not Found\r\n\r\n");
                //     safeSend(clientSocket, pmsgbuf, frameSize);
                //     break;
                // }
                
                fprintf(stdout, "[hs.resource]:%s\n", hs.resource);
                //TODO use token as URL, check TOKEN or PASSWD, invoke TOKEN auth script
                COPYSTR(pinfo->prunurl, hs.resource)
                //char* pret = runScript(appinfo.pauthscript, hs.resource);
                //if(strcmp(pret, "true"))
                //{ safeSend("invalid user.")
                //  free(pret); break;
                //}
                //free(pret)
                
                prepareBuffer;
                wsGetHandshakeAnswer(&hs, pmsgbuf, &frameSize);
                freeHandshake(&hs);
                if (safeSend(clientSocket, pmsgbuf, frameSize) == EXIT_FAILURE){
                    fprintf(stdout, "[clientWorker]safeSend error\n");
                    break;                    
                }
                state = WS_STATE_NORMAL;
                initNewFrame;
            }
        } else {
            if (frameType == WS_CLOSING_FRAME) {
                fprintf(stdout, "[clientWorker]WS_CLOSING_FRAME\n");
                if (state == WS_STATE_CLOSING) {
                    break;
                } else {
                    prepareBuffer;
                    wsMakeFrame(NULL, 0, pmsgbuf, &frameSize, WS_CLOSING_FRAME);
                    safeSend(clientSocket, pmsgbuf, frameSize);
                    break;
                }
            } else if (frameType == WS_TEXT_FRAME) {
                uint8_t *recievedString = NULL;
                recievedString = malloc(dataSize+1);
                assert(recievedString);
                memcpy(recievedString, data, dataSize);
                recievedString[ dataSize ] = 0;
                
                fprintf(stdout, "RECV:%s\n", recievedString);
                char* presp = runScript(appoption.pcgiscript, recievedString);
                if(presp != NULL){
                    fprintf(stdout, "SCRIPT RESP:%s\n", presp);
                    prepareBuffer;
                    wsMakeFrame(presp, strlen(presp), pmsgbuf, &frameSize, WS_TEXT_FRAME);
                    //wsMakeFrame(recievedString, dataSize, gBuffer, &frameSize, WS_TEXT_FRAME);
                    free(presp);
                    
                    if (safeSend(clientSocket, pmsgbuf, frameSize) == EXIT_FAILURE){
                        free(recievedString);
                        fprintf(stdout, "[clientWorker]safeSend error\n");                        
                        break;
                    }
                }
                free(recievedString);
                initNewFrame;
            }
        }
    } // read/write cycle
    
    fprintf(stdout, "[clientWorker]close socket, exit..\n");
    close(clientSocket);
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
        //clientWorker(clientSocket);
        //printf("disconnected\n");
    }
    
    close(listenSocket);
    return EXIT_SUCCESS;
}

