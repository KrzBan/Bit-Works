#define _POSIX_C_SOURCE 200809L
#define _GNU_SOURCE

//stolen from <sys/time.h>
#define timespecsub(a, b, result)                        \
    do                                                   \
    {                                                    \
        (result)->tv_sec = (a)->tv_sec - (b)->tv_sec;    \
        (result)->tv_nsec = (a)->tv_nsec - (b)->tv_nsec; \
        if ((result)->tv_nsec < 0)                       \
        {                                                \
            --(result)->tv_sec;                          \
            (result)->tv_nsec += 1000000000;             \
        }                                                \
    } while (0)

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/poll.h>
#include <sys/epoll.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <memory.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <time.h>
#include <unistd.h>

#define DEFAULT_ADDRESS "localhost"
#define BLOCK_SIZE 30720 
#define PORTION_SIZE 1024
#define DELIVERY_SIZE 13312
#define CONSUMPTION_UNIT 4435
#define DEGRADATION_UNIT 819

extern char *optarg;

typedef struct {
    struct timespec consumptionTs;
    size_t port;
    const char *address;
    float consumptionRate;
    float degradationRate;
    int bufferSize;
} Parameters;

typedef struct{
    struct sockaddr_in lastClientAddr;
    size_t blockCapacity;
    size_t blockSize;
    struct timespec lastConnectTs;
    struct timespec lastRecvTs;
    struct timespec lastCloseTs;
    struct timespec* connectTs;
    struct timespec* closeTs;
    struct timespec* blockTs;
    struct sockaddr_in* clientAddr;
    int localStorage;
} Stats;

void Error(const char *message);            //perror and exit
void Check(int value, const char *message); //compare with (-1) and Error()

size_t ParseULong(const char *arg);
float ParseFloat(const char *arg);
void ParseParameters(int argc, char *argv[]);

int ConnectToServer();
void AddBlockInfo();

int GetDelivery(int socketFd);
void PrintStatistics();

//These should be visible for the entire program, so I'm kind of fine with these "globals"
//There is also only one instance of these, I see no reason for them to be defined locally and passed as arguments
static Stats stats = {};
static Parameters parameters = {};

int main(int argc, char *argv[])
{
    parameters.address = DEFAULT_ADDRESS;
    ParseParameters(argc, argv);

    signal(SIGPIPE, SIG_IGN); //handle read/write errors by myself, quit the active loop and print statistics 

    while(1){

        int socketFd = ConnectToServer(&parameters);
        clock_gettime(CLOCK_MONOTONIC, &stats.lastConnectTs);

        socklen_t clientAddrSize = sizeof(stats.clientAddr);
        int getSockNameRes = getsockname(socketFd, &stats.lastClientAddr, &clientAddrSize);
        Check(getSockNameRes, "getsockname(): ");

        //Get a delivery, output is just how much data was read
        int dataRead = GetDelivery(socketFd);

        close(socketFd);    //server is supposed to close the connection, but client should also close the socket, right?
        clock_gettime(CLOCK_MONOTONIC, &stats.lastCloseTs);

        AddBlockInfo(&stats); //save current timestamps

        if (dataRead < DELIVERY_SIZE)   //we read less than we wanted, we probably lost connection and should just quit
            break;                      

        if(parameters.bufferSize - stats.localStorage < DELIVERY_SIZE) //no more space for deliveries
            break;
    }

    PrintStatistics(&stats);

    if(stats.connectTs)     free(stats.connectTs);
    if(stats.blockTs)       free(stats.blockTs);
    if(stats.closeTs)       free(stats.closeTs);
    if(stats.clientAddr)    free(stats.clientAddr);

    return EXIT_SUCCESS;
}

int ConnectToServer(){
    //Setup AF_INET
    int socketFd = socket(AF_INET, SOCK_STREAM, 0);
    Check(socketFd, "socket(): ");

    struct hostent *server = gethostbyname(parameters.address);
    if(server == NULL) Error("gethostbyname(): ");

    struct sockaddr_in serverAddress = {0};
    serverAddress.sin_family = AF_INET;
    serverAddress.sin_port = htons(parameters.port);
    memcpy(&serverAddress.sin_addr.s_addr, server->h_addr_list[0], server->h_length);

    int connectRes = connect(socketFd, &serverAddress, sizeof(serverAddress));
    Check(connectRes, "connect(): ");

    return socketFd;
}

int GetDelivery(int socketFd){

    int dataRead = 0;
    int firstDataBatch = 0;
    char buffer[DELIVERY_SIZE] = {};    //generally unused, but it's here
    do{
        struct timespec dgrdStart = {}; //degradation take place here, as this is the only place where the program blocks its' execution
        clock_gettime(CLOCK_MONOTONIC, &dgrdStart);

        //Request data
        int writeRes = write(socketFd, "Give me data!", 14);
        if (writeRes <= 0){
            fprintf(stderr, "Problem has occured on write(). Quitting...\n");
            break; //Server disconnected?
        }

        if (firstDataBatch == 0){
            clock_gettime(CLOCK_MONOTONIC, &stats.lastRecvTs);
            firstDataBatch = 1;
        }

        int readRes = read(socketFd, buffer + dataRead, PORTION_SIZE);
        if (readRes <= 0){
            fprintf(stderr, "Problem has occured on read(). Quitting...\n");
            break; //may mean that server disconnected,
        }
        dataRead += readRes;

        //Process data
        int nanoSleepRes = nanosleep(&parameters.consumptionTs, NULL);
        Check(nanoSleepRes, "nanosleep(): ");

        //Degrade and add to storage
        struct timespec dgrdEnd = {};
        clock_gettime(CLOCK_MONOTONIC, &dgrdEnd);

        int degraded = ((dgrdEnd.tv_sec + dgrdEnd.tv_nsec / 1e9) - (dgrdStart.tv_sec + dgrdStart.tv_nsec / 1e9)) * parameters.degradationRate;
        //First degrade, then add processed
        stats.localStorage -= degraded;
        if (stats.localStorage < 0) stats.localStorage = 0; //cant be negative
        stats.localStorage += readRes;

    } while (dataRead < DELIVERY_SIZE);

    return dataRead;
}

void PrintStatistics(){
    //Print statistics
    struct timespec timeStamp = {};
    clock_gettime(CLOCK_REALTIME, &timeStamp);
    fprintf(stderr, "Timestamp: %fs\n", timeStamp.tv_sec + timeStamp.tv_nsec / 1e9);

    pid_t myPid = getpid();
    fprintf(stderr, "Pid: %d\n", myPid);

    //all block statistics
    for (size_t i = 0; i < stats.blockSize; ++i){
        fprintf(stderr, "Block #%ld\n", i + 1);
        fprintf(stderr, "Address: %s:%d\n", inet_ntoa(stats.clientAddr[i].sin_addr), ntohs(stats.clientAddr[i].sin_port));

        struct timespec startDelay = {};
        timespecsub(&stats.blockTs[i], &stats.connectTs[i], &startDelay);
        fprintf(stderr, "Delay since connect: %15fs\n", startDelay.tv_sec + startDelay.tv_nsec / 1e9);

        struct timespec endDelay = {};
        timespecsub(&stats.closeTs[i], &stats.blockTs[i], &endDelay);
        fprintf(stderr, "Delay until close:   %15fs\n", endDelay.tv_sec + endDelay.tv_nsec / 1e9);
    }
}

void AddBlockInfo(){
    //All of this can also be achieved with on_exit(), probably much easier, but
    //this way, I can try to manage this myself (for fun) and also, all statistics are not being
    //printed in reverse order, as they would've been when using on_exit()
    
    //Allocate new memory
    if(stats.blockSize >= stats.blockCapacity){
        struct timespec *newBlock1 = calloc(sizeof(struct timespec), stats.blockCapacity * 2 + 1);
        struct timespec *newBlock2 = calloc(sizeof(struct timespec), stats.blockCapacity * 2 + 1);
        struct timespec *newBlock3 = calloc(sizeof(struct timespec), stats.blockCapacity * 2 + 1);
        struct sockaddr_in *newBlock4 = calloc(sizeof(struct sockaddr_in), stats.blockCapacity * 2 + 1);
        if(!newBlock1 || !newBlock2 || !newBlock3 || !newBlock4)
            Error("calloc(): ");

        memcpy(newBlock1, stats.connectTs, stats.blockSize * sizeof(struct timespec));
        memcpy(newBlock2, stats.blockTs, stats.blockSize * sizeof(struct timespec));
        memcpy(newBlock3, stats.closeTs, stats.blockSize * sizeof(struct timespec));
        memcpy(newBlock4, stats.clientAddr, stats.blockSize * sizeof(struct sockaddr_in));
        if (stats.connectTs != NULL)    free(stats.blockTs);
        if (stats.blockTs != NULL)      free(stats.connectTs);
        if (stats.closeTs != NULL)      free(stats.closeTs);
        if (stats.clientAddr != NULL)   free(stats.clientAddr);

        stats.blockCapacity     = stats.blockCapacity * 2 + 1;
        stats.connectTs         = newBlock1;
        stats.blockTs           = newBlock2;
        stats.closeTs           = newBlock3;
        stats.clientAddr        = newBlock4;
    }
    stats.connectTs[stats.blockSize]  = stats.lastConnectTs;
    stats.blockTs[stats.blockSize]    = stats.lastRecvTs;
    stats.closeTs[stats.blockSize]    = stats.lastCloseTs;
    stats.clientAddr[stats.blockSize] = stats.lastClientAddr;
    ++stats.blockSize;
}

void Error(const char *message){
    perror(message);
    exit(EXIT_FAILURE);
}

void Check(int value, const char *message){
    if (value == -1)
        Error(message);
}

void ParseParameters(int argc, char *argv[]){
    if (argv == NULL){
        fprintf(stderr, "ParseParameters(): argv is NULL\n");
        exit(EXIT_FAILURE);
    }

    int opt;
    while ((opt = getopt(argc, argv, "c:p:d:")) != -1){
        switch (opt){
        case 'c':
            parameters.bufferSize = ParseULong(optarg);
            break;
        case 'p':
            parameters.consumptionRate = ParseFloat(optarg);
            break;
        case 'd':
            parameters.degradationRate = ParseFloat(optarg);
            break;
        default:
            fprintf(stderr, "Usage: %s -c <size_t> -p <float> -d <float> [<addr>:]port\n", argv[0]);
            break;
        }
    }

    if (parameters.consumptionRate <= 0){
        fprintf(stderr, "ParseParameters(): -p <float> is mandatory and must be a positive, non-zero number.\n");
        exit(EXIT_FAILURE);
    }
    if (parameters.consumptionRate <= 0){
        fprintf(stderr, "ParseParameters(): -d <float> is mandatory and must be a positive, non-zero number.\n");
        exit(EXIT_FAILURE);
    }
    if (parameters.bufferSize == 0){
        fprintf(stderr, "ParseParameters(): -c <int> is mandatory and must be a positive, non-zero number.\n");
        exit(EXIT_FAILURE);
    }

    parameters.consumptionRate *= CONSUMPTION_UNIT;
    parameters.degradationRate *= DEGRADATION_UNIT;
    parameters.bufferSize      *= BLOCK_SIZE;

    if(parameters.degradationRate > parameters.consumptionRate){
        fprintf(stderr, "Degradation rate is higher than consumption rate!\n");
        fprintf(stderr, "Local storage will never be full.\n");
        exit(EXIT_FAILURE);
    }

    char *addressStr = NULL;
    for (int i = optind; i < argc; ++i){
        if (addressStr != NULL){
            fprintf(stderr, "Too many arguments!\n");
            fprintf(stderr, "Usage: %s -c <size_t> -p <float> -d <float> [<addr>:]port\n", argv[0]);
            exit(EXIT_FAILURE);
        }
        addressStr = argv[optind];
    }
    if (addressStr == NULL){
        fprintf(stderr, "ParseParameters(): [<addr>:]port is mandatory.\n");
        exit(EXIT_FAILURE);
    }

    //Find first occurence of ":" and split the string
    char *ipStr = NULL;
    char *portStr = addressStr;
    for (char *c = addressStr + 1; *c; ++c){
        if (*c == ':'){
            *c = '\0';
            ipStr = addressStr;
            portStr = c + 1;
            break;
        }
    }
    parameters.port = ParseULong(portStr);
    if (ipStr != NULL){
        parameters.address = ipStr;
    }

    size_t consumptionSleepNs = PORTION_SIZE * 1e9 / parameters.consumptionRate;
    parameters.consumptionTs.tv_sec = consumptionSleepNs / 1e9;
    parameters.consumptionTs.tv_nsec = consumptionSleepNs % (size_t)1e9;
}

size_t ParseULong(const char *arg){
    errno = 0;
    char *end = NULL;

    if (arg == NULL){
        fprintf(stderr, "ParseULong(): arg is NULL\n");
        exit(EXIT_FAILURE);
    }
    if (arg[0] == '-'){
        fprintf(stderr, "ParseULong(): Failed to parse %s into unsigned long. Passed argument is a negative value.\n", optarg);
        exit(EXIT_FAILURE);
    }

    size_t num = strtoul(arg, &end, 0);
    if (*end != '\0' || errno != 0)
        Error("strotoul(): ");

    return num;
}

float ParseFloat(const char *arg){
    errno = 0;
    char *end = NULL;

    if (arg == NULL){
        fprintf(stderr, "ParseFloat(): arg is NULL\n");
        exit(EXIT_FAILURE);
    }

    float val = strtof(arg, &end);
    if ((val == 0 && errno != 0) || *end != '\0')
        Error("strtof(): ");

    return val;
}