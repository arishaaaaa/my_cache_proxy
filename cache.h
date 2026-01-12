#ifndef CACHE_H
#define CACHE_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <time.h>

#include "request.h"

#define MAX_CACHE_SIZE 3
#define MAX_URL_LEN 1024
#define CACHE_BUFFER_SIZE (500 * 1024 * 1024)  

typedef struct {
    char url[MAX_URL_LEN];
    Data* data;
    time_t LRU;
    int is_loading;
    int is_size_full;
    int is_error;
    pthread_mutex_t elem_mutex;
    pthread_cond_t loading_cond;
} CacheItem;

typedef struct {
    CacheItem cache[MAX_CACHE_SIZE];
    pthread_mutex_t cache_global_mutex;
} Cache;

void init_cache(Cache* cache);
void destroy_cache(Cache* cache);

void delete_item(const char* url, Cache* cache);
CacheItem* atomic_find_or_add_url(Cache* cache, const char* url);

#endif //CACHE_H
