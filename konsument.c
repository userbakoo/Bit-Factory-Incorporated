#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <time.h>
#include <stdbool.h>

#define LOCALHOST "127.0.0.1"
#define CAPACITY_MULT 30720
#define READ_RATE 4435
#define DECAY_RATE 819
#define SAFE_MAX 50
#define READ_SIZE 4096
#define FULL_READ 13312

struct InputArguments
{
    int depoCapacity;
    float readingRate;
    float decayRate;
    char locAddress[16];
    size_t port;
};

struct Server
{
    int socketFd;
    struct sockaddr_in sockAddr;
};

struct Report
{
    struct timespec connectionTS;
    struct timespec firstBatchTS;
    struct timespec closedTS;
    struct timespec generationTS;
    struct sockaddr_in connectionAddress;
    int blockID;
};

void parseInputArguments(int, char**, struct InputArguments *);
void checkArgCount(int, char **);
void parseInputAddr(char**, struct InputArguments *);
void setupConnection(struct InputArguments *, struct Server *);
void receiveData(struct Server *, struct Report *, struct InputArguments *);
int readFromServer(struct Server *, int , struct InputArguments *, struct Report *);
void updateStorage(long *, int, struct timespec *, struct InputArguments *);

int getInt(char * arg);
double getFloat(char * arg);
struct timespec timespecDifference(struct timespec, struct timespec);
void parseTime(float, struct timespec *,int, int);

struct sockaddr_in generateAddress(int);
void generateReport(struct sockaddr_in);
void reportOnConnection(int, void *);

int main(int argc, char** argv)
{
    struct InputArguments inputArguments;
    struct Server server;
    parseInputArguments(argc, argv, &inputArguments);
    struct Report reportTab[SAFE_MAX];
    setupConnection(&inputArguments, &server);
    receiveData(&server, reportTab, &inputArguments);
    return 0;
}

struct sockaddr_in generateAddress(int fd)
{
    errno = 0;
    struct sockaddr_in myAddress;
    socklen_t addressLength = sizeof(myAddress);
    if((getsockname(fd, (struct sockaddr*) &myAddress, &addressLength)) == -1)
    {
        perror("getpeername");
        exit(EXIT_FAILURE);
    }
    return myAddress;
}

void generateReport(struct sockaddr_in myAddress)
{
    struct timespec time;
    clock_gettime(CLOCK_REALTIME, &time);
    char * p = ctime(&time.tv_sec);
    fprintf(stderr, "\n-----EXIT REPORT-----\n");
    fprintf(stderr,"%s",p);
    fprintf(stderr, "-----------------------\n");
}

struct timespec timespecDifference(struct timespec early, struct timespec late)
{
    struct timespec diff;
    if(early.tv_nsec > late.tv_nsec)
    {
        late.tv_sec--;
        late.tv_nsec += 1e9;
    }
    diff.tv_sec = late.tv_sec - early.tv_sec;
    diff.tv_nsec = late.tv_nsec - early.tv_nsec;

    return diff;
}

void parseTime(float multi, struct timespec * sleepTime, int size, int rate)
{
    size_t nanoTime = size / (multi * rate) * 1000000000;
    sleepTime->tv_sec = nanoTime / 1000000000;
    sleepTime->tv_nsec = nanoTime % (size_t)(1000000000);
}

void reportOnConnection(int exit, void * arg)
{
    struct Report thisReport = *(struct Report*)arg;
    struct timespec diff1, diff2;

    diff1 = timespecDifference(thisReport.connectionTS, thisReport.firstBatchTS);
    diff2 = timespecDifference(thisReport.firstBatchTS, thisReport.closedTS);

    fprintf(stderr, "\n----- REPORT ID %d -----\n", thisReport.blockID);
    fprintf(stderr, "PID: %d\n", getpid());
    fprintf(stderr, "Connection address: %s:%hu\n", inet_ntoa(thisReport.connectionAddress.sin_addr), ntohs(thisReport.connectionAddress.sin_port));
    fprintf(stderr, "firstBatch - connection : %lds %ldns\n", diff1.tv_sec, diff1.tv_nsec);
    fprintf(stderr, "connection - closed : %lds %ldns\n", diff2.tv_sec, diff2.tv_nsec);
    fprintf(stderr, "------------------------\n");

}

void updateStorage(long * currentCapacity, const int readSum, struct timespec * startTime, struct InputArguments * inputArguments)
{
    struct timespec decayTime, endTime;
    *currentCapacity += readSum;                 // Update current capacity and decay
    clock_gettime(CLOCK_MONOTONIC, &endTime);   // startTime - endTime is the interval in which decay occurs
    decayTime = timespecDifference(*startTime, endTime);
    // Remove decayed data
    *currentCapacity -= (long) (decayTime.tv_sec * (inputArguments->decayRate * DECAY_RATE) + (decayTime.tv_nsec / 1e9) * (inputArguments->decayRate * DECAY_RATE));
}

int readFromServer(struct Server * server, int connectionIter, struct InputArguments * inputArguments, struct Report * reportTab)
{
    int readSum = 0;
    char buf[READ_SIZE+1]={};           // Could also read into /dev/null
    while(readSum < FULL_READ)
    {

        // Determines the size of the package (READ_SIZE or whatever is left that is < than READ_SIZE)
        errno = 0;
        int readSize = ( FULL_READ - readSum > READ_SIZE ? READ_SIZE : FULL_READ - readSum );
        int readNum = read(server->socketFd, buf, readSize);
        if(readNum == -1)
        {
            perror("read from server");
            exit(EXIT_FAILURE);
        }
        if (readNum == 0)       // read EOF (server DC before full transaction)
        {
            fprintf(stderr,"Unexpected DC from the server.\n");
            exit(EXIT_FAILURE);
            // Could return -1 and make a DC report but I choose to exit.
        }
        // Not checking if readSize != readNum (shouldn't be an error)
        readSum += readNum;
        if(readSum == readNum)      // This means it's 1st package received
            clock_gettime(CLOCK_MONOTONIC, &reportTab[connectionIter].firstBatchTS);
        struct timespec sleepTime;
        parseTime(inputArguments->readingRate, &sleepTime, readNum, READ_RATE);
        nanosleep(&sleepTime, NULL);        // No signal to interrupt.
    }
    return readSum;
}

void receiveData(struct Server * server, struct Report * reportTab, struct InputArguments * inputArguments)
{
    long depoCapacity = inputArguments->depoCapacity * CAPACITY_MULT;       // Max capacity
    long currentCapacity = 0;                                               // Current capacity
    struct timespec startTime;          // Decay start timestamp
    struct sockaddr_in myAddress;       // Address that's put through to every connection report.

    int connectionIter = 0;         // Number of connection
    while(1)                        // True until capacity reached
    {
        clock_gettime(CLOCK_MONOTONIC, &startTime);     // Get decay start TS
        errno = 0;
        if((connect(server->socketFd, (struct sockaddr *)&server->sockAddr, sizeof(server->sockAddr))) == -1)
        {
            perror("connecting to server");
            exit(EXIT_FAILURE);
        }
        // Add this connection's address:port and connectionTS to reportTab structure
        reportTab[connectionIter].connectionAddress = generateAddress(server->socketFd);
        clock_gettime(CLOCK_MONOTONIC, &reportTab[connectionIter].connectionTS);

        // Read the FULL_READ bytes from server
        int readSum = readFromServer(server, connectionIter, inputArguments, reportTab);

        // Server has closed our connection - we read EOF
        errno = 0;
        int readByte = read(server->socketFd, NULL, 1);
        if(readByte != 0)
        {
            fprintf(stderr,"should have read EOF, instead read: %d", readByte);
            perror("read");
            exit(EXIT_FAILURE);
        }

        clock_gettime(CLOCK_MONOTONIC, &reportTab[connectionIter].closedTS);    // After reading EOF - get a TS
        reportTab[connectionIter].blockID = connectionIter;                     // Give the connection an ID

        // on_exit registers a function that writes out a report (will be executed in reverse order though)
        errno = 0;
        int onExit = on_exit(reportOnConnection, (void*)&reportTab[connectionIter]);
        if( onExit != 0)
        {
            perror("registering on_exit");
            exit(EXIT_FAILURE);
        }

        close(server->socketFd);                    // Preparing for the next connection
        setupConnection(inputArguments, server);    //

        updateStorage(&currentCapacity, readSum, &startTime, inputArguments);  // Updates storage values (read - decay)

        if(depoCapacity - currentCapacity < FULL_READ)  // Full capacity, success -> break leads into report and return;
            break;
        if(connectionIter++ == SAFE_MAX)
        {
            fprintf(stderr, "Reached SAFE_MAX amt of connections. [client got clinical depression and chose to end his existence]\n");
            exit(EXIT_FAILURE);
        }

    }
    generateReport(myAddress);      // Ending report.
}

void setupConnection(struct InputArguments * inputArguments, struct Server * server)
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
}


void parseInputArguments(int argc, char** argv, struct InputArguments * inputArguments)
{
    bool cFlag = false, pFlag = false, dFlag = false;
    checkArgCount(argc, argv);
    int opt;
    while ((opt = getopt(argc, argv, ":c:p:d:")) != -1) {
        switch (opt) {
            case 'c':
                inputArguments->depoCapacity = getInt(optarg);
                cFlag = true;
                break;
            case 'p':
                inputArguments->readingRate = (float)getFloat(optarg);
                pFlag = true;
                break;
            case 'd':
                inputArguments->decayRate = (float)getFloat(optarg);
                dFlag = true;
                break;
            case ':': // Missing argument
                fprintf(stderr, "Missing argument!\n");
                fprintf(stderr, "USAGE: -c <int> -p <float> -d <float> [<addr>:]port\n");
                exit(EXIT_FAILURE);
            case '?': // Unrecognized option
                fprintf(stderr, "Unrecognized option: %c%c, arg: %d\n",
                        argv[optind - 1][0],argv[optind - 1][1], optind-1);
                fprintf(stderr, "USAGE: -c <int> -p <float> -d <float> [<addr>:]port\n");
                exit(EXIT_FAILURE);
            default: // Unrecognized case in switch
                fprintf(stderr, "Unrecognized case\n");
                fprintf(stderr, "USAGE: -c <int> -p <float> -d <float> [<addr>:]port\n");
                exit(EXIT_FAILURE);
        }
    }

    if(!cFlag || !pFlag || !dFlag)
    {
        fprintf(stderr, "Did not find required flags!\n");
        fprintf(stderr, "USAGE: -c <int> -p <float> -d <float> [<addr>:]port\n");
        exit(EXIT_FAILURE);
    }
    parseInputAddr(argv, inputArguments);
}

void parseInputAddr(char ** argv, struct InputArguments * inputArguments)
{
    // Input addr is verified later by inet_aton (eg. if address is theoretically invalid, but goes through inet_aton - all is good
    if(argv[optind] == NULL)
    {
        fprintf(stderr, "USAGE: -c <int> -p <float> -d <float> [<addr>:]port\n");
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
        if(strlen(token) < 7 || strlen(token) > 15)     // Not checking if eg. 1.11111.1.1 is invalid - it will go through inet_aton
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
        fprintf(stderr, "USAGE: -c <int> -p <float> -d <float> [<addr>:]port\n");
        exit(EXIT_FAILURE);
    }
    if (res < 0)
    {
        fprintf(stderr,"Negative values not allowed\n");
        fprintf(stderr, "USAGE: -c <int> -p <float> -d <float> [<addr>:]port\n");
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
        fprintf(stderr, "USAGE: -c <int> -p <float> -d <float> [<addr>:]port\n");
        exit(EXIT_FAILURE);
    }
    if (res < 0)
    {
        fprintf(stderr,"Negative values not allowed\n");
        fprintf(stderr, "USAGE: -c <int> -p <float> -d <float> [<addr>:]port\n");
        exit(EXIT_FAILURE);
    }
    return res;
}

void checkArgCount(int argc, char ** argv)
{
    if((argc > 8 || argc < 5) || strcmp(argv[1], "--help") == 0)
    {
        fprintf(stderr, "USAGE: -c <int> -p <float> -d <float> [<addr>:]port\n");
        exit(EXIT_FAILURE);
    }
}