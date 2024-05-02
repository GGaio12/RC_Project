/*******************************************************************************
 * SERVER, waiting new clients TCP and/or UDP to connect in ports defined in
 * argv[1] and argv[2] respectively and with a configuration of all user names
 * passwords and user types defined in a configuraion file (defined in argv[3]).
 * When clients appear, they are requested to login and, if they match a user
 * name and password, they can do every command their type can do by sending
 * the command to the server.
 * USE: >class_server <CLASS_PORT> <CONFIG_PORT> <CONFIG_FILE_NAME>
 *******************************************************************************/
#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <netdb.h>
#include <string.h>
#include <strings.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <arpa/inet.h>
#include <errno.h>
#include <signal.h>
#include <sys/select.h>
#include <sys/shm.h>
#include <sys/ipc.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <semaphore.h>
#include <stdbool.h>

#define BUF_SIZE 1024 // size of the buffer used for send and recive messages
#define MAX_CLASSES 5
#define MULTICAST_PORT 9867
#define BASE_MULTICAST_IP "239.0.0."
#define SHM_PATH "./tmp/classes_shm"
#define BIN_SEM_PATH "./tmp/mutex"

/* Functions prototipes for tcp */
int create_tcp_server(int TCP_SV_PORT, int LISTEN_NUMS);
void request_login_tcp(int client_fd);
void process_tcp_client(int client_fd);
char* list_classes();
char* list_subscribed();
char* subscribe_class(char* name);
char* create_class(char* name, char* size);
void send_text(char* name, char* text);
int create_multicast_socket();
struct sockaddr_in create_multicast_addr(char* multicast_ip);


/* Functions prototipes for udp */
int create_udp_server(int UDP_SV_PORT);
void request_login_udp(int udp_fd, struct sockaddr_in client_addr, socklen_t client_addr_size);
void process_udp_client(int udp_fd);

/* Functions prototipes for program suddenly close and/or error */
void free_credential_vars();
void close_sockets();
void close_shm();
void free_all_resources();
void handle_sigint();
void error(char *msg);

typedef struct Class {
    char* name;
    char ip_address[16];
    int socket;
    struct sockaddr_in addr;
    int max_n_members;
    int n_members;
    char** members_user_names;
};

typedef struct shmStruct {
    struct Class classes[MAX_CLASSES];
    int n_classes;
}shm;

/* Server main process PID */
pid_t SV_PID;

/* Important variables given, in the start of the program, by main arguments */
char* CONFIG_FILE_NAME;
int TCP_SV_PORT;
int UDP_SV_PORT;

/* Sockets used in tcp and udp, client and server */
int tcp_client, tcp_fd, udp_s;
bool tcp_fdCreated = false, udp_sCreated = false;

/* Variables that will store the user information for future use if need */
char* USER_NAME;
char* USER_PASSWORD;
char* USER_TYPE;

shm* shm_ptr;
int shm_size = sizeof(shm);
int shm_fd;
bool shm_fdOpened = false, classesMaped = false;

sem_t* mutex;
bool mutexCreated = false;

/**
 * Main function.
 */
int main(int argc, char* argv[]) {
    /* Verifies if all the arguments were given in file execution */
    if(argc != 4) {
        printf("class_server <CLASS_PORT> <CONFIG_PORT> <CONFIG_FILE_NAME>");
        exit(EXIT_FAILURE);
    }

    SV_PID = getpid();

    /* Save port numbers for server and config file name */
    TCP_SV_PORT = atoi(argv[1]);
    UDP_SV_PORT = atoi(argv[2]);
    CONFIG_FILE_NAME = argv[3];

    USER_NAME = (char*) malloc(sizeof(char)*30);
    USER_PASSWORD = (char*) malloc(sizeof(char)*30);
    USER_TYPE = (char*) malloc(sizeof(char)*20);

    fd_set read_set;

    /* Closes open sockets and frees all memories in C^ (SIGINT) case */
    signal(SIGINT, handle_sigint);

    /* Create a new shared memory object and maping it into address space of the process */
    if((shm_fd = shm_open(SHM_PATH, O_CREAT | O_RDWR, 0666)) == -1) error("Opening shared memory");
    shm_fdOpened = true;
    if(ftruncate(shm_fd, shm_size) == -1) error("Ftruncate shared memory");
    shm_ptr = mmap(NULL, shm_size, PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
    if(shm_ptr == MAP_FAILED) error("Mapping shared memory");
    classesMaped = true;
    shm_ptr->n_classes = 0;

    /* Initializing message queue named semaphore */
    mutex = sem_open(BIN_SEM_PATH, O_CREAT, 0666, 1);
    if(mutex == SEM_FAILED) error("Opening binary semaphore (mutex)");
    mutexCreated = true;


    /******************** TODO --> read file with shared memory saved if exits to update in memory *****************/


    /* Client socket address creation */
    struct sockaddr_in client_addr;
    int client_addr_size = sizeof(client_addr);

    /* Creates the servers sockets used in tcp and udp */
    tcp_fd = create_tcp_server(TCP_SV_PORT, 10);
    tcp_fdCreated = true;
    udp_s = create_udp_server(UDP_SV_PORT);
    udp_sCreated = true;

    /* Clear the descriptor set */
    FD_ZERO(&read_set); 
    
    /* Gets the max fd between the tcp and udp */
    int maxfdp = tcp_fd; 
    if(maxfdp < udp_s) maxfdp = udp_s;
    maxfdp++;

    /* Starts lisning and process all informations that were received in the two sockets */
    while(1) {
        /* Sets the tcp and udp fds in the read set */
        FD_SET(tcp_fd, &read_set);
        FD_SET(udp_s, &read_set);

        /* Selects the ready descriptor (the socket with information in) */
        int nready = select(maxfdp, &read_set, NULL, NULL, NULL);

        /* If there is information in the tcp socket it accept the connection */
        if(FD_ISSET(tcp_fd, &read_set)) {
            tcp_client = accept(tcp_fd,(struct sockaddr *)&client_addr,(socklen_t *)&client_addr_size);

            /* Creates a child process to process the communication with the user */
            if(fork() == 0) {
                close(tcp_fd);
                process_tcp_client(tcp_client);
                exit(EXIT_SUCCESS);
            }
        }

        /* If there is information in the udp socket it receive and process the communication */
        if(FD_ISSET(udp_s, &read_set)) {
            process_udp_client(udp_s);
        }
    }

    return 0;
}


/*******************************************************
 *
 *                      TCP CLIENTS
 * 
 *******************************************************/

/**
 * Creates and returns a new tcp server socket in port 'TCP_SV_PORT' with 'LISTEN_NUMS' number of max listeners simultaneously.
 */
int create_tcp_server(int TCP_SV_PORT, int LISTEN_NUMS) {
  int fd;
  struct sockaddr_in addr;

  /* Filling up the socket address structure */
  bzero((void *) &addr, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  addr.sin_port = htons(TCP_SV_PORT);

  /* Creating socket */
  if((fd = socket(AF_INET, SOCK_STREAM, 0)) < 0) error("socket function (fd < 0)");

  /* Binding the socket with the address info */
  if(bind(fd,(struct sockaddr*)&addr,sizeof(addr)) < 0) error("bind function");

  /* Setting the max number of simultaneous listeners */
  if(listen(fd, LISTEN_NUMS) < 0) error("listen function");

  /* Returns the socket */
  return fd;
}

/**
 * Requests and wait for a correct login by the command LOGIN <username> <password> of a tcp client.
 */
void request_login_tcp(int client_fd) {
  /* Initializations */
  int nread;
  FILE* configFile;
  short checking = 1;
  short in_file;
  char buffer[BUF_SIZE];
  char authentication[BUF_SIZE];
  char message[BUF_SIZE];
  char* token;

  /* Message that asks the user about the login credentials */
  const char login_message[] = "Please enter your LOGIN credentials (LOGIN <username> <password>):";

  /* Loop that ends when the login was successfully done or the user quit */
  while(checking) {
    /* Writes the login message */
    if(write(client_fd, login_message, 1 + strlen(login_message)) == -1) {
      error("sending login message");
    }

    /* Loop that starts reading until the client send the login command */
    do {
      nread = read(client_fd, buffer, BUF_SIZE-1);
      if(nread < 0) error("read function (nread < 0)");

      /* If the client sends anything to the server */
      if(nread > 0) {
	      buffer[nread] = '\0';

        /* If client send 'QUIT' ends the communication */
        if(strcmp(buffer, "QUIT") == 0) {
          checking = 0;
          break;
        }

        /* Checking if the 'LOGIN' command was written */
        token = strtok(buffer, " ");
        if(strcmp(token, "LOGIN") != 0) {
          if(sprintf(message, "%s is not the valid commaSnd, to login please use: LOGIN <username> <password>", token) < 0) {
            error("creating login invalid command message");
          }
          if(write(client_fd, message, 1 + strlen(message)) == -1) {
            error("sending login invalid command message");
          }
          break;
        }

        /* Checking if the user name was written and save it in 'USER_NAME' global variable */
        token = strtok(NULL, " ");
        if(token == NULL) {
          if(sprintf(message, "Please insert the user name to login: LOGIN <username> <password>") < 0) {
            error("creating login invalid user name message");
          }
          if(write(client_fd, message, 1 + strlen(message)) == -1) {
            error("sending login invalid user name message");
          }
          break;
        }
        else USER_NAME = token;

        /* Checking if the password was written and save it in 'USER_PASSWORD' global variable */
        token = strtok(NULL, " ");
        if(token == NULL) {
          if(sprintf(message, "Please insert the password to login: LOGIN <username> <password>") < 0) {
            error("creating login invalid password message");
          }
          if(write(client_fd, message, 1 + strlen(message)) == -1) {
            error("sending login invalid password message");
          }
          break;
        }
        else USER_PASSWORD = token;
        
        /* Checks if the user with password is in the config file. Sends 'OK' if it's and 'REJECTED' if not */
        in_file = 0;
        if((configFile = fopen(CONFIG_FILE_NAME, "r")) == NULL) {
          error("opening the config file");
        }
        while(fgets(authentication, BUF_SIZE, configFile)) {
          token = strtok(authentication, ";");

          if(strcmp(token, USER_NAME) == 0) {
            token = strtok(NULL, ";");

            if(strcmp(token, USER_PASSWORD) == 0) {
              USER_TYPE = strtok(NULL, ";");
              if(sprintf(message, "OK") < 0) {
                fclose(configFile);
                error("creating 'OK' login message");
              }
              if(write(client_fd, message, 1 + strlen(message)) == -1) {
                fclose(configFile);
                error("sending 'OK' login message");
              }
              in_file = 1;
              checking = 0;
              break;
            }
          }
        }
        fclose(configFile);  // closes the file

        /* In case the user name and or password aren't correct */
        if(!in_file) {
          if(sprintf(message, "REJECTED") < 0) {
            error("creating 'REJECTED' login message");
          }
          if(write(client_fd, message, 1 + strlen(message)) == -1) {
            error("sending 'REJECTED' login message");
          }
        }
        break;
      }
    } while(1);
  }
}

/**
 * Process the tcp client requesting a login and then reading the commands sent.
 */
void process_tcp_client(int client_fd) {
  int nread;
  char buffer[BUF_SIZE];
  char message[BUF_SIZE + 18];
  short waiting_commands = 1;

  /* Requests the login first */
  request_login_tcp(client_fd);

  /* Reads all the commands sent. 'QUIT' to end the communication */
  do {
    /* Sends a message to the client for he to know the server is waiting a new command */
    if(sprintf(message, "Waiting new command...") < 0) {
      error("creating waitng new command message");
    }
    if(write(client_fd, message, 1 + strlen(message)) == -1) {
      error("sending waitng new command message");
    }

    /* Wait for the client to send a new command and process it. If command is 'QUIT' ends the comunication */
    do {
      nread = read(client_fd, buffer, BUF_SIZE-1);
      if(nread < 0) error("read function (nread < 0)");

      if(nread > 0) {
        buffer[nread] = '\0';

        /* Command identification */
        if(strcmp(buffer, "QUIT") == 0) {
            waiting_commands = 0;
            break;
        }
        else if(strcmp(buffer, "LIST_CLASSES") == 0) {
            if(sprintf(message, list_classes()) < 0) error("creating list_classes message");
        }
        else if(strcmp(buffer, "LIST_SUBSCRIBED") == 0) {
            if(sprintf(message, list_subscribed()) < 0) error("creating list_subscribed message");
        }
        else {
            char* token = strtok(buffer, " ");
            char* name;
            if(strcmp(token, "SUBSCRIBE_CLASS") == 0) {
                token = strtok(NULL, " ");
                if(token == NULL || strtok(NULL, " ") != NULL) {
                    if(sprintf(message, "Invalid command. USE: SUBSCRIBE_CLASS {name}") < 0) error("creating invalid command message");
                }
                else {
                    if(sprintf(message, subscribe_class(token)) < 0) error("creating subscribe_class message");
                }
            }
            else if(strcmp(token, "CREATE_CLASS") == 0 && strcmp(USER_TYPE, "professor") == 0) {
                if(token == NULL) {
                    if(sprintf(message, "Invalid command. USE: CREATE_CLASS {name} {size}") < 0) error("creating invalid command message");
                }
                else {
                    strcpy(name, token);

                    token = strtok(NULL, " ");
                    if(token == NULL || strtok(NULL, " ") != NULL) {
                        if(sprintf(message, "Invalid command. USE: CREATE_CLASS {name} {size}") < 0) error("creating invalid command message");
                    }
                    else {
                        if(sprintf(message, create_class(name, token)) < 0) error("creating create_class message");
                    }
                }
            }
            else if(strcmp(token, "SEND") == 0 && strcmp(USER_TYPE, "professor") == 0) {
                char* text;
                if(token == NULL) {
                    if(sprintf(message, "Invalid command. USE: SEND {name} {text to send}") < 0) error("creating invalid command message");
                }
                else {
                    strcpy(name, token);

                    if(strtok(NULL, " ") == NULL) {
                        if(sprintf(message, "Invalid command. USE: SEND {name} {text to send}") < 0) error("creating invalid command message");
                    }
                    else {
                        strncpy(text, buffer + strlen("SEND "), strlen(buffer) - strlen("SEND "));
                        send_text(name, text);
                        break;
                    }
                }
            }
            else strcpy(message, "Message not recognized");
        }

        /* Sending message generated by the command */
        if(write(client_fd, message, 1 + strlen(message)) == -1) error("sending message");

        break;
      }
    } while(1);
  } while(waiting_commands);
  close(client_fd); // closes the client socket
}

/**
 * Creates and returns a message that indicates all avaiable classes.
 */
char* list_classes() {
    char* message;
    strcpy(message, "CLASS ");

    int i = 0;
    sem_wait(mutex);
    /* Going throught all created classes in shared memory */
    while(i < shm_ptr->n_classes) {
        if(i != 0) strcat(message, ", ");
        strcat(message, shm_ptr->classes[i].name);
        i++;
    }
    sem_post(mutex);

    if(i == 0) strcpy(message, "NO CLASSES CREATED");
    return message;
}

/**
 * Creates and returns a message that indicates all classes that the client is subscribed.
 */
char* list_subscribed() {
    bool sub;
    char* message;
    strcpy(message, "CLASS ");
    
    int i = 0;
    int j = 0;
    sem_wait(mutex);
    /* Going throught all created classes in shared memory */
    while(i < shm_ptr->n_classes) {
        sub = false;
        /* Verifies if the student is in the class */
        for(int j = 0; j < shm_ptr->classes[i].n_members; j++) {
            if(strcmp(shm_ptr->classes[i].members_user_names[j], USER_NAME) == 0) {
                sub = true;
                break;
            }
        }

        /* If the student is in the class, adds the class name and multicast ip address to the message */
        if(sub) {
            if(i != 0) strcat(message, ", ");
            strcat(message, shm_ptr->classes[i].name);
            strcat(message, "/");
            strcat(message, shm_ptr->classes[i].ip_address);
            j++;
        }
        i++;
    }
    sem_post(mutex);

    if(i == 0) strcpy(message, "NO CLASSES CREATED");
    else if(j == 0) strcpy(message, "NO CLASSES SUBSCRIBED");
    return message;
}

/**
 * Susbscribe a class with a specific 'name' and return a message 'ACCEPTED {multicast}' or 'REJECTED'.
 */
char* subscribe_class(char* name) {
    char* message;

    bool found = false;
    bool already_in = false;
    int i = 0;
    sem_wait(mutex);
    /* Going throught all created classes in shared memory */
    while(i < shm_ptr->n_classes) {
        /* Verifies if the class have the same name wanted */
        if(strcmp(shm_ptr->classes[i].name, name) == 0) {
            found = true;

            /* Verifing if user is already registed in class */
            for(int j = 0; j < shm_ptr->classes[i].n_members; j++) {
                if(strcmp(shm_ptr->classes[i].members_user_names[j], USER_NAME) == 0) {
                    already_in = true;
                    break;
                }
            }
            if(already_in) {
                i++;
                break;
            }

            /* Verifies if the class still have space for new members */
            if(shm_ptr->classes[i].n_members < shm_ptr->classes[i].max_n_members) {
                /* Adds the student user name to the members user name */
                strcpy(shm_ptr->classes[i].members_user_names[shm_ptr->classes[i].n_members], USER_NAME);
                shm_ptr->classes[i].n_members++;

                strcpy(message, "ACCEPTED ");
                strcat(message, shm_ptr->classes[i].ip_address);
            }
            else strcpy(message, "REJECTED");
            i++;
            break;
        }
        i++;
    }
    sem_post(mutex);

    if(i == 0) strcpy(message, "NO CLASSES CREATED");
    else if(!found) strcpy(message, "CLASS NOT FOUND");
    else if(already_in) strcpy(message, "ALREADY SUBSCRIBED IN CLASS");
    return message;
}

/**
 * Creates a new class with a specific 'name' and 'size' and return a message 'OK {multicast}'.
 */
char* create_class(char* name, char* size) {
    char* message;

    sem_wait(mutex);
    /* Verifies if there are space to create a new class */
    if(shm_ptr->n_classes >= MAX_CLASSES) strcpy(message, "REJECTED: MAX CLASSES REACHED");
    else {
        /* Creating a new class */
        struct Class newClass;
        char newIP[16] = strcat(BASE_MULTICAST_IP, itoa((shm_ptr->n_classes + 1)));
        strcpy(newClass.name, name);                        // Class name
        strcpy(newClass.ip_address, newIP);                 // Multicast IP

        newClass.socket = create_multicast_socket();        // Multicast socket
        newClass.addr = create_multicast_addr(newIP);       // Multicast address

        newClass.max_n_members = atoi(size);                // Class max number of members
        newClass.n_members = 1;                             // Adding the professor to the number of members
        strcpy(newClass.members_user_names[0], USER_NAME);  // Adding the professor user name to the members user names

        /* Adding the new class to the classes in shared memory */
        shm_ptr->classes[shm_ptr->n_classes] = newClass;
        shm_ptr->n_classes++;

        strcpy(message, "OK ");
        strcat(message, newIP);
    }
    sem_post(mutex);

    return message;
}

/**
 * Sends a message to a class with a specific 'name'.
 */
void send_text(char* name, char* text) {
    int class_socket;
    struct sockaddr_in class_addr;

    int i = 0;
    sem_wait(mutex);
    /* Going throught all created classes in shared memory */
    while(i < shm_ptr->n_classes) {
        if(strcmp(shm_ptr->classes[i].name, name) == 0) {
            class_socket = shm_ptr->classes[i].socket;
            class_addr = shm_ptr->classes[i].addr;
            break;
        }
        i++;
    }
    sem_post(mutex);

    /* Sending muticast message */
    if(sendto(class_socket, text, 1 + strlen(text), 0, (struct sockaddr *)&class_addr, sizeof(class_addr)) < 0) {
        error("sending multicast message");
    }
}

/**
 * Creates a new multicast socket in a given multicast address
 */
int create_multicast_socket() {
    int sock;
    
    /* Creating the socket */
    sock = socket(AF_INET, SOCK_DGRAM, 0);
    if(sock < 0) error("Socket creation failed");

    return sock;
}

/**
 * Creates the address structure for a multicast socket with a given multicast ip.
 */
struct sockaddr_in create_multicast_addr(char* multicast_ip) {
    struct sockaddr_in addr;

    /* Filling the multicast address details */
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = inet_addr(multicast_ip);
    addr.sin_port = htons(MULTICAST_PORT);

    return addr;
}


/*******************************************************
 *
 *                      UDP CLIENTS
 * 
 *******************************************************/

/**
 * Creates and returns a new udp server socket in port 'UDP_SV_PORT'.
*/
int create_udp_server(int UDP_SV_PORT) {
    int udp_fd;
    struct sockaddr_in udp_addr;

    /* Filling up the socket address structure */
    bzero((void *) &udp_addr, sizeof(udp_addr));
    udp_addr.sin_family = AF_INET;
    udp_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    udp_addr.sin_port = htons(UDP_SV_PORT);

    /* Creating UDP socket */
    udp_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if(udp_fd < 0) error("socket function (udp_fd < 0)");

    /* Binding the socket with the address info */
    if(bind(udp_fd, (struct sockaddr*)&udp_addr, sizeof(udp_addr)) < 0) error("bind function");

    return udp_fd;
}

/**
 * Requests and wait for a correct login by the command LOGIN <username> <password> of a udp client.
 */
void request_login_udp(int udp_fd, struct sockaddr_in client_addr, socklen_t client_addr_size) {
    int nread;
    FILE* configFile;
    short checking = 1;
    short in_file;
    char buffer[BUF_SIZE];
    char authentication[BUF_SIZE];
    char message[BUF_SIZE];
    char* token;

    /* Message that asks the user about the login credentials */
    const char login_message[] = "Please enter your LOGIN credentials (LOGIN <username> <password>):";

    /* Loop that ends when the login was successfully done or the user quit */
    while(checking) {
      /* Writes the login message */
      if(sendto(udp_fd, login_message, strlen(login_message), 0, (struct sockaddr*)&client_addr, client_addr_size) == -1) {
        error("sending login message");
      }

      /* Loop that starts reading until the client send the login command */
      do {
        nread = recvfrom(udp_fd, buffer, BUF_SIZE-1, 0, (struct sockaddr*)&client_addr, &client_addr_size);
        if(nread < 0) error("read function (nread < 0)");

        if(nread > 0) {
          buffer[nread] = '\0';

          /* If client send 'QUIT' ends the communication */
          if(strcmp(buffer, "QUIT") == 0) {
            checking = 0;
            break;
          }

          /* Checking if the 'LOGIN' command was written */
          token = strtok(buffer, " ");
          if(strcmp(token, "LOGIN") != 0) {
            if(sprintf(message, "%s is not the valid command, to login please use: LOGIN <username> <password>", token) < 0) {
              error("creating login invalid command message");
            }
            if(sendto(udp_fd, message, strlen(message), 0, (struct sockaddr*)&client_addr, client_addr_size) == -1) {
              error("sending login invalid command message");
            }
            break;
          }

          /* Checking if the user name was written and save it in 'USER_NAME' global variable */
          token = strtok(NULL, " ");
          if(token == NULL) {
            if(sprintf(message, "Please insert the user name to login: LOGIN <username> <password>") < 0) {
              error("creating login invalid user name message");
            }
            if(sendto(udp_fd, message, strlen(message), 0, (struct sockaddr*)&client_addr, client_addr_size) == -1) {
              error("sending login invalid user name message");
            }
            break;
          }
          else USER_NAME = token;

          /* Checking if the password was written and save it in 'USER_PASSWORD' global variable */
          token = strtok(NULL, " ");
          if(token == NULL) {
            if(sprintf(message, "Please insert the password to login: LOGIN <username> <password>") < 0) {
              error("creating login invalid password message");
            }
            if(sendto(udp_fd, message, strlen(message), 0, (struct sockaddr*)&client_addr, client_addr_size) == -1) {
              error("sending login invalid password message");
            }
            break;
          }
          else USER_PASSWORD = token;
            
          /* Checks if the user with password is in the config file. Sends 'OK' if it's and 'REJECTED' if not */
          in_file = 0;
          if((configFile = fopen(CONFIG_FILE_NAME, "r")) == NULL) {
            error("opening the config file");
          }
          while(fgets(authentication, BUF_SIZE, configFile)) {
            token = strtok(authentication, ";");

            if(strcmp(token, USER_NAME) == 0) {
              token = strtok(NULL, ";");

              if(strcmp(token, USER_PASSWORD) == 0) {
                USER_TYPE = strtok(NULL, ";");
                if(sprintf(message, "OK") < 0) {
                  fclose(configFile);
                  error("creating 'OK' login message");
                }
                if(sendto(udp_fd, message, strlen(message), 0, (struct sockaddr*)&client_addr, client_addr_size) == -1) {
                  fclose(configFile);
                  error("sending 'OK' login message");
                }
                in_file = 1;
                checking = 0;
                break;
              }
            }
          }
          fclose(configFile);  // closes the file

          /* In case the user name and or password aren't correct */
          if(!in_file) {
            if(sprintf(message, "REJECTED") < 0) {
              error("creating 'REJECTED' login message");
            }
            if(sendto(udp_fd, message, strlen(message), 0, (struct sockaddr*)&client_addr, client_addr_size) == -1) {
              error("sending 'REJECTED' login message");
            }
          }
          break;
        }
      } while(1);
    }
}

/**
 * Process the udp client requesting a login and then reading the commands sent.
 */
void process_udp_client(int udp_fd) {
  struct sockaddr_in client_addr;
  socklen_t client_addr_size = sizeof(client_addr);
  int nread;
  char buffer[BUF_SIZE];
  char message[BUF_SIZE];
  short waiting_commands = 1;

  nread = recvfrom(udp_fd, buffer, BUF_SIZE-1, 0, (struct sockaddr*)&client_addr, &client_addr_size);
  if(nread < 0) error("read function (nread < 0)");

  if(nread > 0) {
    buffer[nread] = '\0';

    if(strcmp(buffer, "Connecting message...") != 0) error("connecting message not correct");
  }
    
  /* Requests the login first */
  request_login_udp(udp_fd, client_addr, client_addr_size);

  /* Reads all the commands sent. 'QUIT' to end the communication */
  do {
    /* Sends a message to the client for he to know the server is waiting a new command */
    if(sprintf(message, "Waiting new command...") < 0) {
      error("creating waitng new command message");
    }
    if(sendto(udp_fd, message, strlen(message), 0, (struct sockaddr*)&client_addr, client_addr_size) == -1) {
      error("sending waitng new command message");
    }
    
    /* Wait for the client to send a new command and process it. If command is 'QUIT' ends the comunication */
    do {
      nread = recvfrom(udp_fd, buffer, BUF_SIZE-1, 0, (struct sockaddr*)&client_addr, &client_addr_size);
      if(nread < 0) error("read function (nread < 0)");

      if(nread > 0) {
        buffer[nread] = '\0';

        printf("Command %s received", buffer);
        fflush(stdout);

        if(strcmp(buffer, "QUIT") == 0) waiting_commands = 0;

        break;
      }
    } while(1);
  } while(waiting_commands);
}


/*******************************************************
 *
 *                     SYSTEM MANAGE
 * 
 *******************************************************/

/**
 * Liberates all dynamic memories globally created.
 */
void free_credential_vars() {
  free(USER_NAME);
  free(USER_PASSWORD);
  free(USER_TYPE);
}

/**
 * Closes all the sockets created if they exist.
 */
void close_sockets() {
  if(tcp_fdCreated) close(tcp_fd);
  if(udp_sCreated) close(udp_s);
  sem_wait(mutex);
  for(int i = 0; i < shm_ptr->n_classes; i++) {
    close(shm_ptr->classes[i].socket);
  }
  sem_post(mutex);
}

/**
 * Closes the shared memory.
 */
void close_shm() {
    if(classesMaped) {
        /* Unmaping shared memory */
        if(munmap(shm_ptr, shm_size) == -1) {
            perror("SHM unmap");
            exit(EXIT_FAILURE);
        }
    }
    if(shm_fdOpened) {
        /* Closing shared memory */
        if(close(shm_fd) == -1) {
            perror("SHM close");
            exit(EXIT_FAILURE);
        }
        /* Unlinking shared memory */
        if(shm_unlink(SHM_PATH) == -1) {
            perror("SHM unlink");
            exit(EXIT_FAILURE);
        }
    }
}

/**
 * Frees all resources.
 */
void free_all_resources() {
    free_credential_vars();
    close_sockets();
    close_shm();
    sem_close(mutex);
}

/**
 * Handles SIGINT signal closing and freeing all the resorces.
 */
void handle_sigint() {
    if(getpid() == SV_PID) free_all_resources();
    else close(tcp_client);

    exit(EXIT_FAILURE);
}

/**
 * Prints an error message and exists after closes and frees everything.
 */
void error(char *msg){
    if(getpid() == SV_PID) {
        signal(SIGQUIT, SIG_IGN);
        kill(0, SIGQUIT);
        free_all_resources();
    }
    else close(tcp_client);

	printf("Error: %s\n", msg);
    fflush(stdout);
	exit(EXIT_FAILURE);
}
