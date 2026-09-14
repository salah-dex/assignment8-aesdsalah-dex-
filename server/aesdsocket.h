
// defines and function prototypes for aesdsocket.c
/*
 * AESD Socket Server
 *
 * This server listens for incoming TCP connections on port 9000. It accepts
 * messages from clients, logs them to a file, and sends the logged messages
 * back to the client upon receiving a newline character.
 *
 * The server performs the following steps:
 *
 * 1. Sets up signal handlers for SIGINT and SIGTERM to allow graceful shutdown.
 *
 * 2. Creates a TCP socket and binds it to port 9000.
 *
 * 3. Listens for incoming connections with a backlog of 5.
 *
 * 4. In an infinite loop, it:
 *     a. Waits for a client to connect.
 *     b. Logs the client's IP address to the syslog.
 *     c. Creates a new thread to handle the client's connection.
 *     d. The thread receives data from the client, appends it to a log file, and   
 *     e. If a newline character is received, it sends the contents of the log file back to the client.
 *     5. On shutdown, it logs a shutdown message to the syslog and cleans up resources.
 * 
 * Author:  GHANEM Salaheddine
 * Date:    26/05/2026 
 */ 

#ifndef AESDSOCKET_H
#define AESDSOCKET_H 

/* header files */
#include <stdio.h>
#include <stdlib.h> 
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <syslog.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <errno.h>
#include <pthread.h>
#include <stdbool.h>

/****************** Function Prototypes ******************/
/* public functions */
int start_server();

#endif
