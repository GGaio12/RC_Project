/**********************************************************************
 * TCP CLIENT, links to the server (defined in argv[1]) in a specific 
 * port (in argv[2]). After connection the client is requested to login 
 * and then to send commands to the server to obtain some information 
 * from the server until they want to quit by send 'QUIT'.
 * USO: >class_client_tcp <TCP_SV_ADDRESS> <TCP_SV_PORT>
 **********************************************************************/
#include <stdio.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <netdb.h>
#include <signal.h>
#include <stdbool.h>

#define BUF_SIZE 1024        // size of the buffer used for send and recive messages
#define MAX_SUB_CLASSES 5    // Max number of multicast sockets
#define MULTICAST_PORT 9867

/* Functions prototipes for program suddenly close and/or error */
int create_socket(char* server_addr, char* server_port);
void close_socket();
void handle_sigint();
void error(char *msg);

typedef struct class {
    char* name;
    int socket;
    struct sockaddr_in addr;
};

typedef struct classes_sockets {
    struct class classes[MAX_SUB_CLASSES];
    int n_classes;
};

/* Initializing variables */
char buffer[BUF_SIZE];
char message[BUF_SIZE];
int nread;
bool loged_in;
struct classes_sockets class_sockets;

/* Socket used in tcp communication */
int fd;

/**
 * Main function.
 */
int main(int argc, char *argv[]) {
    /* Closes the open socket in C^ (SIGINT) case */
    signal(SIGINT, handle_sigint);

    /* Verifies if all the arguments were given in file execution */
    if(argc != 3) {
        printf("class_client <TCP_SV_ADDRESS> <TCP_SV_PORT> \n");
        exit(0);
    }

    /* Creates the socket structure and connects it to the server */
    fd = create_socket(argv[1], argv[2]);

    class_sockets.n_classes = 0;

    /* Reads everything server sends and send command to the server if loged in until 'QUIT' command */
    loged_in = false;
    do {
        nread = read(fd, buffer, BUF_SIZE-1);
        if(nread < 0) error("read function (nread < 0)");

        /* If the server send a message print it and act if necessary/wanted */
        if(nread > 0) {
            buffer[nread] = '\0';

            /* Prints what server send */
            puts(buffer);

            /* If the client isn't loged in the server, keeps trying to login until it is or until 'QUIT' */
            if(!loged_in) {
                if(strcmp(buffer, "Please enter your LOGIN credentials (LOGIN <username> <password>):") == 0) {
                    fgets(message, sizeof(message), stdin);
                    message[strcspn(message, "\n")] = 0;

                    if(write(fd, message, 1 + strlen(message)) == -1) {
                        error("sending login credentials message");
                    }

                    if(strcmp(message, "QUIT") == 0) break;
                }
                else if(strcmp(buffer, "OK") == 0) loged_in = true;  
            }
            /* Otherwise the server is waiting for a new command or answering */
            else if(strcmp(buffer, "Waiting new command...") == 0) {
                fgets(message, sizeof(message), stdin);
                message[strcspn(message, "\n")] = 0;

                if(write(fd, message, 1 + strlen(message)) == -1) error("sending new command message");

                if(strcmp(message, "QUIT") == 0) break;
                else if(strcmp(message, "LIST_CLASSES") == 0) continue;
                else if(strcmp(message, "LIST_SUBSCRIBED") == 0) process_list_subscribed();
                else {
                    char* token = strtok(message, " ");
                    if(token != NULL) {
                        if(strcmp(token, "SUBSCRIBE_CLASS") == 0) process_subscribe_class();
                        else if(strcmp(token, "CREATE_CLASS") == 0) process_create_class();
                        else if(strcmp(token, "SEND") == 0) process_send();
                    }
                }
            }
        }
    } while(1);
  
    close(fd); // closes the socket to end communication
    return 0;
}

/**
 * Creates a new tcp socket connected to the server by giving the server address and port. Returns the new socket.
 */
int create_socket(char* server_addr, char* server_port) {
    int s;
    char endServer[100];
    struct sockaddr_in addr;
    struct hostent *hostPtr;

    /* Creating the new socket structure */
    strcpy(endServer, server_addr);
    if((hostPtr = gethostbyname(endServer)) == 0) error("obtaining server address");

    bzero((void *) &addr, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = ((struct in_addr *)(hostPtr->h_addr))->s_addr;
    addr.sin_port = htons((short) atoi(server_port));

    /* Creating the socket */
    if((s = socket(AF_INET,SOCK_STREAM,0)) == -1) error("creating the socket");

    /* Connecting the socket to the server */
    if(connect(s,(struct sockaddr *)&addr,sizeof (addr)) < 0) error("connecting the socket");

    /* Returns the new socket */
    return s;
}

/**
 * Process LIST_SUBSCRIBED command servers answer.
 */
void process_list_subscribed() {
    do {
        nread = read(fd, buffer, BUF_SIZE-1);
        if(nread < 0) error("read function (nread < 0)");

        if(nread > 0) {
            buffer[nread] = '\0';

            puts(buffer);

            char* token = strtok(buffer, " ");
            if(strcmp(token, "CLASS") == 0) {
                char* name;
                char* ip;
                int i = 0;
                while((token = strtok(NULL, ", ")) != NULL) {
                    if(i >= MAX_SUB_CLASSES) break;
                    int j = 0;
                    while(token[j] != '/') j++;
                    strncpy(name, token, j);
                    strncpy(ip, token+j+1, strlen(token)-j-1);
                    add_class(name, ip);
                }
            }
        }
    } while(1);
}

/**
 * Process SUBSCRIBE_CLASS command servers answer.
 */
void process_subscribe_class() {

}

/**
 * Process CREATE_CLASS command servers answer.
 */
void process_create_class() {

}

/**
 * Process SEND command servers answer.
 */
void process_send() {

}

/**
 * Adds a new class with a specific name and in the specific IP.
 */
void add_class(char* name, char* addr_ip) {
    if(class_sockets.n_classes > MAX_SUB_CLASSES) return;

    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if(sock < 0) error("class socket creation");

    struct sockaddr_in addr;

    // set up the multicast address structure
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = inet_addr(addr_ip);
    addr.sin_port = htons(MULTICAST_PORT);

    // bind the socket to the port
    if(bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) error("binding class socket");

    // join the multicast group
    struct ip_mreq mreq;
    mreq.imr_multiaddr.s_addr = inet_addr(addr_ip);
    mreq.imr_interface.s_addr = INADDR_ANY;
    if(setsockopt(sock, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq)) < 0) error("seting class socket options");
    
    struct class newClass;
    strcpy(newClass.name, name);
    newClass.addr = addr;
    newClass.socket = sock;
    
    class_sockets.classes[class_sockets.n_classes] = newClass;
    class_sockets.n_classes++;
}

/*******************************************************
 *
 *                    SYSTEM MANAGE
 * 
 *******************************************************/

/**
 * Closes the socket created if it exists.
 */
void close_socket() {
    if(fd) close(fd);
}

/**
 * Handles SIGINT signal closing and freeing all the resorces.
 */
void handle_sigint() {
    close_socket();

    exit(0);
}

/**
 * Prints an error message and exists after closes the socket if it isn't closed yet.
 */
void error(char *msg){
    close_socket();

    printf("Error: %s\n", msg);
    exit(-1);
}

