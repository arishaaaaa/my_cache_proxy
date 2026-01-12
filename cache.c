#include "cache.h"

void init_cache(Cache* cache) {
    pthread_mutex_init(&cache->cache_global_mutex, NULL);
    
    for (int i = 0; i < MAX_CACHE_SIZE; i++) {
        memset(cache->cache[i].url, '\0', MAX_URL_LEN);

        cache->cache[i].data = (Data*)malloc(sizeof(Data));
        cache->cache[i].data->memory = NULL;
        cache->cache[i].data->size = 0;

        cache->cache[i].LRU = 0;
        cache->cache[i].is_error = cache->cache[i].is_loading = cache->cache[i].is_size_full = 0;

        pthread_mutex_init(&cache->cache[i].elem_mutex, NULL);
        pthread_cond_init(&cache->cache[i].loading_cond, NULL);
    }
    printf("Cache has been initialized successfully!\n");
}

void destroy_cache(Cache* cache) {
    if (cache == NULL) return;

    pthread_mutex_destroy(&cache->cache_global_mutex);

    for (int i = 0; i < MAX_CACHE_SIZE; ++i) {
        pthread_mutex_lock(&cache->cache[i].elem_mutex);
        
        if (cache->cache[i].data != NULL) {
            if (cache->cache[i].data->memory != NULL)
                free(cache->cache[i].data->memory);
            free(cache->cache[i].data);
        }
        
        pthread_mutex_unlock(&cache->cache[i].elem_mutex);
        pthread_mutex_destroy(&cache->cache[i].elem_mutex);
        pthread_cond_destroy(&cache->cache[i].loading_cond);
    }

    printf("Cache has been destroyed successfully!\n");
    free(cache);
}

void delete_item(const char* url, Cache* cache) {
    CacheItem* item = atomic_find_or_add_url(cache, url);

    if (item == NULL || strcmp(item->url, "") == 0) return;

    pthread_mutex_lock(&item->elem_mutex);
    if (item->data != NULL) {
        if (item->data->memory != NULL) {
            free(item->data->memory);
            item->data->memory = NULL;
        }
        item->data->size = 0;
    }
    memset(item->url, '\0', MAX_URL_LEN);
    item->is_error = 0;
    item->is_loading = 0;
    item->is_size_full = 0;
    pthread_cond_broadcast(&item->loading_cond);
    pthread_mutex_unlock(&item->elem_mutex);
}

CacheItem* atomic_find_or_add_url(Cache* cache, const char* url) {
    pthread_mutex_lock(&cache->cache_global_mutex);
    
    CacheItem* item = NULL;
    
    // Сначала ищем существующий URL
    for (int i = 0; i < MAX_CACHE_SIZE; ++i) {
        pthread_mutex_lock(&cache->cache[i].elem_mutex);
        
        if (strcmp(cache->cache[i].url, "") != 0 && 
            strcmp(cache->cache[i].url, url) == 0) {
            item = &cache->cache[i];
            item->LRU = time(NULL);
            pthread_mutex_unlock(&cache->cache[i].elem_mutex);
            break;
        }
        
        pthread_mutex_unlock(&cache->cache[i].elem_mutex);
    }
    
    // Если не нашли - добавляем новый
    if (item == NULL) {
        int index = -1;
        
        // Ищем свободный слот
        for (int i = 0; i < MAX_CACHE_SIZE; ++i) {
            pthread_mutex_lock(&cache->cache[i].elem_mutex);
            
            if (strcmp(cache->cache[i].url, "") == 0) {
                pthread_mutex_unlock(&cache->cache[i].elem_mutex);
                index = i;
                break;
            }
            
            pthread_mutex_unlock(&cache->cache[i].elem_mutex);
        }
        
        // Если нет свободных - ищем LRU
        if (index == -1) {
            int minInd = 0;
            time_t min = 0;
            int first = 1;
            
            for (int i = 0; i < MAX_CACHE_SIZE; ++i) {
                pthread_mutex_lock(&cache->cache[i].elem_mutex);
                
                if (strcmp(cache->cache[i].url, "") == 0) {
                    pthread_mutex_unlock(&cache->cache[i].elem_mutex);
                    continue;
                }
                
                if (first) {
                    min = cache->cache[i].LRU;
                    minInd = i;
                    first = 0;
                } else if (cache->cache[i].LRU < min) {
                    min = cache->cache[i].LRU;
                    minInd = i;
                }
                
                pthread_mutex_unlock(&cache->cache[i].elem_mutex);
            }
            
            index = minInd;
            
            // Очищаем старый элемент
            pthread_mutex_lock(&cache->cache[index].elem_mutex);
            if (cache->cache[index].data != NULL) {
                if (cache->cache[index].data->memory != NULL) {
                    free(cache->cache[index].data->memory);
                }
                free(cache->cache[index].data);
                cache->cache[index].data = (Data*)malloc(sizeof(Data));
                cache->cache[index].data->memory = NULL;
                cache->cache[index].data->size = 0;
            }
            
            // Сбрасываем флаги
            cache->cache[index].is_loading = 0;
            cache->cache[index].is_error = 0;
            cache->cache[index].is_size_full = 0;
            pthread_cond_broadcast(&cache->cache[index].loading_cond);
            pthread_mutex_unlock(&cache->cache[index].elem_mutex);
        } else {
            // Инициализируем новый элемент
            pthread_mutex_lock(&cache->cache[index].elem_mutex);
            if (cache->cache[index].data == NULL) {
                cache->cache[index].data = (Data*)malloc(sizeof(Data));
                cache->cache[index].data->memory = NULL;
                cache->cache[index].data->size = 0;
            }
            pthread_mutex_unlock(&cache->cache[index].elem_mutex);
        }
        
        // Занимаем слот
        pthread_mutex_lock(&cache->cache[index].elem_mutex);
        strcpy(cache->cache[index].url, url);
        cache->cache[index].LRU = time(NULL);
        cache->cache[index].is_loading = 0;
        cache->cache[index].is_error = 0;
        cache->cache[index].is_size_full = 0;
        pthread_mutex_unlock(&cache->cache[index].elem_mutex);
        
        item = &cache->cache[index];
    }
    
    pthread_mutex_unlock(&cache->cache_global_mutex);
    return item;
}
