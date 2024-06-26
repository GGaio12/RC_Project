/**********************************************************************
 * UDP CLIENT, links to the server (defined in argv[1]) in a specific 
 * port (in argv[2]). After connection the client is requested to login 
 * and then to send commands to the server to obtain some information 
 * from the server until they want to quit by send 'QUIT' or ending
 * server by doing 'QUIT_SERVER'.
 * USO: >class_client_udp <SV_ADDRESS> <UDP_SV_PORT>
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

#define BUF_SIZE 1024 // size of the buffer used for send and recive messages

/* Socket used in tcp communication */
int fd;
int nread;

struct sockaddr_in addr;
socklen_t addr_size = sizeof(addr);

/* UDP functions */
int create_socket(char* server_addr, char* server_port);
void process_list();

/* Functions prototipes for program suddenly close and/or error */
void close_socket();
void handle_sigint();
void error(char *msg);

/**
 * Main function.
 */
int main(int argc, char *argv[]) {
    /* Closes the open socket in C^ (SIGINT) case */
    signal(SIGINT, handle_sigint);

    /* Initializing variables to send and receive messages */
    char buffer[BUF_SIZE];
    char message[BUF_SIZE];
    bool loged_in; // control variable

    /* Verifies if all the arguments were given in file execution */
    if(argc != 3) {
        printf("class_client <SV_ADDRESS> <UDP_SV_PORT> \n");
        exit(EXIT_SUCCESS);
    }

    /* Creates the socket structure and connects it to the server */
    fd = create_socket(argv[1], argv[2]);

    if(sprintf(message, "Connecting message...") < 0) error("creating connecting message");
    if(sendto(fd, message, strlen(message), 0, (struct sockaddr*)&addr, addr_size) == -1) error("sending connecting message");

    /* Reads everything server sends and send command to the server if loged in until 'QUIT' command */
    loged_in = false;
    do {
        nread = recvfrom(fd, buffer, BUF_SIZE-1, 0, (struct sockaddr*)&addr, &addr_size);
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

                    if(sendto(fd, message, strlen(message), 0, (struct sockaddr*)&addr, addr_size) == -1) error("sending login credentials message");

                    if(strcmp(message, "QUIT") == 0) break;
                }
                else if(strcmp(buffer, "OK") == 0) loged_in = true;
            }
            /* Otherwise the server is waiting for a new command, so the client sends a new command to the server or 'QUIT' */
            else if(strcmp(buffer, "Waiting new command...") == 0) {
                fgets(message, sizeof(message), stdin);
                message[strcspn(message, "\n")] = 0;

                if(sendto(fd, message, strlen(message), 0, (struct sockaddr*)&addr, addr_size) == -1) error("sending new command message");

                if((strcmp(message, "QUIT") == 0) || (strcmp(message, "QUIT_SERVER") == 0)) break;
                else if(strcmp(message, "LIST") == 0) process_list();
            }
        }
    } while(true);
    
    close(fd); // closes the socket to end communication
    return 0;
}

/**
 * Creates a new udp socket connected to the server by giving the server address and port. Returns the new socket.
 */
int create_socket(char* server_addr, char* server_port) {
  int s;
  char endServer[100];
  struct hostent *hostPtr;

  /* Creating the new socket structure */
  strcpy(endServer, server_addr);
  if((hostPtr = gethostbyname(endServer)) == 0) error("obtaining server address");

  bzero((void *) &addr, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = ((struct in_addr *)(hostPtr->h_addr))->s_addr;
  addr.sin_port = htons((short) atoi(server_port));

  /* Creating the socket */
  if((s = socket(AF_INET,SOCK_DGRAM,0)) == -1) error("creating the socket");

  /* Returns the new socket */
  return s;
}

/**
 * Process LIST command servers answer.
 */
void process_list() {
    char big_buffer[2*BUF_SIZE];
    do {
        nread = recvfrom(fd, big_buffer, BUF_SIZE-1, 0, (struct sockaddr*)&addr, &addr_size);
        if(nread < 0) error("read function (nread < 0)");

        if(nread > 0) {
            big_buffer[nread] = '\0';

            /* Prints the list sent */
            puts(big_buffer);

            break;
        }
    } while(true);
}

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

    exit(EXIT_SUCCESS);
}

/**
 * Prints an error message and exists after closes the socket if it isn't closed yet.
 */
void error(char *msg) {
    close_socket();

    printf("Error: %s\n", msg);
    exit(EXIT_FAILURE);
}

