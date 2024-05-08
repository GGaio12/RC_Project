/*******************************************************************************
 * SERVER, waiting new clients TCP and/or UDP to connect in ports defined in
 * argv[1] and argv[2] respectively and with a configuration of all user names
 * passwords and user types defined in a configuraion file (defined in argv[3]).
 * When clients appear, they are requested to login and, if they match a user
 * name and password, they can do every command their type can do by sending
 * the command to the server.
 * USE: >class_server <CLASS_PORT> <CONFIG_PORT> <CONFIG_FILE_NAME>
 * Compile: gcc class_server.c -lpthread -Wextra -Wall -lm -lrt -o class_server 
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
#define MAX_NAME_LENGTH 31
#define MAX_PASS_LENGTH 21
#define MAX_TYPE_LENGTH 21
#define MAX_CLASSES 5
#define MAX_CLASS_SIZE 51
#define MULTICAST_PORT 9867
#define BASE_MULTICAST_IP "239.0.0."
#define SHM_PATH "classes_shm"
#define BIN_SEM_PATH "mutex"
#define FILE_BIN_SEM_PATH "file_mutex"

/* Functions prototipes for tcp */
int create_tcp_server(int TCP_SV_PORT, int LISTEN_NUMS);
void request_login_tcp(int client_fd);
void process_tcp_client(int client_fd);
void list_classes(char* message);
void list_subscribed(char* message);
void subscribe_class(char* message, char* name);
void create_class(char* message, char* name, char* size);
void send_text(char* message, char* name, char* text);
int create_multicast_socket();
struct sockaddr_in create_multicast_addr(char* multicast_ip);


/* Functions prototipes for udp */
int create_udp_server(int UDP_SV_PORT);
void request_login_udp(int udp_fd, struct sockaddr_in client_addr, socklen_t client_addr_size);
void process_udp_client(int udp_fd);
void quit_server();
void list_users(char* message);
void del_user(char* message, char* name);
void add_user(char* message, char* name, char* pass, char* type);

/* Functions prototipes for program suddenly close and/or error */
void free_credential_vars();
void close_sockets();
void close_shm();
void free_all_resources();
void handle_sigint();
void handle_sigquit();
void error(char *msg);

typedef struct Class {
    char name[MAX_NAME_LENGTH];
    char ip_address[16];
    int socket;
    struct sockaddr_in addr;
    int max_n_members;
    int n_members;
    char members_user_names[MAX_CLASS_SIZE][MAX_NAME_LENGTH];
}Class;

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
char USER_NAME[MAX_NAME_LENGTH];
char USER_PASSWORD[MAX_PASS_LENGTH];
char USER_TYPE[MAX_TYPE_LENGTH];
bool admIN = false;

shm* shm_ptr;
int shm_size = sizeof(shm);
int shm_fd;
bool shm_fdOpened = false, classesMaped = false;

FILE* configFile;

sem_t* mutex;
bool mutexCreated = false;
sem_t* file_mutex;
bool file_mutexCreated = false;

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

    /* Initializing named semaphores */
    mutex = sem_open(BIN_SEM_PATH, O_CREAT, 0666, 1);
    if(mutex == SEM_FAILED) error("Opening binary semaphore (mutex)");
    mutexCreated = true;

    file_mutex = sem_open(FILE_BIN_SEM_PATH, O_CREAT, 0666, 1);
    if(file_mutex == SEM_FAILED) error("Opening binary semaphore (file_mutex)");
    file_mutexCreated = true;

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
    while(true) {
        /* Sets the tcp and udp fds in the read set */
        FD_SET(tcp_fd, &read_set);
        FD_SET(udp_s, &read_set);

        /* Selects the ready descriptor (the socket with information in) */ /* Warning unsued nready */
        int nready = select(maxfdp, &read_set, NULL, NULL, NULL);

        if(nready > 0) {
            /* If there is information in the tcp socket it accept the connection */
            if(FD_ISSET(tcp_fd, &read_set)) {
                tcp_client = accept(tcp_fd,(struct sockaddr *)&client_addr,(socklen_t *)&client_addr_size);

                /* Creates a child process to process the communication with the user */
                if(fork() == 0) {
                    close(tcp_fd);
                    signal(SIGQUIT, handle_sigquit);
                    process_tcp_client(tcp_client);
                    exit(EXIT_SUCCESS);
                }
            }

            /* If there is information in the udp socket it receive and process the communication */
            if(FD_ISSET(udp_s, &read_set)) {
                process_udp_client(udp_s);
            }
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
    bool checking = true;
    bool in_file;
    char buffer[BUF_SIZE];
    char authentication[BUF_SIZE];
    char message[BUF_SIZE];
    char* token;

    /* Message that asks the user about the login credentials */
    const char login_message[] = "Please enter your LOGIN credentials (LOGIN <username> <password>):";

    /* Loop that ends when the login was successfully done or the user quit */
    while(checking) {
        /* Writes the login message */
        if(write(client_fd, login_message, 1 + strlen(login_message)) == -1) error("sending login message");

        /* Loop that starts reading until the client send the login command */
        do {
            nread = read(client_fd, buffer, BUF_SIZE-1);
            if(nread < 0) error("read function (nread < 0)");

            /* If the client sends anything to the server */
            if(nread > 0) {
                buffer[nread] = '\0';

                /* If client send 'QUIT' ends the communication */
                if(strcmp(buffer, "QUIT") == 0) {
                    checking = false;
                    break;
                }

                /* Checking if the 'LOGIN' command was written */
                token = strtok(buffer, " ");
                if(strcmp(token, "LOGIN") != 0) {
                    if(sprintf(message, "%s is not the valid a command. To login please use: LOGIN <username> <password>", token) < 0) error("creating login invalid command message");
                    if(write(client_fd, message, 1 + strlen(message)) == -1) error("sending login invalid command message");
                    break;
                }

                /* Checking if the user name was written and save it in 'USER_NAME' global variable */
                token = strtok(NULL, " ");
                if(token == NULL) {
                    if(sprintf(message, "Please insert the user name to login: LOGIN <username> <password>") < 0) error("creating login invalid user name message");
                    if(write(client_fd, message, 1 + strlen(message)) == -1) error("sending login invalid user name message");
                    break;
                }
                else strncpy(USER_NAME, token, sizeof(USER_NAME));

                /* Checking if the password was written and save it in 'USER_PASSWORD' global variable */
                token = strtok(NULL, " ");
                if(token == NULL) {
                    if(sprintf(message, "Please insert the password to login: LOGIN <username> <password>") < 0) error("creating login invalid password message");
                    if(write(client_fd, message, 1 + strlen(message)) == -1) error("sending login invalid password message");
                    break;
                }
                else strncpy(USER_PASSWORD, token, sizeof(USER_PASSWORD));
                
                /* Checks if the user with password is in the config file. Sends 'OK' if it's and 'REJECTED' if not */
                in_file = false;
                sem_wait(file_mutex);
                if((configFile = fopen(CONFIG_FILE_NAME, "r")) == NULL) {
                    sem_post(file_mutex);
                    error("opening the config file");
                }
                while(fgets(authentication, BUF_SIZE, configFile)) {
                    token = strtok(authentication, ";");

                    if(strcmp(token, USER_NAME) == 0) {
                        token = strtok(NULL, ";");

                        if(strcmp(token, USER_PASSWORD) == 0) {
                            token = strtok(NULL, ";");
                            strncpy(USER_TYPE, token, sizeof(USER_TYPE));
                            USER_TYPE[strlen(USER_TYPE)-1] = 0;
                            if(sprintf(message, "OK") < 0) {
                                fclose(configFile);
                                sem_post(file_mutex);
                                error("creating 'OK' login message");
                            }
                            if(write(client_fd, message, 1 + strlen(message)) == -1) {
                                fclose(configFile);
                                sem_post(file_mutex);
                                error("sending 'OK' login message");
                            }
                            in_file = true;
                            checking = false;
                            break;
                        }
                    }
                }
                fclose(configFile);  // closes the file
                sem_post(file_mutex);

                /* In case the user name and or password aren't correct */
                if(!in_file) {
                    if(sprintf(message, "REJECTED") < 0) error("creating 'REJECTED' login message");
                    if(write(client_fd, message, 1 + strlen(message)) == -1) error("sending 'REJECTED' login message");
                }
                break;
            }
        } while(true);
    }
}

/**
 * Process the tcp client requesting a login and then reading the commands sent.
 */
void process_tcp_client(int client_fd) {
    memset(USER_NAME, 0, sizeof(USER_NAME));
    memset(USER_PASSWORD, 0, sizeof(USER_PASSWORD));
    memset(USER_TYPE, 0, sizeof(USER_TYPE));

    int nread;
    char buffer[BUF_SIZE];
    char message[BUF_SIZE + 18];
    bool waiting_commands = true;

    /* Requests the login first */
    request_login_tcp(client_fd);

    /* Reads all the commands sent. 'QUIT' to end the communication */
    do {
        /* Sends a message to the client for he to know the server is waiting a new command */
        if(sprintf(message, "Waiting new command...") < 0) error("creating waitng new command message");
        if(write(client_fd, message, 1 + strlen(message)) == -1) error("sending waitng new command message");

        /* Wait for the client to send a new command and process it. If command is 'QUIT' ends the comunication */
        do {
            nread = read(client_fd, buffer, BUF_SIZE-1);
            if(nread < 0) error("read function (nread < 0)");

            if(nread > 0) {
                buffer[nread] = '\0';
            
                /* Command identification */
                if(strcmp(buffer, "QUIT") == 0) {
                    waiting_commands = false;
                    break;
                }
                else if(strcmp(buffer, "LIST_CLASSES") == 0) list_classes(message);
                else if(strcmp(buffer, "LIST_SUBSCRIBED") == 0) list_subscribed(message);
                else {
                    char* token = strtok(buffer, " ");
                    char name[MAX_NAME_LENGTH];
                    if(strcmp(token, "SUBSCRIBE_CLASS") == 0) {
                        token = strtok(NULL, " ");
                        if(token == NULL || strtok(NULL, " ") != NULL) {
                            if(sprintf(message, "Invalid command. USE: SUBSCRIBE_CLASS {name}") < 0) error("creating invalid command message");
                        }
                        else if(strlen(token) >= MAX_NAME_LENGTH) {
                            if(sprintf(message, "Invalid command. Class name must be <= %d", MAX_NAME_LENGTH-1) < 0) error("creating invalid command message");
                        }
                        else {
                            strncpy(name, token, sizeof(name));
                            subscribe_class(message, name);
                        }
                    }
                    else if(strcmp(token, "CREATE_CLASS") == 0 && strcmp(USER_TYPE, "aluno") != 0) {
                        token = strtok(NULL, " ");
                        if(token == NULL) {
                            if(sprintf(message, "Invalid command. USE: CREATE_CLASS {name} {size}") < 0) error("creating invalid command message");
                        }
                        else if(strlen(token) >= MAX_NAME_LENGTH) {
                            if(sprintf(message, "REJECTED: NAME MUST NOT EXCEED %d CHARACTERS", MAX_NAME_LENGTH-1) < 0) error("creating invalid command message");
                        }
                        else {
                            strncpy(name, token, sizeof(name));

                            token = strtok(NULL, " ");
                            if(token == NULL || strtok(NULL, " ") != NULL) {
                                if(sprintf(message, "Invalid command. USE: CREATE_CLASS {name} {size}") < 0) error("creating invalid command message");
                            }
                            else create_class(message, name, token);
                        }
                    }
                    else if(strcmp(token, "SEND") == 0 && strcmp(USER_TYPE, "aluno") != 0) {
                        char text[BUF_SIZE];
                        token = strtok(NULL, " ");
                        if(token == NULL) {
                            if(sprintf(message, "Invalid command. USE: SEND {name} {text to send}") < 0) error("creating invalid command message");
                        }
                        else if(strlen(token) >= MAX_NAME_LENGTH) {
                            if(sprintf(message, "REJECTED: NAME MUST NOT EXCEED %d CHARACTERS", MAX_NAME_LENGTH-1) < 0) error("creating invalid command message");
                        }
                        else {
                            strncpy(name, token, sizeof(name));

                            int text_start = strlen("SEND ") + strlen(name) + 1; // +1 for the space between name and text
                            int text_length = nread - 1 - text_start;

                            if(text_length <= 0) {
                                if(sprintf(message, "Invalid command. USE: SEND {name} {text to send}") < 0) error("creating invalid command message");
                            }
                            else {
                                strncpy(text, buffer + text_start, nread-1);
                                text[text_length] = '\0';

                                send_text(message, name, text);
                                break;
                            }
                        }
                    }
                    else strcpy(message, "Command not recognized");
                }

                /* Sending message generated by the command */
                if(write(client_fd, message, 1 + strlen(message)) == -1) error("sending message");

                break;
            }
        } while(true);
    } while(waiting_commands);
    close(client_fd); // closes the client socket
}

/**
 * Creates and returns a message that indicates all avaiable classes.
 */
void list_classes(char* message) {
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
}

/**
 * Creates and returns a message that indicates all classes that the client is subscribed.
 */
void list_subscribed(char* message) {
    bool sub;
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
}

/**
 * Susbscribe a class with a specific 'name' and return a message 'ACCEPTED {multicast}' or 'REJECTED'.
 */
void subscribe_class(char* message, char* name) {
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
                strncpy(shm_ptr->classes[i].members_user_names[shm_ptr->classes[i].n_members], USER_NAME, MAX_NAME_LENGTH-1);
                shm_ptr->classes[i].n_members++;

                strcpy(message, "ACCEPTED ");
                strcat(message, shm_ptr->classes[i].ip_address);
            }
            else strcpy(message, "REJECTED: CLASS ALREADY FULL");
            i++;
            break;
        }
        i++;
    }
    sem_post(mutex);
    if(i == 0) strcpy(message, "REJECTED: NO CLASSES CREATED");
    else if(!found) strcpy(message, "REJECTED: CLASS NOT FOUND");
    else if(already_in) strcpy(message, "REJECTED: ALREADY SUBSCRIBED IN CLASS");
}

/**
 * Creates a new class with a specific 'name' and 'size' and return a message 'OK {multicast}'.
 */
void create_class(char* message, char* name, char* size) {
    bool have_same_name = false;
    sem_wait(mutex);
    /* Verifies if there are space to create a new class */
    if(atoi(size) >= MAX_CLASS_SIZE) sprintf(message, "REJECTED: SIZE EXCEEDS MAX SIZE OF %d", MAX_CLASS_SIZE-1);
    else if(atoi(size) < 1) strcpy(message, "REJECTED: SIZE NEEDS TO BE >= 1");
    else if(shm_ptr->n_classes >= MAX_CLASSES) strcpy(message, "REJECTED: MAX CLASSES REACHED");
    else {
        /* First verifies if already exist a class with the same name */
        for(int i = 0; i < shm_ptr->n_classes; i++) {
            if(strcmp(shm_ptr->classes[i].name, name) == 0) {
                strcpy(message, "REJECTED: ALREADY EXIST A CLASS WITH THAT NAME");
                have_same_name = true;
                break;
            }
        }
        
        /* Creating a new class */
        if(!have_same_name) {
            /* Creating aux variables */
            char str_aux[2];
            char newIP[16];
            str_aux[0] = shm_ptr->n_classes + 1 + '0';
            strcpy(newIP, BASE_MULTICAST_IP);
            strcat(newIP, str_aux);
            /* Assigning values to the class */
            strncpy(shm_ptr->classes[shm_ptr->n_classes].name, name, MAX_NAME_LENGTH);      // Class name
            strncpy(shm_ptr->classes[shm_ptr->n_classes].ip_address, newIP, 15);            // Multicast IP
            shm_ptr->classes[shm_ptr->n_classes].socket = create_multicast_socket();        // Multicast socket
            shm_ptr->classes[shm_ptr->n_classes].addr = create_multicast_addr(newIP);       // Multicast address
            shm_ptr->classes[shm_ptr->n_classes].max_n_members = atoi(size);                // Class max number of members
            shm_ptr->classes[shm_ptr->n_classes].n_members = 1;                             // Adding the professor to the number of members
            strncpy(shm_ptr->classes[shm_ptr->n_classes].members_user_names[0], USER_NAME, MAX_NAME_LENGTH-1);  // Adding the professor user name to the members user names
            
            shm_ptr->n_classes++;
            strcpy(message, "OK ");
            strcat(message, newIP);
        }
    }
    sem_post(mutex);
}

/**
 * Sends a message to a class with a specific 'name'.
 */
void send_text(char* message, char* name, char* text) {
    int class_socket;
    struct sockaddr_in class_addr;
    bool class_found = false;
    char text_to_send[BUF_SIZE + MAX_NAME_LENGTH + 32];

    int i = 0;
    sem_wait(mutex);
    /* Going throught all created classes in shared memory */
    while(i < shm_ptr->n_classes) {
        if(strcmp(shm_ptr->classes[i].name, name) == 0) {
            class_socket = shm_ptr->classes[i].socket;
            class_addr = shm_ptr->classes[i].addr;
            class_found = true;
            break;
        }
        i++;
    }
    sem_post(mutex);

    if(class_found) {
        /* Sending muticast message */
        if(sprintf(text_to_send, "MESSAGE FROM CLASS '%s': %s", name, text) < 0) error("creating text to send message");
        if(sendto(class_socket, text_to_send, 1 + strlen(text_to_send), 0, (struct sockaddr *)&class_addr, sizeof(class_addr)) < 0) error("sending multicast message");
        strcpy(message, "MESSAGE SENT");
    }
    else strcpy(message, "CLASS NOT FOUND");
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
    bool checking = true;
    bool in_file;
    char buffer[BUF_SIZE];
    char authentication[BUF_SIZE];
    char message[BUF_SIZE];
    char* token;

    /* Message that asks the user about the login credentials */
    const char login_message[] = "Please enter your LOGIN credentials (LOGIN <username> <password>):";

    /* Loop that ends when the login was successfully done or the user quit */
    while(checking) {
        /* Writes the login message */
        if(sendto(udp_fd, login_message, strlen(login_message), 0, (struct sockaddr*)&client_addr, client_addr_size) == -1) error("sending login message");

        /* Loop that starts reading until the client send the login command */
        do {
            nread = recvfrom(udp_fd, buffer, BUF_SIZE-1, 0, (struct sockaddr*)&client_addr, &client_addr_size);
            if(nread < 0) error("read function (nread < 0)");

            if(nread > 0) {
                buffer[nread] = '\0';

                /* If client send 'QUIT' ends the communication */
                if(strcmp(buffer, "QUIT") == 0) {
                    checking = false;
                    break;
                }

                /* Checking if the 'LOGIN' command was written */
                token = strtok(buffer, " ");
                if(strcmp(token, "LOGIN") != 0) {
                    if(sprintf(message, "%s is not the valid command, to login please use: LOGIN <username> <password>", token) < 0) error("creating login invalid command message");
                    if(sendto(udp_fd, message, strlen(message), 0, (struct sockaddr*)&client_addr, client_addr_size) == -1) error("sending login invalid command message");
                    break;
                }

                /* Checking if the user name was written and save it in 'USER_NAME' global variable */
                token = strtok(NULL, " ");
                if(token == NULL) {
                    if(sprintf(message, "Please insert the user name to login: LOGIN <username> <password>") < 0) error("creating login invalid user name message");
                    if(sendto(udp_fd, message, strlen(message), 0, (struct sockaddr*)&client_addr, client_addr_size) == -1) error("sending login invalid user name message");
                    break;
                }
                else strncpy(USER_NAME, token, sizeof(USER_NAME));

                /* Checking if the password was written and save it in 'USER_PASSWORD' global variable */
                token = strtok(NULL, " ");
                if(token == NULL) {
                    if(sprintf(message, "Please insert the password to login: LOGIN <username> <password>") < 0) error("creating login invalid password message");
                    if(sendto(udp_fd, message, strlen(message), 0, (struct sockaddr*)&client_addr, client_addr_size) == -1) error("sending login invalid password message");
                    break;
                }
                else strncpy(USER_PASSWORD, token, sizeof(USER_PASSWORD));

                /* Checks if the user with password is in the config file. Sends 'OK' if it's and 'REJECTED' if not */
                in_file = false;
                sem_wait(file_mutex);
                if((configFile = fopen(CONFIG_FILE_NAME, "r")) == NULL) {
                    sem_post(file_mutex);
                    error("opening the config file");
                }
                while(fgets(authentication, BUF_SIZE, configFile)) {
                    token = strtok(authentication, ";");

                    if(strcmp(token, USER_NAME) == 0) {
                        token = strtok(NULL, ";");

                        if(strcmp(token, USER_PASSWORD) == 0) {
                            in_file = true;
                            token = strtok(NULL, ";");
                            strncpy(USER_TYPE, token, sizeof(USER_TYPE));
                            USER_TYPE[strlen(USER_TYPE) - 1] = 0;

                            if(strcmp(USER_TYPE, "administrador") == 0) {
                                if(sprintf(message, "OK") < 0) {
                                    fclose(configFile);
                                    sem_post(file_mutex);
                                    error("creating 'OK' login message");
                                }
                                checking = false;
                            }
                            else {
                                if(sprintf(message, "USER IS NOT ADM") < 0) {
                                    fclose(configFile);
                                    sem_post(file_mutex);
                                    error("creating not adm login message");
                                }
                            }
                            if(sendto(udp_fd, message, strlen(message), 0, (struct sockaddr*)&client_addr, client_addr_size) == -1) {
                                fclose(configFile);
                                sem_post(file_mutex);
                                error("sending 'OK' login message");
                            }
                            break;
                        }
                    }
                }
                fclose(configFile);  // closes the file
                sem_post(file_mutex);

                /* In case the user name and or password aren't correct */
                if(!in_file) {
                    if(sprintf(message, "REJECTED") < 0) error("creating 'REJECTED' login message");
                    if(sendto(udp_fd, message, strlen(message), 0, (struct sockaddr*)&client_addr, client_addr_size) == -1) error("sending 'REJECTED' login message");
                }
                break;
            }
        } while(true);
        if(checking == false) break;
    }
}

/**
 * Process the udp client requesting a login and then reading the commands sent.
 */
void process_udp_client(int udp_fd) {
    memset(USER_NAME, 0, sizeof(USER_NAME));
    memset(USER_PASSWORD, 0, sizeof(USER_PASSWORD));
    memset(USER_TYPE, 0, sizeof(USER_TYPE));

    struct sockaddr_in client_addr;
    socklen_t client_addr_size = sizeof(client_addr);
    int nread;
    char buffer[2*BUF_SIZE];
    char message[2*BUF_SIZE];
    bool waiting_commands = true;

    nread = recvfrom(udp_fd, buffer, (2*BUF_SIZE)-1, 0, (struct sockaddr*)&client_addr, &client_addr_size);
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
        if(sprintf(message, "Waiting new command...") < 0) error("creating waitng new command message");
        if(sendto(udp_fd, message, strlen(message), 0, (struct sockaddr*)&client_addr, client_addr_size) == -1) error("sending waitng new command message");
    
        /* Wait for the client to send a new command and process it. If command is 'QUIT' ends the comunication */
        do {
            nread = recvfrom(udp_fd, buffer, (2*BUF_SIZE)-1, 0, (struct sockaddr*)&client_addr, &client_addr_size);
            if(nread < 0) error("read function (nread < 0)");

            if(nread > 0) {
                buffer[nread] = '\0';

                /* Command identification */
                if(strcmp(buffer, "QUIT") == 0) {
                    waiting_commands = false;
                    break;
                }
                else if(strcmp(buffer, "QUIT_SERVER") == 0) quit_server();
                else if(strcmp(buffer, "LIST") == 0) list_users(message);
                else {
                    char* token = strtok(buffer, " ");
                    char name[MAX_NAME_LENGTH];
                    if(strcmp(token, "DEL") == 0) {
                        token = strtok(NULL, " ");
                        if(token == NULL || strtok(NULL, " ") != NULL) {
                            if(sprintf(message, "Invalid command. USE: DEL {user_name}") < 0) error("creating invalid command message");
                        }
                        else if(strlen(token) >= MAX_NAME_LENGTH) {
                            if(sprintf(message, "Invalid command. NAME MUST NOT EXCEED %d CHARACTERS", MAX_NAME_LENGTH-1) < 0) error("creating invalid command message");
                        }
                        else {
                            strncpy(name, token, sizeof(name));
                            del_user(message, name);
                        }
                    }
                    else if(strcmp(token, "ADD_USER") == 0) {
                        char pass[MAX_PASS_LENGTH];
                        char type[MAX_TYPE_LENGTH];
                        token = strtok(NULL, " ");
                        if(token == NULL) {
                            if(sprintf(message, "Invalid command. USE: ADD_USER {user_name} {password} {user_type}") < 0) error("creating invalid command message");
                        }
                        else if(strlen(token) >= MAX_NAME_LENGTH) {
                            if(sprintf(message, "Invalid command. NAME MUST NOT EXCEED %d CHARACTERS", MAX_NAME_LENGTH-1) < 0) error("creating invalid command message");
                        }
                        else {
                            strncpy(name, token, sizeof(name));
                            token = strtok(NULL, " ");
                            if(token == NULL) {
                                if(sprintf(message, "Invalid command. USE: ADD_USER {user_name} {password} {user_type}") < 0) error("creating invalid command message");
                            }
                            else if(strlen(token) >= MAX_PASS_LENGTH) {
                                if(sprintf(message, "Invalid command. PASSWORD MUST NOT EXCEED %d CHARACTERS", MAX_PASS_LENGTH-1) < 0) error("creating invalid command message");
                            }
                            else {
                                strncpy(pass, token, sizeof(pass));
                                token = strtok(NULL, " ");
                                if(token == NULL) {
                                    if(sprintf(message, "Invalid command. USE: ADD_USER {user_name} {password} {user_type}") < 0) error("creating invalid command message");
                                }
                                else if(strlen(token) >= MAX_TYPE_LENGTH) {
                                    if(sprintf(message, "Invalid command. USER TYPE MUST NOT EXCEED %d CHARACTERS", MAX_TYPE_LENGTH-1) < 0) error("creating invalid command message");
                                }
                                else {
                                    strncpy(type, token, sizeof(type));
                                    add_user(message, name, pass, type);
                                }
                            }
                        }
                    }
                    else strcpy(message, "Command not recognized");
                }
                
                /* Sending message generated by the command */
                if(sendto(udp_fd, message, strlen(message), 0, (struct sockaddr*)&client_addr, client_addr_size) == -1) error("sending message");

                break;
            }
        } while(true);
    } while(waiting_commands);
}

/**
 * Lists all the users info present in config file.
 */
void list_users(char* message) {
    char line[BUF_SIZE];
    char name[MAX_NAME_LENGTH];
    char pass[MAX_PASS_LENGTH];
    char type[MAX_TYPE_LENGTH];
    char* token;

    strcpy(message, "------- USERS LIST -------");

    /* Checks if the user with password is in the config file. Sends 'OK' if it's and 'REJECTED' if not */
    sem_wait(file_mutex);
    if((configFile = fopen(CONFIG_FILE_NAME, "r")) == NULL) {
        sem_post(file_mutex);
        error("opening the config file");
    }
    int i = 0;
    while(fgets(line, BUF_SIZE, configFile)) {
        memset(name, 0, sizeof(name));
        memset(name, 0, sizeof(name));
        memset(name, 0, sizeof(name));

        token = strtok(line, ";");
        strcpy(name, token);

        token = strtok(NULL, ";");
        strcpy(pass, token);

        token = strtok(NULL, ";");
        strcpy(type, token);

        if(i != 0) strcat(message, "\n--------------------------");
        strcat(message, "\nUSER NAME: ");
        strcat(message, name);
        strcat(message, "\n PASSWORD: ");
        strcat(message, pass);
        strcat(message, "\n   TYPE  : ");
        strcat(message, type);
        if(message[strlen(message) - 1] == '\n') message[strlen(message) - 1] = 0;
        i++;
    }
    fclose(configFile);
    sem_post(file_mutex);

    strcat(message, "\n------- USERS LIST -------");
}

/**
 * Deletes a user with a specific user name from the config file.
 */
void del_user(char* message, char* name) {
    FILE* tempFile;
    char line[BUF_SIZE];
    char newline[BUF_SIZE+1];
    bool in_file = false;

    tempFile = tmpfile();
    if(tempFile == NULL) error("opening temporary file");

    /* Checks if the user is in the config file and puts in temp file all other users */
    sem_wait(file_mutex);
    if((configFile = fopen(CONFIG_FILE_NAME, "r")) == NULL) {
        sem_post(file_mutex);
        fclose(tempFile);
        error("opening the config file for read");
    }
    while(fgets(line, BUF_SIZE, configFile)) {
        if(strncmp(line, name, strlen(name)) == 0) in_file = true;
        else fputs(line, tempFile);
    }
    fclose(configFile);
    
    /* In case the user is not in the file */
    if(!in_file) {
        if(sprintf(message, "USER NOT FOUND") < 0) {
            sem_post(file_mutex);
            fclose(tempFile);
            error("creating user not found message");
        }
    }
    else {
        /* Rewinding the temporary file to the beginning */
        rewind(tempFile);

        /* Rewriting the file with all the users info that are in temp file */
        if((configFile = fopen(CONFIG_FILE_NAME, "w")) == NULL) {
            sem_post(file_mutex);
            fclose(tempFile);
            error("opening the config file for write");
        }
        while(fgets(line, BUF_SIZE, tempFile)) fputs(line, configFile);
        if(sprintf(message, "USER %s REMOVED SUCCESSFULLY", name) < 0) {
            sem_post(file_mutex);
            fclose(tempFile);
            error("creating user removed message");
        }
        fclose(configFile);
    }
    sem_post(file_mutex);

    fclose(tempFile);
}

/**
 * Adds a new user with a specific name, password and type to the config file.
 */
void add_user(char* message, char* name, char* pass, char* type) {
    char line[BUF_SIZE];
    bool in_file = false;

    sem_wait(file_mutex);
    if((configFile = fopen(CONFIG_FILE_NAME, "r")) == NULL) {
        sem_post(file_mutex);
        error("opening the config file for append");
    }
    while(fgets(line, BUF_SIZE, configFile)) {
        if(strncmp(line, name, strlen(name)) == 0) {
            in_file = true;
            break;
        }
    }
    fclose(configFile);

    if(in_file) strcpy(message, "USER ALREADY IN CONFIG FILE");
    else{
        if((configFile = fopen(CONFIG_FILE_NAME, "a")) == NULL) {
            sem_post(file_mutex);
            error("opening the config file for append");
        }
        fprintf(configFile, "%s;%s;%s\n", name, pass, type);
        fclose(configFile);
        if(sprintf(message, "USER %s ADDED SUCCESSFULLY", name) < 0) error("creating user added message");
    }
    sem_post(file_mutex);
}


/*******************************************************
 *
 *                     SYSTEM MANAGE
 * 
 *******************************************************/

/**
 * Ends server closing and freeing every process and resource.
 */
void quit_server() {
    int status;
    signal(SIGQUIT, SIG_IGN);
    kill(0, SIGQUIT);
    while(waitpid(-1, &status, WNOHANG) > 0);
    free_all_resources();
    exit(EXIT_SUCCESS);
}

/**
 * Closes all the sockets created if they exist.
 */
void close_sockets() {
    if(tcp_fdCreated) close(tcp_fd);
    if(udp_sCreated) close(udp_s);
    int wait = sem_trywait(mutex);
    for(int i = 0; i < shm_ptr->n_classes; i++) close(shm_ptr->classes[i].socket);
    if(wait == 0) sem_post(mutex);
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
    close_sockets();
    close_shm();
    if(mutexCreated) {
        sem_close(mutex);
        sem_unlink(BIN_SEM_PATH);
    }
    if(file_mutexCreated) {
        if(sem_trywait(file_mutex) != 0) fclose(configFile);
        sem_close(file_mutex);
        sem_unlink(FILE_BIN_SEM_PATH);
    }
}

/**
 * Handles SIGINT signal closing and freeing all the resorces.
 */
void handle_sigint() {
    if(getpid() == SV_PID) quit_server();
    else close(tcp_client);

    exit(EXIT_SUCCESS);
}

/**
 * Handles SIGQUIT signal closing and freeing all the resorces of child processes.
 */
void handle_sigquit() {
    close(tcp_client);

    exit(EXIT_SUCCESS);
}

/**
 * Prints an error message and exists after closes and frees everything.
 */
void error(char *msg){
    if(getpid() == SV_PID) quit_server();
    else close(tcp_client);

	printf("Error: %s\n", msg);
    fflush(stdout);
	exit(EXIT_FAILURE);
}
