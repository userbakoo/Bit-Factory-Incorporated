#define _GNU_SOURCE

#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/ioctl.h>
#include <time.h>
#include <sys/poll.h>
#include <sys/timerfd.h>
#include <signal.h>
#include <bits/signum.h>

#include "buffer.h"

#define BASE_RATE 2662
#define BLOCK_SIZE 650
#define LOCALHOST "127.0.0.1"
#define MAX_CLIENTS 100
#define PACKAGE_SIZE 4096
#define SEND_THRESHOLD 13312
#define POLL_WAIT 100

struct Server {
    int socketFd;
    struct sockaddr_in sockAddr;
};

struct InputArguments
{
    float productionRate;
    char locAddress[16];
    size_t port;
};

struct ClientTransferData
{
    int alreadySent;
    struct sockaddr_in sockAddr;
};

struct Storage
{
    int currentStorage;
    int prevStorage;        // Stored to compare in the 5 sec intervals
    int reservedData;
    int freeData;
    float percentage;
};

void parseInputArguments(int, char**, struct InputArguments *);
void checkArgCount(int, char**);
void parseInputAddr(char**, struct InputArguments *);
int setupStorage(float);
void setupServer(struct Server *, struct InputArguments *);
void trainPeon(int*, float);
void workWork(float, int);
void setupPollFD(struct pollfd *, struct Server);
void pollTheFDs(struct pollfd *, struct buffer *, struct ClientTransferData *, struct Storage *, int *, struct Server *, int);
void updateStorage(struct Storage *, int);
void pollClients(int *, struct pollfd *, struct ClientTransferData *, int, struct Storage *, int *);
void pollServer(struct pollfd *, struct buffer *, const int *, int *, struct Server *);
void pollTimer(struct pollfd *, struct buffer *, struct Storage *, const int *);

void parseTime(float, struct timespec *);
int getInt(char * arg);
double getFloat(char * arg);

void clientDisconnectReport(struct ClientTransferData clientData);
void intervalReport(int, int, struct Storage );


int main(int argc, char** argv)
{
    signal(SIGCHLD, SIG_IGN);
    signal(SIGPIPE, SIG_IGN);
    struct Server server;
    struct InputArguments inputArguments;
    parseInputArguments(argc, argv, &inputArguments);
    int pipeRead = setupStorage(inputArguments.productionRate);
    setupServer(&server, &inputArguments);
    struct pollfd pollFD[MAX_CLIENTS+2];        // MAX_CLIENTS + serverFD + timerFD
    setupPollFD(pollFD, server);

    struct ClientTransferData clientData[MAX_CLIENTS];
    int clientDataSize = 0;
    struct buffer* clientQueue = create(MAX_CLIENTS);
    struct Storage storage;

    while(1)
    {
        updateStorage(&storage, pipeRead);
        while(storage.freeData >= SEND_THRESHOLD && getCurrentSize(clientQueue) != 0)   // Adds clients to the poll
        {
            for(int i = 0; i < MAX_CLIENTS; i++)
            {
                if(pollFD[i].fd == -1)
                {
                    pollFD[i].fd = pop(clientQueue);    // This adds the client to poll (should always succeed)
                    clientDataSize++;                   // Update clientStructure data to reflect the pollFD structure
                    clientData[i].alreadySent = 0;
                    socklen_t addressLength = sizeof(clientData->sockAddr);
                    // Need to get the address before potential DC from the client (to report)
                    if((getpeername(pollFD[i].fd, (struct sockaddr*) &clientData->sockAddr, &addressLength)) == -1)
                    {
                        perror("getpeername");
                        exit(EXIT_FAILURE);
                    }

                    storage.freeData -= SEND_THRESHOLD;          // Allocating storage data
                    storage.reservedData += SEND_THRESHOLD;      //
                    break;
                }
            }
        }
        pollTheFDs(pollFD, clientQueue, clientData, &storage, &clientDataSize, &server, pipeRead);
    }
}

void pollTimer(struct pollfd * pollFD, struct buffer * clientQueue, struct Storage * storage, const int * clientDataSize)
{
    if(pollFD[MAX_CLIENTS+1].revents & POLLERR)             // POLLERR for the timerFD
    {
        perror("timerFD pollerr");
        exit(EXIT_FAILURE);
    }
    if(pollFD[MAX_CLIENTS+1].revents & POLLIN)              // POLLIN for the timerFD (5 sec interval timeout)
    {
        uint64_t timesExpired;      // Could use this for some warnings but whatever
        int readTimerErr = read(pollFD[MAX_CLIENTS+1].fd, &timesExpired, sizeof(timesExpired) );
        if(readTimerErr == -1)
        {
            perror("read timerfd");
            exit(EXIT_FAILURE);
        }
        intervalReport(*clientDataSize, getCurrentSize(clientQueue), *storage); // 5 sec interval report
        storage->prevStorage = storage->currentStorage;                         //
    }
}

void pollServer(struct pollfd * pollFD, struct buffer * clientQueue, const int * clientDataSize, int * ready, struct Server * server)
{
    if(pollFD[MAX_CLIENTS].revents & POLLERR)       // POLLERR for the serverFD
    {
        perror("serverFD pollerr");
        exit(EXIT_FAILURE);
    }
    if(pollFD[MAX_CLIENTS].revents & POLLIN)        // POLLIN for the serverFD (new connection)
    {

        if(getCurrentSize(clientQueue) + *clientDataSize < MAX_CLIENTS)      // This checks if we exceed MAX_CLIENTS
        {                                                                    // (both in queue and currently polled)
            struct sockaddr_in clientAddress;
            uint32_t clientSize = sizeof(clientAddress);
            int clientFd = accept(server->socketFd, (struct sockaddr *)&clientAddress, &clientSize);
            if(clientFd == -1)
            {
                perror("accept clientFD");
                exit(EXIT_FAILURE);
            }                                       // Not adding to the poll yet
            push(clientQueue, clientFd);            // Accepted client gets pushed onto a circular buffer queue (should always succeed)
            (*ready)--;
            return;
        }
        else
        {
            // Can't fit more clients
            pollFD[MAX_CLIENTS].revents = 0;        // Idk if needed
            return;
        }
    }
}

void pollClients(int *ready, struct pollfd * pollFD, struct ClientTransferData * clientData, int pipeRead, struct Storage * storage, int * clientDataSize)
{
    int iterator = 0;
    while( (*ready) && iterator < MAX_CLIENTS)       // Iterate over all the polled descriptors
    {                                                // Theoretically (ready && iterator < clientDataSize should suffice)
        if(pollFD[iterator].revents & POLLHUP)
        {
            // -- Client disconnected. Need to flush down the wasted data and update all the structures.
            (*ready)--;

            if(clientData[iterator].alreadySent != 0) // Transmission has begun, dump the rest of the data
            {
                int wastedData = 0;
                char buf[SEND_THRESHOLD];                           // Could read into /dev/null instead of buf
                errno = 0;
                if((wastedData = read(pipeRead,  buf, SEND_THRESHOLD - clientData[iterator].alreadySent)) == -1)
                {
                    perror("read waste from pipe");
                    exit(EXIT_FAILURE);
                }
                storage->reservedData -= wastedData;
            }
            else        // No transmission - can recover the data
            {
                storage->reservedData -= SEND_THRESHOLD;
                storage->freeData += SEND_THRESHOLD;
                clientData[iterator].alreadySent = SEND_THRESHOLD;       // So the report lines up (0 bytes wasted)
            }
            clientDisconnectReport(clientData[iterator]);       // Prints the report
            clientData[iterator].alreadySent = 0;               // Reuse clientData structure
            (*clientDataSize)--;                                //

            pollFD[iterator].fd = -1;                           // Reuse pollFD structure
            close(pollFD[iterator].fd);                         //
        }
        if(pollFD[iterator].revents & POLLOUT)                  // Sending client the data
        {
            errno = 0;
            int testForDc = recv(pollFD[iterator].fd, NULL, 1, MSG_PEEK|MSG_DONTWAIT);
            if(testForDc == 0)      // Test if client has already DC'd
            {
                pollFD[iterator].revents = POLLHUP;     // If DC - send him straight to POLLHUP
                continue;
            }

            if(clientData[iterator].alreadySent == SEND_THRESHOLD)       // If the transaction has completed
            {
                // Write a report. Disconnect the client. Reuse structures.
                clientDisconnectReport(clientData[iterator]);
                close(pollFD[iterator].fd);
                pollFD[iterator].fd = -1;
                clientData[iterator].alreadySent = 0;
                (*clientDataSize)--;
                break;
            }

            // --- Transmission ---

            // Determines the size of the package (PACKAGE_SIZE or whatever is left that is < than PACKAGE_SIZE)
            int readSize = ( SEND_THRESHOLD - clientData[iterator].alreadySent > PACKAGE_SIZE ?
                             PACKAGE_SIZE : SEND_THRESHOLD - clientData[iterator].alreadySent );

            int num = 0;
            errno = 0;
            char package[PACKAGE_SIZE]={};                          // Read the package from pipe
            if((num = read(pipeRead, package, readSize)) == -1)
            {
                perror("read from pipe");
                exit(EXIT_FAILURE);
            }
            if((num = write(pollFD[iterator].fd, package, readSize)) == -1)     // Write it to the client
            {
                perror("write to client");
                exit(EXIT_FAILURE);
            }
            // Not checking if num == readSize (shouldn't be an error)
            clientData[iterator].alreadySent += num;                // Update total num of bytes send
            storage->reservedData -= num;                           // Update total amt. of reserved data
            (*ready)--;
            updateStorage(storage, pipeRead);                       // Reassess the storage (mb not necessary)
        }
        iterator++;
    }
}

void pollTheFDs(struct pollfd * pollFD, struct buffer * clientQueue, struct ClientTransferData * clientData, struct Storage * storage, int * clientDataSize, struct Server * server, int pipeRead)
{
    int ready = 0;
    while((ready = poll(pollFD, MAX_CLIENTS+2, POLL_WAIT)) != -1)   // not sure what's the best POLL_WAIT value
    {
        if(ready == 0)      // If timeout:
            break;          // Break so we can continually check if storage current size >= 13KiB
        errno = 0;
        pollTimer(pollFD, clientQueue, storage, clientDataSize);
        pollServer(pollFD, clientQueue, clientDataSize, &ready, server);
        pollClients(&ready, pollFD, clientData, pipeRead, storage, clientDataSize);
    }
}

void intervalReport(int cntPolled, int cntQueued, struct Storage storage)
{
    struct timespec reportTime;
    clock_gettime(CLOCK_REALTIME, &reportTime);
    char * p = ctime(&reportTime.tv_sec);

    fprintf(stderr, "\n-----INTERVAL REPORT-----\n");
    fprintf(stderr,"%s", p);
    fprintf(stderr, "Clients - total: %d, polled: %d, queued: %d\n", cntPolled+cntQueued, cntPolled, cntQueued);
    fprintf(stderr, "Flow: %d\n", storage.currentStorage - storage.prevStorage);
    fprintf(stderr, "Storage status : %d, %2.2f %%\n", storage.currentStorage, storage.percentage* 100);
    fprintf(stderr, "-------------------------\n");
}

void clientDisconnectReport(struct ClientTransferData clientData)
{
    struct timespec reportTime;
    clock_gettime(CLOCK_REALTIME, &reportTime);
    char * p = ctime(&reportTime.tv_sec);
    fprintf(stderr, "\n-----DISCONNECT REPORT-----\n");
    fprintf(stderr,"%s", p);
    fprintf(stderr, "Client address: %s:%hu\n", inet_ntoa(clientData.sockAddr.sin_addr), ntohs(clientData.sockAddr.sin_port));
    fprintf(stderr, "Wasted data: %d (bytes)\n", SEND_THRESHOLD-clientData.alreadySent);
    fprintf(stderr, "---------------------------\n");
}

void updateStorage(struct Storage * storage, int pipeRead)
{
    int ioctlErr = ioctl(pipeRead, FIONREAD, &storage->currentStorage);
    if(ioctlErr == -1)
    {
        perror("ioctl FIONREAD");
        exit(EXIT_FAILURE);
    }
    int pipeSize = fcntl(pipeRead, F_GETPIPE_SZ);
    if(pipeSize == -1)
    {
        perror("F_GETPIPE_SZ");
        exit(EXIT_FAILURE);
    }
    storage->freeData = storage->currentStorage - storage->reservedData;
    storage->percentage =(float)storage->currentStorage/(float)pipeSize;
}

void setupPollFD(struct pollfd * pollFD, struct Server server)
{
    //  pollFD[0] - pollFD[MAX_CLIENTS -1] == client indexes
    //  pollFD[MAX_CLIENTS]                == server index
    //  pollFD[MAX_CLIENTS+1]              == timerFD index

    int timerFd = timerfd_create(CLOCK_REALTIME, 0);
    struct timespec timerInterval = {.tv_sec = 5, .tv_nsec = 0};
    struct itimerspec timerISpec= {.it_interval=timerInterval, .it_value=timerInterval};
    errno = 0;
    if( timerfd_settime(timerFd, 0, &timerISpec, NULL) == -1)
    {
        perror("timerfd_settime");
        exit(EXIT_FAILURE);
    }

    pollFD[MAX_CLIENTS].fd = server.socketFd;   // Server poll
    pollFD[MAX_CLIENTS].events |= POLLIN;
    pollFD[MAX_CLIENTS].events |= POLLERR;

    pollFD[MAX_CLIENTS+1].fd = timerFd;         // TimerFD poll
    pollFD[MAX_CLIENTS+1].events |= POLLIN;
    pollFD[MAX_CLIENTS+1].events |= POLLERR;

    for(int i=0; i<MAX_CLIENTS; i++)
    {
        pollFD[i].fd = -1;                      // ClientFDs poll
        pollFD[i].events |= POLLOUT;
        pollFD[i].events |= POLLHUP;
    }
}

int setupStorage(float productionRate)
{
    int pipeFD[2]={};
    errno = 0;
    int pipeErr = pipe(pipeFD);     // Pipe for communication  between the server and the worker
    if(pipeErr == -1)
    {
        perror("pipe");
        exit(EXIT_FAILURE);
    }
    trainPeon(pipeFD, productionRate);      // Creates the child process
    return pipeFD[0];                       // Returns the file descriptor
}

void trainPeon(int * pipeFD, float productionRate)
{
    pid_t currPID = fork();
    if( currPID == -1 )                   // -- Errors
    {
        perror("fork");
        exit(EXIT_FAILURE);
    }
    else if ( currPID == 0 )             // -- Child
    {
        close(pipeFD[0]);            // -- Close read
        workWork(productionRate, pipeFD[1]);
        exit(EXIT_SUCCESS);
    }
    else                                // -- Parent
    {
        close(pipeFD[1]);           // -- Close write
    }
}

void workWork(float productionRate, int pipeWrite)
{
    // The peon, in his eternal struggle, works to produce data

    struct timespec sleepTime={};
    parseTime(productionRate, &sleepTime);
    errno = 0;
    int pipeSize = fcntl(pipeWrite, F_GETPIPE_SZ);
    if(pipeSize == -1)
    {
        perror("F_GETPIPE_SZ");
        exit(EXIT_FAILURE);
    }
    char theBlock[BLOCK_SIZE]={};
    char value = 65;
    while(1)
    {
        nanosleep(&sleepTime, NULL);    // No signal to interrupt.
        memset(theBlock, value, BLOCK_SIZE);
        value++;
        if(value == 91)
            value = 97;
        if(value == 123)
            value = 65;
        errno = 0;
        if(write(pipeWrite, theBlock, BLOCK_SIZE) == -1)
        {
            if(errno == EPIPE)
            {
                perror("epipe");
                exit(EXIT_FAILURE);
            }
            else
            {
                perror("write");
                exit(EXIT_FAILURE);
            }
        }
    }
}

void parseTime(float productionRate, struct timespec * sleepTime)
{
    // First translate to nano seconds so we don't lose data from float precision and simple division
    size_t nanoTime = BLOCK_SIZE / (productionRate * BASE_RATE) * 1000000000;
    sleepTime->tv_sec = nanoTime / 1000000000;
    sleepTime->tv_nsec = nanoTime % (size_t)(1000000000);
}

void parseInputArguments(int argc, char** argv, struct InputArguments * inputArguments)
{
    bool pFlag = false;
    checkArgCount(argc,argv);
    int opt;
    while ((opt = getopt(argc, argv, ":p:")) != -1) {
        switch (opt) {
            case 'p':
                inputArguments->productionRate = (float)getFloat(optarg);
                pFlag = true;
                break;
            case ':': // Missing argument
                fprintf(stderr, "Missing argument!\n");
                fprintf(stderr, "USAGE: -p <float> [<addr>:]port \n");
                exit(EXIT_FAILURE);
            case '?': // Unrecognized option
                fprintf(stderr, "Unrecognized option: %c%c, arg: %d\n",
                        argv[optind - 1][0],argv[optind - 1][1], optind-1);
                fprintf(stderr, "USAGE: -p <float> [<addr>:]port\n");
                exit(EXIT_FAILURE);
            default: // Unrecognized case in switch
                fprintf(stderr, "Unrecognized case\n");
                fprintf(stderr, "USAGE: -p <float> [<addr>:]port\n");
                exit(EXIT_FAILURE);
        }
    }
    if(!pFlag)
    {
        fprintf(stderr, "Did not find required flags!\n");
        fprintf(stderr, "USAGE: -p <float> [<addr>:]port \n");
        exit(EXIT_FAILURE);
    }
    parseInputAddr(argv, inputArguments);
}

void parseInputAddr(char ** argv, struct InputArguments * inputArguments)
{
    // Input addr is verified later by inet_aton (eg. if address is theoretically invalid, but goes through inet_aton - all is good
    if(argv[optind] == NULL)
    {
        fprintf(stderr, "USAGE: -p <float> [<addr>:]port\n");
        exit(EXIT_FAILURE);
    }
    if(strchr(argv[optind], ':') == NULL)
    {
        // This means we use the default address
        inputArguments->port = getInt(argv[optind]);
        strcpy(inputArguments->locAddress, LOCALHOST);  // LOCALHOST == "127.0.0.1" [hope this is ok]
    }
    else
    {
        char *token;
        token = strtok(argv[optind], ":");
        if(strlen(token) < 7 || strlen(token) > 15) // Not checking if eg. 1.11111.1.1 is invalid - it will go through inet_aton
        {
            fprintf(stderr, "Bad address.\n");
            fprintf(stderr, "USAGE: -c <int> -p <float> -d <float> [<addr>:]port\n");
            exit(EXIT_FAILURE);
        }
        if(strcmp(token, "localhost") == 0)
            strcpy(inputArguments->locAddress, LOCALHOST);
        else
            strncpy(inputArguments->locAddress, token, strlen(token));
        token = strtok(NULL, "");
        inputArguments->port = (short) getInt(token);
    }
}

void setupServer(struct Server * server, struct InputArguments * inputArguments)
{
    errno = 0;
    if((server->socketFd = socket(AF_INET, SOCK_STREAM, 0)) == -1)
    {
        perror("creating socket");
        exit(EXIT_FAILURE);
    }

    server->sockAddr.sin_family = AF_INET;
    server->sockAddr.sin_port = htons(inputArguments->port);

    errno = 0;
    if((inet_aton(inputArguments->locAddress, &server->sockAddr.sin_addr)) == 0)
    {
        perror("inet_aton couldn't parse provided address");
        exit(EXIT_FAILURE);
    }

    errno = 0;
    if((bind(server->socketFd, (struct sockaddr*) &server->sockAddr, sizeof(server->sockAddr))) == -1)
    {
        perror("bind server socket");
        exit(EXIT_FAILURE);
    }

    errno = 0;
    if((listen(server->socketFd, 5)) == -1)
    {
        perror("listen server socket");
        exit(EXIT_FAILURE);
    }
}

double getFloat(char * arg)
{
    double res;
    char *endptr;
    errno = 0;
    res = strtod(arg, &endptr);
    if (errno != 0)
        perror("strtod");
    if (*endptr != '\0')
    {
        fprintf(stderr,"Non-numeric argument: %s, at %s, arg: %d\n", arg, endptr, optind-1);
        fprintf(stderr, "USAGE: -p <float> [<addr>:]port\n");
        exit(EXIT_FAILURE);
    }
    if (res < 0)
    {
        fprintf(stderr,"Negative values not allowed\n");
        fprintf(stderr, "USAGE: -p <float> [<addr>:]port\n");
        exit(EXIT_FAILURE);
    }
    return res;
}

int getInt(char * arg)
{
    int res;
    char *endptr;
    errno = 0;
    res = (int) strtol(arg, &endptr, 0);
    if (errno != 0)
        perror("strtol");
    if (*endptr != '\0')
    {
        fprintf(stderr,"Non-numeric argument: %s, at %s, arg: %d\n", arg, endptr, optind-1);
        fprintf(stderr, "USAGE: -p <float> [<addr>:]port\n");
        exit(EXIT_FAILURE);
    }
    if (res < 0)
    {
        fprintf(stderr,"Negative values not allowed\n");
        fprintf(stderr, "USAGE: -p <float> [<addr>:]port\n");
        exit(EXIT_FAILURE);
    }
    return res;
}

void checkArgCount(int argc, char ** argv)
{
    if( (argc > 4 || argc < 3) || strcmp(argv[1], "--help") == 0)
    {
        fprintf(stderr, "USAGE: -p <float> [<addr>:]port \n");
        exit(EXIT_FAILURE);
    }
}
