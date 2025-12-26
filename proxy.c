#include "proxy.h"

#include <pthread.h>
#include <semaphore.h>

typedef struct {
    char* request;
    Cache* cache;
    int client_socket;
} ThreadArgs;

int server_socket_init() {
    int server_socket = socket(AF_INET, SOCK_STREAM, 0); 
    if (server_socket < 0) {
        perror("Error creating socket");
        exit(-1);
    }
    int option = 1;
    setsockopt(server_socket, SOL_SOCKET, SO_REUSEADDR, &option, sizeof(option));
    return server_socket;
}

void set_params(struct sockaddr_in* server_addr) {
    memset(server_addr, 0, sizeof(*server_addr));
    server_addr->sin_family = AF_INET;          
    server_addr->sin_addr.s_addr = INADDR_ANY;
    server_addr->sin_port = htons(PORT);
}

void binding_and_listening(int server_socket, struct sockaddr_in* server_addr) {
    if (bind(server_socket, (struct sockaddr*)server_addr, sizeof(*server_addr)) < 0) {
        perror("Error binding socket");
        exit(-1);
    }

    if (listen(server_socket, MAX_USERS_COUNT) < 0) {
        perror("Error listening on socket");
        exit(-1);
    }

    printf("Proxy server started. Listening on port %d...\n", PORT);
}

void read_and_cache_rest(int dest_socket, CacheItem* item, size_t already_read) {
    char buffer[BUFFER_SIZE] = {0};
    size_t bytes_read, all_bytes_read = already_read;

    while ((bytes_read = read(dest_socket, buffer, BUFFER_SIZE)) > 0) {
        if (all_bytes_read + bytes_read > CACHE_BUFFER_SIZE) {
            printf("Data size exceeds CACHE_BUFFER_SIZE! Not saving to cache. Closing connection...\n");

            pthread_mutex_lock(&item->elem_mutex);
            item->is_size_full = 1;
            pthread_mutex_unlock(&item->elem_mutex);

            break;
        }

        // Записываем часть ответа в кэш
        pthread_rwlock_wrlock(&item->rwlock);

        memcpy(item->data->memory + all_bytes_read, buffer, bytes_read);
        item->data->size += bytes_read;

        pthread_rwlock_unlock(&item->rwlock);

        all_bytes_read += bytes_read;
    }
}

void* fetch_and_cache_data(void* arg) {
    ThreadArgs* args = (ThreadArgs*)arg;
    Cache* cache = args->cache;
    char* request = args->request;
    int client_socket = args->client_socket;
    
    char* host = extract_host(request, 50);
    char* url = extract_url(request);
    
    if (!host || !url) {
        printf("Error: Could not extract host or URL\n");
        free(host);
        free(url);
        free(request);
        free(args);
        close(client_socket);
        return NULL;
    }

    CacheItem* item = add_url_to_cache(cache, url);
    if (!item) {
        printf("Error: Could not add item to cache\n");
        free(host);
        free(url);
        free(request);
        free(args);
        close(client_socket);
        return NULL;
    }
    
    pthread_mutex_lock(&item->elem_mutex);
    item->is_loading = 1;
    item->data->memory = (char*)calloc(CACHE_BUFFER_SIZE, sizeof(char));
    item->data->size = 0;
    pthread_mutex_unlock(&item->elem_mutex);

    // Подключаемся к целевому серверу
    int dest_socket = connect_to_remote(host);
    if (dest_socket == -1) {
        printf("Destiny socket error\n");
        
        pthread_mutex_lock(&item->elem_mutex);
        item->is_error = 1;
        free(item->data->memory);
        item->data->memory = NULL;
        pthread_mutex_unlock(&item->elem_mutex);
        
        free(host);
        free(url);
        free(request);
        free(args);
        close(client_socket);
        return NULL;
    }
    
    printf("Created new connection with remote server %s\n", host);

    // Отправляем серверу запрос, полученный от клиента
    ssize_t bytes_sent = send_to(dest_socket, request, strlen(request));
    if (bytes_sent == -1) {
        printf("Error while sending request to remote server\n");
        
        pthread_mutex_lock(&item->elem_mutex);
        item->is_error = 1;
        free(item->data->memory);
        item->data->memory = NULL;
        pthread_mutex_unlock(&item->elem_mutex);
        
        free(host);
        free(url);
        free(request);
        free(args);
        close(client_socket);
        close(dest_socket);
        return NULL;
    }
    
    printf("Sent request to remote server, len = %zd\n", bytes_sent);

    char buffer[BUFFER_SIZE] = {0};
    ssize_t bytes_read, all_bytes_read = 0;
    int client_disconnected = 0;

    while ((bytes_read = read(dest_socket, buffer, BUFFER_SIZE)) > 0) {
        // Проверяем, не отключился ли клиент
        if (!client_disconnected) {
            // Пытаемся отправить данные клиенту
            ssize_t sent_to_client = send_to(client_socket, buffer, bytes_read);
            if (sent_to_client == -1) {
                if (errno == EPIPE || errno == ECONNRESET) {
                    printf("Client disconnected. Continuing to cache data...\n");
                    client_disconnected = 1;
                } else {
                    printf("Error sending to client: %s\n", strerror(errno));
                    break;
                }
            }
        }

        if (all_bytes_read + bytes_read > CACHE_BUFFER_SIZE) {
            printf("Data size exceeds CACHE_BUFFER_SIZE! Not saving to cache.\n");
            
            pthread_mutex_lock(&item->elem_mutex);
            item->is_size_full = 1;
            pthread_mutex_unlock(&item->elem_mutex);
            
            break;
        }

        pthread_rwlock_wrlock(&item->rwlock);
        memcpy(item->data->memory + all_bytes_read, buffer, bytes_read);
        item->data->size += bytes_read;
        pthread_rwlock_unlock(&item->rwlock);

        all_bytes_read += bytes_read;
        
        if (all_bytes_read == bytes_read && !is_response_status_ok(buffer)) {
            printf("Server returned error, not saving to cache.\n");
            
            pthread_mutex_lock(&item->elem_mutex);
            item->is_error = 1;
            pthread_mutex_unlock(&item->elem_mutex);
            
            break;
        }
    }

    pthread_mutex_lock(&item->elem_mutex);
    item->is_loading = 0;
    
    if (item->is_size_full || item->is_error) {
        printf("Error appeared while reading the response, freeing memory...\n");
        free(item->data->memory);
        item->data->memory = NULL;
        item->data->size = 0;
    } else {
        printf("Data added to cache with size %zu\n\n", item->data->size);
    }
    pthread_mutex_unlock(&item->elem_mutex);

    if (!client_disconnected) {
        close(client_socket);
    }
    close(dest_socket);
    free(host);
    free(url);
    free(request);
    free(args);

    return NULL;
}

int send_to(int socket, void* data, unsigned int size) {
    return write(socket, data, size);
}

void handle_client_request(void* args) {
    struct FuncArgs* arg = (struct FuncArgs*)args;
    int client_socket = arg->client_socket;
    Cache* cache = arg->cache;
    
    
    free(args);

    printf("Handling client request...\n");
    char* buffer = calloc(BUFFER_SIZE, sizeof(char));

    int bytes_read = recv(client_socket, buffer, BUFFER_SIZE, 0);
    if (bytes_read <= 0) {
        if (bytes_read == 0) {
            printf("Client disconnected before sending request\n");
        } else {
            perror("Error reading from socket");
        }
        close(client_socket);
        free(buffer);
        return;
    }

    char* url = extract_url(buffer);
    if (url == NULL) {
        printf("Could not extract URL from request\n");
        free(buffer);
        close(client_socket);
        return;
    }

    CacheItem* item = find_url_in_cache(cache, url);
    if (item != NULL) {
        printf("Data found in cache!\n");

        pthread_mutex_lock(&item->elem_mutex);
        if (item->is_loading) {
            pthread_mutex_unlock(&item->elem_mutex);
            printf("Data is loading by other client right now. Starting to catch...\n");

            size_t sent = 0;
            int attempts = 0;
            const int max_attempts = 100; // Максимальное количество попыток (10 секунд)

            while (attempts < max_attempts) {
                pthread_rwlock_rdlock(&item->rwlock);
                size_t to_send = item->data->size - sent;
                
                if (to_send > 0) {
                    ssize_t written = send_to(client_socket, item->data->memory + sent, to_send);
                    if (written <= 0) {
                        pthread_rwlock_unlock(&item->rwlock);
                        if (errno == EPIPE || errno == ECONNRESET) {
                            printf("Client disconnected during streaming\n");
                        } else {
                            printf("Error while sending data to client: %s\n", strerror(errno));
                        }
                        break;
                    }
                    sent += written;
                }
                
                pthread_rwlock_unlock(&item->rwlock);

                pthread_mutex_lock(&item->elem_mutex);
                if (!item->is_loading && sent == item->data->size) {
                    pthread_mutex_unlock(&item->elem_mutex);
                    printf("Finished streaming cached data\n");
                    break;
                }
                pthread_mutex_unlock(&item->elem_mutex);

                usleep(100000); 
                attempts++;
            }
            
            if (attempts >= max_attempts) {
                printf("Timeout waiting for data to load\n");
            }
        } else {
            // Данные уже загружены, отправляем все сразу
            if (item->data->size > 0) {
                send_to(client_socket, item->data->memory, item->data->size);
            }
            pthread_mutex_unlock(&item->elem_mutex);
        }
    } else {
        printf("Data not found in cache. Fetching from remote server...\n");

        pthread_t tid;
        ThreadArgs* thread_args = (ThreadArgs*)malloc(sizeof(ThreadArgs));
        thread_args->cache = cache;
        thread_args->request = strdup(buffer); // Копир запрос
        thread_args->client_socket = client_socket;

        printf("Initializing new downloading thread\n");

        int err = pthread_create(&tid, NULL, &fetch_and_cache_data, thread_args);
        if (err != 0) {
            fprintf(stderr, "Error creating thread: %s\n", strerror(err));
            free(thread_args->request);
            free(thread_args);
        } else {
            pthread_detach(tid);
        }
    }

    // Не закрываем client_socket здесь - он будет закрыт в fetch_and_cache_data
    // ну или уже закрыт при ошибке отправки
    
    free(buffer);
    free(url);
}

// Извлекаем URL из HTTP-запроса
char* extract_url(char* request) {
    const char* method_end = strstr(request, " "); 
    if (!method_end) {
        return NULL;
    }

    //конец URL
    const char* url_end = strstr(method_end + 1, " ");
    if (!url_end) {
        return NULL;
    }

    size_t url_length = url_end - (method_end + 1);
    if (url_length >= MAX_URL_LEN) {
        return NULL;
    }

    char* url = (char*)malloc(url_length + 1);
    strncpy(url, method_end + 1, url_length);
    url[url_length] = '\0';

    return url;
}

char* extract_host(const char* request, size_t max_host_len) {
    const char* host_start = strstr(request, "Host: ");
    if (!host_start) {
        return NULL;
    }

    const char* host_end = strstr(host_start, "\r\n");
    if (!host_end) {
        return NULL;
    }

    // Пропустить "Host: "
    host_start += 6;

    // Вычислить длину Host
    size_t host_length = host_end - host_start;
    if (host_length >= max_host_len) {
        return NULL;
    }

    // Копируем Host в выходной буфер
    char* host = (char*)malloc(host_length + 1);
    strncpy(host, host_start, host_length);
    host[host_length] = '\0';

    return host;
}

int is_response_status_ok(char* buffer) {
    return strstr(buffer, "HTTP/1.0 200 OK") || strstr(buffer, "HTTP/1.1 200 OK");
}