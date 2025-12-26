#ifndef HTTP_REQUEST_H
#define HTTP_REQUEST_H

#define _GNU_SOURCE
#include <arpa/inet.h>
#include <curl/curl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>


typedef struct DataStruct {
    char* memory;
    size_t size;
} Data;

int connect_to_remote(const char* host);

#endif //HTTP_REQUEST_H