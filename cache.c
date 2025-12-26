#include "cache.h"

void init_cache(Cache* cache) {
    for (int i = 0; i < MAX_CACHE_SIZE; i++) {
        memset(cache->cache[i].url, '\0', MAX_URL_LEN);

        cache->cache[i].data = (Data*)malloc(sizeof(Data));
        cache->cache[i].data->memory = NULL;

        cache->cache[i].LRU = 0;
        cache->cache[i].is_error = cache->cache[i].is_loading = cache->cache[i].is_size_full = 0;

        pthread_rwlock_init(&cache->cache[i].rwlock, NULL);
        pthread_mutex_init(&cache->cache[i].elem_mutex, NULL);
    }
    printf("Cache has been initialized successfully!\n");
}

void destroy_cache(Cache* cache) {
    if (cache == NULL) return;

    for (int i = 0; i < MAX_CACHE_SIZE; ++i) {
        pthread_mutex_lock(&cache->cache[i].elem_mutex);
        if (cache->cache[i].data != NULL) {
            if (cache->cache[i].data->memory != NULL)
                free(cache->cache[i].data->memory);
            free(cache->cache[i].data);
        }
        pthread_mutex_unlock(&cache->cache[i].elem_mutex);

        pthread_rwlock_destroy(&cache->cache[i].rwlock);
        pthread_mutex_destroy(&cache->cache[i].elem_mutex);
    }

    printf("Cache has been destroyed successfully!\n");

    free(cache);
}

void delete_item(const char* url, Cache* cache) {
    CacheItem* item = find_url_in_cache(cache, url);

    if (item == NULL) return;

    pthread_mutex_lock(&item->elem_mutex);
    if (item->data != NULL) {
        if (item->data->memory != NULL) {
            free(item->data->memory);
            item->data->memory = NULL;
        }
        // Не освоб сам item->data, будет переиспользован
    }
    memset(item->url, '\0', MAX_URL_LEN);
    item->is_error = 0;
    item->is_loading = 0;
    item->is_size_full = 0;
    pthread_mutex_unlock(&item->elem_mutex);
}

CacheItem* find_url_in_cache(Cache* cache, const char* url) {
    for (int i = 0; i < MAX_CACHE_SIZE; ++i) {
        pthread_mutex_lock(&cache->cache[i].elem_mutex);
        if (strcmp(cache->cache[i].url, "") == 0) {
            pthread_mutex_unlock(&cache->cache[i].elem_mutex);
            continue;
        }
        if (strcmp(cache->cache[i].url, url) == 0) {
            cache->cache[i].LRU = time(NULL);
            pthread_mutex_unlock(&cache->cache[i].elem_mutex);
            return &cache->cache[i];
        }
        pthread_mutex_unlock(&cache->cache[i].elem_mutex);
    }
    return NULL;
}

int find_empty_url_in_cache(Cache* cache) {
    for (int i = 0; i < MAX_CACHE_SIZE; ++i) {
        pthread_mutex_lock(&cache->cache[i].elem_mutex);
        if (strcmp(cache->cache[i].url, "") == 0)  {
            pthread_mutex_unlock(&cache->cache[i].elem_mutex);
            return i;
        }
        pthread_mutex_unlock(&cache->cache[i].elem_mutex);
    }
    return -1;
}

CacheItem* add_url_to_cache(Cache* cache, const char* url) {
    if (find_url_in_cache(cache, url) != NULL) {
        printf("Not add to cache url (already in) : |%s| \n", url);
        return NULL;
    }

    CacheItem* item = (CacheItem*)malloc(sizeof(CacheItem));

    int index = find_empty_url_in_cache(cache);

    if (index != -1) {
        printf("Add to cache url (count < max) : |%s| \n", url);

        pthread_mutex_lock(&cache->cache[index].elem_mutex);

        strcpy(cache->cache[index].url, url);
        cache->cache[index].LRU = time(NULL);

        item = &cache->cache[index];

        pthread_mutex_unlock(&cache->cache[index].elem_mutex);
    } else {
        printf("Add to cache url (delete) : |%s| \n", url);

        int minInd = 0;
        time_t min = 0;

        for (int i = 0; i < MAX_CACHE_SIZE; ++i) {
            pthread_mutex_lock(&cache->cache[i].elem_mutex);
            if (strcmp(cache->cache[i].url, "") == 0) {
                continue;
            }
            if (!i) {
                min = cache->cache[i].LRU;
                minInd = i;
                continue;
            }
            if (cache->cache[i].LRU < min) {
                min = cache->cache[i].LRU;
                minInd = i;
            }
        }

        strcpy(cache->cache[minInd].url, url);
        free(cache->cache[minInd].data);       
        cache->cache[minInd].LRU = time(NULL);

        item = &cache->cache[minInd];

        for (int i = 0; i < MAX_CACHE_SIZE; ++i) {
            pthread_mutex_unlock(&cache->cache[i].elem_mutex);
        }
    }

    return item;
}