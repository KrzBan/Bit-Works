#define _POSIX_C_SOURCE 200809L
#define _GNU_SOURCE

//#define KB_DEBUG    //KB, as Krzysztof Bański, like my own namespace
#ifdef KB_DEBUG
#define Debug(x) printf(x)
#else
#define Debug(x)
#endif

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/poll.h>
#include <sys/epoll.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/timerfd.h>
#include <arpa/inet.h>
#include <memory.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define DEFAULT_ADDRESS "localhost"
#define BLOCK_SIZE 640
#define PORTION_SIZE 1024
#define DELIVERY_SIZE 13312
#define PRODUCTION_UNIT 2662
#define EPOLL_INITIAL 3 //listen, pipe, timer

extern char *optarg;

typedef struct{
    float productionRate;
    const char *address;
    size_t port;
} Parameters;

typedef struct{
    size_t lifetimeClients;
    size_t connectedClients;
    size_t pipeSize;
    size_t pipeCapacity;
    int lastThroughput;
} Stats;

typedef struct{
    int fd;
    char buffer[DELIVERY_SIZE];
    size_t dataSent;
} ClientData;
ClientData *CreateClientData(int fd);

typedef struct{
    int *data;
    size_t size;
    size_t capacity;
    size_t begin;
    size_t end;
} CircularBuffer;

CircularBuffer CreateCB(size_t size);
void PushCB(CircularBuffer *cb, int value);
void PopCB(CircularBuffer *cb);
size_t NextCB(CircularBuffer *cb, size_t it);
void ClearCB(CircularBuffer *cb);

void Error(const char *message);            //perror and exit
void Check(int value, const char *message); //compare with "-1" and Error()

size_t ParseULong(const char *arg);
float ParseFloat(const char *arg);
void ParseParameters(int argc, char *argv[], Parameters *parameters);

void SetupSIGCHLD();
void HandleSIGCHLD(int code);

int SetupWarehouse(float productionRate);
void WarehouseWorker(float productionRate, int pipeWrite);

int SetupConection(Parameters* parameters);
int SetupEpoll(int listenFd, int pipeFd, int timerFd);

void ExpandRevents(struct epoll_event** revents, size_t *size);    //size, in and out parameter
void DispatchClient(int epollFd, CircularBuffer *cb);

void HandleTimer(int timerFd);
void NewConnection(int socketFd, CircularBuffer* clientQueue);
void HandleNewPipeData();
void SendPortion(ClientData *currentCD, int pipeFd);

void DropConnection(ClientData *cd);

static Stats stats = {};

int main(int argc, char *argv[]){
    Parameters parameters = {0, DEFAULT_ADDRESS, 0};
    ParseParameters(argc, argv, &parameters);

    //Signals
    signal(SIGPIPE, SIG_IGN); //handle bad writes by myself, so ignore this signal
    SetupSIGCHLD(); //Handle, means quit on child error

    int pipeRead = SetupWarehouse(parameters.productionRate);
    int socketFd = SetupConection(&parameters);

    //Setup timer and epoll
    int timerFd = timerfd_create(CLOCK_MONOTONIC, 0);
    Check(timerFd, "timer_create(): ");

    int epollFd = SetupEpoll(socketFd, pipeRead, timerFd);
    
    stats.pipeCapacity = fcntl(pipeRead, F_GETPIPE_SZ);
    Check(stats.pipeCapacity, "fcntl(): ");

    CircularBuffer clientQueue = CreateCB(0);//could be any number, queue can grow, I choose 0 to assert dominance

    size_t reventsSize = EPOLL_INITIAL;
    struct epoll_event *revents = NULL;
    ExpandRevents(&revents, &reventsSize);

    while (1){

        if(stats.connectedClients + EPOLL_INITIAL > reventsSize){   //Not enough revents to accomodate all possible epoll fds
            ExpandRevents(&revents, &reventsSize);  //x2 + 1 expansion
        }
        //Update current pipe size
        int ioctlRes = ioctl(pipeRead, FIONREAD, &stats.pipeSize);
        Check(ioctlRes, "ioctl(): ");
        
        //Unshelve someone from the queue, one at a time, easy to change if needs be
        if (stats.pipeSize > DELIVERY_SIZE && clientQueue.size > 0){
            DispatchClient(epollFd, &clientQueue);
        }

        int epollRes = epoll_wait(epollFd, revents, reventsSize, 5);    //5 serves as a fail-safe mechanism, when the pipe is full and no other events appear
        Check(epollRes, "epoll(): ");

        for (int i = 0; i < epollRes; ++i){
            ClientData *currentCD = revents[i].data.ptr;
            uint32_t currentEvents = revents[i].events;

            if(currentCD->fd == timerFd){
                HandleTimer(timerFd);
            }
            else if (currentCD->fd == pipeRead){
                HandleNewPipeData();
            }
            else if (currentCD->fd == socketFd){
                NewConnection(socketFd, &clientQueue);
            }
            else if (currentEvents & EPOLLRDHUP){
                DropConnection(currentCD);
            }
            else if (currentEvents & EPOLLIN){
                SendPortion(currentCD, pipeRead); 
            }
            else{
                Error("epoll_wait(): ");
            }
        }
    }

    //Clean-up, optional(because Kernel will re-claim all resources anwyays)
    free(revents);
    ClearCB(&clientQueue);

    //Epoll problem: in epoll_event.data.ptr, I store dynamically allocated structure (because it's convenient)
    //Epoll offers no mechanisms to return all added elements, and so there's no way to free
    //all of allocated memory (easily), it's not a problem in this program, we quit and release all resources anyways
    //but it's something to keep in mind for the future
    //NOTE: I'm only talking about memory allocated for socketFd, pipeFd, timerFd, tests have shown that client code does not leak
    close(socketFd);
    close(pipeRead);
    close(timerFd);
    close(epollFd);

    return EXIT_SUCCESS;
}

void ParseParameters(int argc, char *argv[], Parameters *parameters){
    if (argv == NULL){
        fprintf(stderr, "ParseParameters(): argv is NULL\n");
        exit(EXIT_FAILURE);
    }
    if (parameters == NULL){
        fprintf(stderr, "ParseParameters(): parameters is NULL\n");
        exit(EXIT_FAILURE);
    }

    int opt;
    while ((opt = getopt(argc, argv, "p:")) != -1){
        switch (opt){
        case 'p':
            parameters->productionRate = ParseFloat(optarg);
            break;
        default:
            fprintf(stderr, "Usage: %s -p <float> [<addr>:]port\n", argv[0]);
            break;
        }
    }

    if (parameters->productionRate <= 0){
        fprintf(stderr, "ParseParameters(): -p <float> is mandatory and must be a positive, non-zero number.\n");
        exit(EXIT_FAILURE);
    }

    char *addressStr = NULL;
    for (int i = optind; i < argc; ++i){
        if (addressStr != NULL)
        {
            fprintf(stderr, "Too many arguments!\n");
            fprintf(stderr, "Usage: %s -p <float> [<addr>:]port\n", argv[0]);
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
    parameters->port = ParseULong(portStr);
    if (ipStr != NULL){
        parameters->address = ipStr;
    }
}

int SetupWarehouse(float productionRate){

    int pipeFds[2];
    int pipeRes = pipe(pipeFds);
    Check(pipeRes, "pipe(): ");

    int childPid = fork();
    Check(childPid, "fork(): ");

    if(childPid == 0){
        //Child
        signal(SIGPIPE, SIG_IGN);   //again, not inherited

        close(pipeFds[0]); //close Read end, child only writes
        WarehouseWorker(productionRate, pipeFds[1]);

        close(pipeFds[1]);
        exit(EXIT_SUCCESS);
    }

    //else, Parent
    close(pipeFds[1]);// close Write end, parent only reads

    return pipeFds[0];
}

void WarehouseWorker(float productionRate, int pipeWrite){
    //Working working working working....
    
    //production rate and sleep
    size_t sleepNs = BLOCK_SIZE * 1e9 / (productionRate * PRODUCTION_UNIT);
    struct timespec ts = {};
    ts.tv_sec = sleepNs / 1e9;
    ts.tv_nsec = sleepNs % (size_t)(1e9);

    const size_t pipeCapacity = fcntl(pipeWrite, F_GETPIPE_SZ);
    Check(pipeCapacity, "fcntl(): ");

    char currentChar = 'a';
    char data[BLOCK_SIZE] = {};
    while(1){
        //Generate Data
        for(size_t i = 0; i < BLOCK_SIZE; ++i){
            data[i] = currentChar;  
        }
        if(currentChar == 'z') currentChar = 'A';
        else if(currentChar == 'Z') currentChar = 'a';
        else ++currentChar;

        //Check if there is space inside of pipe
        //This step is entirely optional, just writing and blocking can be desirable
        int bytesLeft = 0;
        do {
            int ioctlRes = ioctl(pipeWrite, FIONREAD, &bytesLeft);
            Check(ioctlRes, "ioctl(): ");
            
            nanosleep(&ts, NULL);   //why not
        } while (pipeCapacity - bytesLeft < BLOCK_SIZE);
        
        //Write
        errno = 0;
        int writeRes = write(pipeWrite, data, BLOCK_SIZE);
        if(writeRes == 0 || (writeRes == -1 && errno == EPIPE))
            return; //connection has been severed, return without warning
        Check(writeRes, "write(): ");

        //Sleep
        nanosleep(&ts, NULL);
    }
}

int SetupConection(Parameters *parameters){
    //Setup AF_INET
    int socketFd = socket(AF_INET, SOCK_STREAM, 0);
    Check(socketFd, "socket(): ");

    struct sockaddr_in serverAddress = {0};
    serverAddress.sin_family = AF_INET;
    serverAddress.sin_port = htons(parameters->port);

    //parsing this leads to quite an ammusing situation
    //on error, inet_* functions, return 0, which is also INNADR_ANY
    //thinking that those functions can parse "localhost" into a proper address can lead to
    //false sense of "working program", when for example user sets the address to some random string i.e. "abcd"
    //the program will continue running as if nothing bad happened, when in my opinion, it should throw an error
    if (strcmp(parameters->address, "localhost") == 0)
        serverAddress.sin_addr.s_addr = inet_addr("127.0.0.1");
    else{
        int inetPtonRes = inet_pton(AF_INET, parameters->address, &serverAddress.sin_addr);
        if (inetPtonRes == 0){
            fprintf(stderr, "Invalid ip address.\n");
            exit(EXIT_FAILURE);
        }
    }

    int bindRes = bind(socketFd, (struct sockaddr *)&serverAddress, sizeof(serverAddress));
    Check(bindRes, "bind(): ");

    int listenRes = listen(socketFd, 1024); //TODO: determine this
    Check(listenRes, "listen(): ");

    return socketFd;
}

int SetupEpoll(int socketFd, int pipeFd, int timerFd){
    int epollFd = epoll_create1(0);
    struct epoll_event event = {};

    //Add listening socket
    event.data.ptr = CreateClientData(socketFd);
    event.events = EPOLLIN;
    int epollCtlRes = epoll_ctl(epollFd, EPOLL_CTL_ADD, socketFd, &event);
    Check(epollCtlRes, "epoll_ctl(ADD): ");

    //Listen for changes inside the pipe(warhouse)
    event.data.ptr = CreateClientData(pipeFd); //I think man says, when working with EPOLLET, this fd should be non-blocking, yet it works...
    event.events = EPOLLET | EPOLLIN | EPOLLOUT;
    epollCtlRes = epoll_ctl(epollFd, EPOLL_CTL_ADD, pipeFd, &event);
    Check(epollCtlRes, "epoll_ctl(ADD): ");

    struct timespec timerTs = {5, 0};   //hard coded, TODO: make a constant
    struct itimerspec timerSpec = {timerTs, timerTs};
    int timerSetRes = timerfd_settime(timerFd, 0, &timerSpec, NULL);
    Check(timerSetRes, "timerfd_settimer(): ");

    event.data.ptr = CreateClientData(timerFd); //I think man says, this fd should be non-blocking, yet it works...
    event.events = EPOLLIN | EPOLLET;
    epollCtlRes = epoll_ctl(epollFd, EPOLL_CTL_ADD, timerFd, &event);
    Check(epollCtlRes, "epoll_ctl(ADD): ");

    return epollFd;
}

void ExpandRevents(struct epoll_event **revents, size_t *size){
    if(revents == NULL || size == NULL){
        fprintf(stderr, "ExpandRevents(): null arguments\n");
        exit(EXIT_FAILURE);
    }

    if(*revents) free(*revents);
    *size = 2 * *size + 1; //+1 to counter starting size of 0
    *revents = calloc(*size, sizeof(struct epoll_event));
    if (*revents == NULL)
        Error("calloc(): ");
}

void DispatchClient(int epollFd, CircularBuffer *cb){
    Debug("Dispatching a client!\n");

    int clientFd = cb->data[cb->begin];
    PopCB(cb);

    struct epoll_event event = {};
    event.data.ptr = CreateClientData(clientFd);
    event.events = EPOLLIN | EPOLLRDHUP;
    int epollCtlRes = epoll_ctl(epollFd, EPOLL_CTL_ADD, clientFd, &event);
    Check(epollCtlRes, "epoll_ctl(ADD): ");
}

void HandleTimer(int timerFd){
    char trashBuf[64]; //empty the buffer, this should easily be enough
    int readRes = read(timerFd, trashBuf, sizeof(trashBuf));
    Check(readRes, "read(): ");

    fprintf(stderr, "\nConnected clients: %ld/%ld\n", stats.connectedClients, stats.lifetimeClients);
    fprintf(stderr, "Warehouse state: %ldB, %4f%%\n", stats.pipeSize, stats.pipeSize * 100.0 / stats.pipeCapacity);
    //pipeSize doubles as current throughput, MaterialGenerated = PipeSize + MaterialSent
    //Throughput = MaterialGenerated - MaterialSent, hence Throughput = PipeSize
    fprintf(stderr, "Throughput: %lld\n", (long long)stats.pipeSize - (long long)stats.lastThroughput);
    stats.lastThroughput = stats.pipeSize;  //pipeSize is always up-to-date, at this point
}

void NewConnection(int socketFd, CircularBuffer *clientQueue){
    //New connection
    Debug("Accepting...\n");
    ++stats.connectedClients;
    ++stats.lifetimeClients;

    struct sockaddr_in clientAddress;
    uint32_t clientSize = sizeof(clientAddress);
    int clientFd = accept(socketFd, (struct sockaddr *)&clientAddress, &clientSize);
    Check(clientFd, "client(): ");

    PushCB(clientQueue, clientFd);
}

void HandleNewPipeData(){
    //Even tough it does nothing by itself, it serves as a fail-safe mechanism
    //When we add a new client to the queue, and no other events happen, the program will continue
    //to block, and the client will be stuck inside the queue. This event gets triggered when new data
    //is supplied to the pipe, unblocking the program, checking the queue on next iteration and realeasing the client from the queue
}

void SendPortion(ClientData *currentCD, int pipeFd){
    //A word on request-based approach
    //From a clients' point of view, EPOLLIN and EPOLLOUT method pose no difference
    //For a server, with EPOLLOUT, all data will be sent essentially immediately, as there's usually space
    //left inside the send buffer, and so, it's next to impossible for the client to disconnect mid-transfer
    //and have server meassure wasted bytes, with EPOLLIN, each package sent is synchronised with each package received
    //making it easier to severe the connection during the transfer, other than that, both methods are equal
    //If it's required, switching back to EPOLLOUT approach is easy, replace all mentions of EPOLLIN with
    //EPOLLOUT, delete recv(client) code on server side, and write(serve) code on client side, literally around 30 seconds, both methods work

    //Got a request
    int recvRes = 0;
    char trashBuf[128] = {}; //no idea how to dispose of read data otherwise
    errno = 0;               //For the peace of mind, i reset errno every time im about to check it's value
    do{ //Empty the buffer
        recvRes = recv(currentCD->fd, trashBuf, sizeof(trashBuf), MSG_DONTWAIT);
    } while (recvRes > 0);
    if (recvRes <= 0 && (errno == EAGAIN || errno == EWOULDBLOCK)){
        errno = 0; //nothing wrong happened, buffer is empty, reset errno, keep going
    }
    else{
        return; //error occurend, let EPOLLRDHUP handle it
    }

    Debug("Emptied!\n");

    if (currentCD->dataSent == 0){
        //client is yet to send data, so get it from warehouse, (reserve it)
        //the fact that EPOLLIN got triggered and not EPOLLRDHUP means that we are still connected, thus we reserve the bytes for client,
        //also recv() hasn't failed, so we are double sure that client is still connected
        int dataRead = 0;
        int readRes = 0;
        do{
            readRes = read(pipeFd, currentCD->buffer + dataRead, sizeof(currentCD->buffer) - dataRead);
            dataRead += readRes;
        } while (dataRead < DELIVERY_SIZE && readRes > 0);
        if (readRes <= 0){
            //0 means disconnected, -1 means error
            //if pipe ( and also the child ) is broken, then SIGCHLD should appear and handle the problem
            return;
        }
    }

    //Send a single portion to the client
    int writeRes = 0;
    int dataWritten = 0;
    do{
        writeRes = write(currentCD->fd, currentCD->buffer + currentCD->dataSent + dataWritten, PORTION_SIZE - dataWritten);
        dataWritten += writeRes;
    } while (dataWritten < PORTION_SIZE && writeRes > 0);

    currentCD->dataSent += dataWritten;

    if (writeRes <= 0){
        return; //error happened
    }

    if (currentCD->dataSent == DELIVERY_SIZE){
        //We are done, disconnect
        DropConnection(currentCD);
    }
    else if (currentCD->dataSent > DELIVERY_SIZE){
        fprintf(stderr, "God must have forsaken me, for a byte or more have been sent with no consent.\n");
    }

    Debug("Finished writing!\n");
}

void DropConnection(ClientData *cd){
    Debug("Dropping....\n");
    --stats.connectedClients;
    //Report
    struct timespec timeStamp = {};
    clock_gettime(CLOCK_REALTIME, &timeStamp);
    fprintf(stderr, "\nTimestamp: %fs\n", timeStamp.tv_sec + timeStamp.tv_nsec / 1e9);

    struct sockaddr_in clientAddr = {};
    socklen_t clientAddrSize = sizeof(clientAddr);
    int getSockNameRes = getsockname(cd->fd, &clientAddr, &clientAddrSize);
    Check(getSockNameRes, "getsockname(): ");
    fprintf(stderr, "Address: %s: %d\n", inet_ntoa(clientAddr.sin_addr), ntohs(clientAddr.sin_port));

    size_t bytesWasted = (cd->dataSent == 0) ? 0 : DELIVERY_SIZE - cd->dataSent;
    fprintf(stderr, "Wasted bytes: %ld\n", bytesWasted);

    //int epollCtlRes = epoll_ctl(epollFd, EPOLL_CTL_DEL, cd->fd, NULL); //redundant, close automatically make epoll delete this FD
    //Check(epollCtlRes, "epoll_ctl(DEL): ");

    close(cd->fd);
    free(cd);
}

void SetupSIGCHLD(){
    //This signal is completely nescessary
    //when child process(warehouse) dies, ioctl() will not recognize that the pipe has been closed
    //And so the producent Parent will keep running, thinking that pipe is empty,
    //waiting until some data arrives, but it never will, being stuck forever
    //The forked warehouse does not exhibit this problem, it dies when parent dies, why?????
    struct sigaction sa;
    sigemptyset(&sa.sa_mask);
    sigfillset(&sa.sa_mask); //block everything, we're going to quit anyways so at least don't bother me
    sa.sa_handler = HandleSIGCHLD;
    int sigActionRes = sigaction(SIGCHLD, &sa, NULL);
    Check(sigActionRes, "sigaction(): ");
}

void HandleSIGCHLD(int code){
    int childCode = 0;
    pid_t childPid = wait(&childCode);
    //even though fprintf is not signal safe, at this moment all signals are being blocked
    fprintf(stderr, "Signal received: %d\n", code);
    fprintf(stderr, "Child process (%d) terminated with code %d. Quitting....\n", childPid, childCode);
    //I feel like this is not quite enough, should I also kill the child, just to make sure it's dead?
    exit(EXIT_FAILURE);
}

ClientData* CreateClientData(int fd){
    ClientData* cd = calloc(1, sizeof(ClientData));
    if(cd == NULL) Error("calloc(): ");

    cd->fd = fd;
    return cd;
}

CircularBuffer CreateCB(size_t size){
    CircularBuffer cb = {};
    if(size) {
        cb.data = calloc(size, sizeof(int));
        if(cb.data == NULL) Error("calloc(): ");
    }
    cb.capacity = size;
    return cb;
}

void PushCB(CircularBuffer* cb, int value){
    if(cb->size >= cb->capacity){ 
        //Expand
        int* newData = calloc(2 * cb->capacity +1, sizeof(int));
        if(newData == NULL) Error("calloc(): ");
        //Copy data
        if(cb->begin < cb->end){
            memcpy( newData + cb->begin, 
                    cb->data + cb->begin, 
                    (cb->end - cb->begin) * sizeof(int));
        }
        else{
            memcpy(newData, cb->data + cb->begin, (cb->size - cb->begin) * sizeof(int));
            memcpy(newData + cb->size - cb->begin, cb->data, cb->end * sizeof(int));
            cb->begin = 0;
            cb->end = cb->size;
        }
        free(cb->data);
        cb->data = newData;
        cb->capacity = cb->capacity * 2 + 1;
    }
    ++cb->size;
    cb->data[cb->end] = value;
    cb->end = NextCB(cb, cb->end);
}

void PopCB(CircularBuffer *cb){
    if(cb->size > 0){
        cb->begin = NextCB(cb, cb->begin);
        --cb->size;
    }
}

size_t NextCB(CircularBuffer* cb, size_t it){
    it = (it+1) % cb->capacity;

    return it;
}

void ClearCB(CircularBuffer* cb){
    free(cb->data);
    memset(cb, 0, sizeof(CircularBuffer));
}

void Error(const char *message){
    perror(message);
    exit(EXIT_FAILURE);
}

void Check(int value, const char *message){
    if (value == -1)
        Error(message);
}

size_t ParseULong(const char *arg){
    errno = 0;
    char *end = NULL;

    if(arg == NULL){
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

    if(arg == NULL){
        fprintf(stderr, "ParseFloat(): arg is NULL\n");
        exit(EXIT_FAILURE);
    }

    float val = strtof(arg, &end);
    if ((val == 0 && errno != 0) || *end != '\0')
        Error("strtof(): ");

    return val;
}