#include "proxy.h"

#include <sys/types.h>
#include <netdb.h>

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

int send_to(int socket, void* data, unsigned int size) {
    ssize_t sent = 0;
    ssize_t total_sent = 0;
    char* buffer = (char*)data;
    
    while (total_sent < size) {
        sent = write(socket, buffer + total_sent, size - total_sent);
        if (sent <= 0) {
            if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                continue;
            }
            return -1;
        }
        total_sent += sent;
    }
    
    return total_sent;
}

void* fetch_and_cache_data(void* arg) {
    ThreadArgs* args = (ThreadArgs*)arg;
    Cache* cache = args->cache;
    char* request = args->request;
    int client_socket = args->client_socket;
    CacheItem* item = args->item;
    
    char* host = extract_host(request, 50);
    char* url = extract_url(request);
    
    if (!host || !url) {
        printf("Error: Could not extract host or URL\n");
        
        pthread_mutex_lock(&item->elem_mutex);
        item->is_error = 1;
        item->is_loading = 0;
        pthread_cond_broadcast(&item->loading_cond);
        pthread_mutex_unlock(&item->elem_mutex);
        
        free(host);
        free(url);
        free(request);
        free(args);
        close(client_socket);
        return NULL;
    }

    // Инициализируем память для данных
    pthread_mutex_lock(&item->elem_mutex);
    item->data->memory = (char*)calloc(CACHE_BUFFER_SIZE, sizeof(char));
    item->data->size = 0;
    pthread_mutex_unlock(&item->elem_mutex);

    // Подключаемся к целевому серверу
    int dest_socket = connect_to_remote(host);
    if (dest_socket == -1) {
        printf("Destiny socket error\n");
        
        pthread_mutex_lock(&item->elem_mutex);
        item->is_error = 1;
        item->is_loading = 0;
        if (item->data->memory != NULL) {
            free(item->data->memory);
            item->data->memory = NULL;
        }
        pthread_cond_broadcast(&item->loading_cond);
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
        item->is_loading = 0;
        if (item->data->memory != NULL) {
            free(item->data->memory);
            item->data->memory = NULL;
        }
        pthread_cond_broadcast(&item->loading_cond);
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
    int first_chunk = 1;

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

        // Проверяем статус ответа в первом чанке
        if (first_chunk) {
            if (!is_response_status_ok(buffer)) {
                printf("Server returned error, not saving to cache.\n");
                
                pthread_mutex_lock(&item->elem_mutex);
                item->is_error = 1;
                item->is_loading = 0;
                if (item->data->memory != NULL) {
                    free(item->data->memory);
                    item->data->memory = NULL;
                }
                pthread_cond_broadcast(&item->loading_cond);
                pthread_mutex_unlock(&item->elem_mutex);
                
                break;
            }
            first_chunk = 0;
        }

        // Проверяем размер данных
        if (all_bytes_read + bytes_read > CACHE_BUFFER_SIZE) {
            printf("Data size exceeds CACHE_BUFFER_SIZE! Not saving to cache.\n");
            
            pthread_mutex_lock(&item->elem_mutex);
            item->is_size_full = 1;
            pthread_mutex_unlock(&item->elem_mutex);
            
            break;
        }

        // Сохраняем данные в кеш
        pthread_mutex_lock(&item->elem_mutex);
        if (item->data->memory != NULL) {
            memcpy(item->data->memory + all_bytes_read, buffer, bytes_read);
            item->data->size += bytes_read;
        }
        pthread_mutex_unlock(&item->elem_mutex);

        all_bytes_read += bytes_read;
    }

    // Завершаем загрузку
    pthread_mutex_lock(&item->elem_mutex);
    item->is_loading = 0;
    
    if (item->is_size_full || item->is_error) {
        printf("Error appeared while reading the response, freeing memory...\n");
        if (item->data->memory != NULL) {
            free(item->data->memory);
            item->data->memory = NULL;
            item->data->size = 0;
        }
    } else {
        printf("Data added to cache with size %zu\n\n", item->data->size);
    }
    
    // Будим все потоки, ожидающие завершения загрузки
    pthread_cond_broadcast(&item->loading_cond);
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
        free(buffer);
        close(client_socket);
        return;
    }

    char* url = extract_url(buffer);
    if (url == NULL) {
        printf("Could not extract URL from request\n");
        free(buffer);
        close(client_socket);
        return;
    }

    printf("Request URL: %s\n", url);

    // Атомарно находим или добавляем URL в кеш
    CacheItem* item = atomic_find_or_add_url(cache, url);
    
    pthread_mutex_lock(&item->elem_mutex);
    
    if (item->is_loading) {
        // Данные загружаются другим потоком, ждем
        printf("Data is loading by other client. Waiting...\n");
        
        while (item->is_loading) {
            pthread_cond_wait(&item->loading_cond, &item->elem_mutex);
        }
        
        // Проверяем результат загрузки
        if (item->is_error || item->data->memory == NULL) {
            printf("Error occurred while loading data\n");
            pthread_mutex_unlock(&item->elem_mutex);
            free(buffer);
            free(url);
            close(client_socket);
            return;
        }
        
        printf("Sending cached data to client\n");
        if (item->data->size > 0) {
            send_to(client_socket, item->data->memory, item->data->size);
        }
        
    } else if (item->data->memory != NULL && item->data->size > 0) {
        // Данные уже загружены в кеш
        printf("Sending existing cached data to client\n");
        send_to(client_socket, item->data->memory, item->data->size);
        
    } else {
        // Нужно начать загрузку
        printf("Data not found in cache. Fetching from remote server...\n");
        
        // Помечаем как загружаемое
        item->is_loading = 1;
        pthread_mutex_unlock(&item->elem_mutex);
        
        // Запускаем поток загрузки
        pthread_t tid;
        ThreadArgs* thread_args = (ThreadArgs*)malloc(sizeof(ThreadArgs));
        thread_args->cache = cache;
        thread_args->request = strdup(buffer);
        thread_args->client_socket = client_socket;
        thread_args->item = item;
        
        printf("Initializing new downloading thread\n");
        
        int err = pthread_create(&tid, NULL, &fetch_and_cache_data, thread_args);
        if (err != 0) {
            fprintf(stderr, "Error creating thread: %s\n", strerror(err));
            
            pthread_mutex_lock(&item->elem_mutex);
            item->is_loading = 0;
            item->is_error = 1;
            pthread_cond_broadcast(&item->loading_cond);
            pthread_mutex_unlock(&item->elem_mutex);
            
            free(thread_args->request);
            free(thread_args);
            free(buffer);
            free(url);
            close(client_socket);
            return;
        }
        
        pthread_detach(tid);
        
        // Ждем завершения загрузки
        pthread_mutex_lock(&item->elem_mutex);
        while (item->is_loading) {
            pthread_cond_wait(&item->loading_cond, &item->elem_mutex);
        }
        
        // Отправляем загруженные данные
        if (!item->is_error && item->data->memory != NULL && item->data->size > 0) {
            printf("Sending freshly loaded data to client\n");
            send_to(client_socket, item->data->memory, item->data->size);
        }
    }
    
    pthread_mutex_unlock(&item->elem_mutex);
    
    free(buffer);
    free(url);
    close(client_socket);
}

// Извлекаем URL из HTTP-запроса
char* extract_url(char* request) {
    const char* method_end = strstr(request, " "); 
    if (!method_end) {
        return NULL;
    }

    // Конец URL
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
    return strstr(buffer, "HTTP/1.0 200 OK") != NULL || 
           strstr(buffer, "HTTP/1.1 200 OK") != NULL ||
           strstr(buffer, "HTTP/1.1 200") != NULL;
}
