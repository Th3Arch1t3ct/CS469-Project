#define _GNU_SOURCE
#include <stdio.h>
#include <argp.h>
#include <stdlib.h>
#include <pthread.h>
#include <sqlite3.h>
#include <fcntl.h>
#include <crypt.h>

#include "../globals.h"
#include "network.h"
#include "queue.h"

void *handle_database_thread(void *data);
void *client_thread(void *data);
void *timer_thread_handler(void *data);
int db_login(sqlite3 *db, char *username, char *password);
int authenticate(const char *hash, char *password);
static error_t parse_args(int key, char *arg, struct argp_state *state);
int parse_conf_file(void *args);
int parse_interval(char *interval);
char * marshalItems(sqlite3_stmt *stmt);
void new_item_from_row(sqlite3_stmt * stmt, Item * item);

struct Arguments {
    int listenPort;
    char *server;
    int backupPort;
    char *backupPsk;
    char *filename;
    char *database;
    int interval;
};

typedef struct {
    pthread_t thread_id;
    int socketfd;
    int open;
    SSL_CTX* ctx;
    struct queue_root* queue;
} client_data;

typedef struct {
    struct queue_root* queue;
    char *database;
    char *backupServer;
    int backupPort;
    char *backupPsk;
} db_info;

typedef struct {
    struct queue_root* queue;
    int interval;
} timer_info;

static struct argp_option options[] = {
        {"listen-port",'l',"<port>", 0, "Port to listen on. Default: 4466"},
        {"backup-inventoryserver", 's', "<inventoryserver>", 0, "Server to backup to. Default: localhost"},
        {"backup-port", 'p', "<port>", 0, "Port of backup inventoryserver. Default: 6644"},
        {"backup-key", 'k', "<key>", 0, "Pre-shared key to authenticate to backup server."},
        {"config", 'c', "<filename>", 0, "A config file that can be used in lieu of CLI arguments. This will override all CLI arguments."},
        {"database", 'd', "<filename>", 0, "SQLite 3 database file to use for the application. Default: items.db"},
        {"backup-interval",'i',"<n:H>", 0, "How frequently to backup the database. The time format is time:unit. Acceptable units are [H]ours, [m]inutes, [s]econds. Default: 24:H"},
        {0}
};


/**
 * Main server method.
 *  Reads arguments and confic file
 *  spawns DB thread
 *  sets up network listener
 *  Spawns new thread for every connected client
 */
struct argp argp = { options, parse_args, 0, "A program to manage remote database queries."};
int main(int argc, char *argv[]){

    struct Arguments arguments = {0};
    SSL_CTX *ssl_ctx;

    client_data clients[MAX_CLIENTS];
    pthread_t database_thread;
    pthread_t timer_thread;
    struct queue_root *db_queue;
    int err, i;

    arguments.listenPort = DEFAULT_SERVER_PORT;
    arguments.server = DEFAULT_SERVER;
    arguments.backupPort = DEFAULT_BACKUP_PORT;
    arguments.backupPsk = "";
    arguments.database = DEFAULT_DATABASE;
    arguments.interval = DEFAULT_INTERVAL;
    arguments.filename = NULL;

    argp_parse(&argp, argc, argv, 0, 0, &arguments);
    if(arguments.filename != NULL) parse_conf_file(&arguments);

    printf("Hello from inventoryserver!\n");
    printf("Provided args:\n");
    printf("\tListen port: %d\n", arguments.listenPort);
    printf("\tServer: %s:%d\n", arguments.server, arguments.backupPort);
    printf("\tConfig file: %s\n", arguments.filename ? arguments.filename: "NULL");
    printf("\tBackup interval: %d seconds\n", arguments.interval);

    // Initializing global writer queue
    db_queue = ALLOC_QUEUE_ROOT();
    struct queue_head *sample_item = malloc_aligned(sizeof(struct queue_head));
    INIT_QUEUE_HEAD(sample_item, "INITIALIZATION", NULL);
    queue_put(sample_item, db_queue);

    db_info *info = (db_info*)malloc(sizeof(db_info));
    info->database = arguments.database;
    info->queue = db_queue;
    info->backupServer = arguments.server;
    info->backupPort = arguments.backupPort;
    info->backupPsk = arguments.backupPsk;

    // Need to spawn Database server
    err = pthread_create(&database_thread, NULL, handle_database_thread, (void *)info);
    if(err != 0){
        fprintf(stderr, "Server: Could not initialize database thread: %d\n", err);
        return -1;
    }

    timer_info *timer = (timer_info*)malloc(sizeof(timer_info));
    timer->interval = arguments.interval;
    timer->queue = db_queue;
    err = pthread_create(&timer_thread, NULL, timer_thread_handler, (void*)timer);
    if(err != 0){
        fprintf(stderr, "Server: Could not initialize timer thread: %d\n", err);
        return -1;
    }

    init_openssl();
    // init_locks();
    ssl_ctx = create_new_context();
    configure_context(ssl_ctx);

    // Initialize clients
    for(i = 0;i < MAX_CLIENTS; i++){
        clients[i].open = 1;
        clients[i].ctx = ssl_ctx;
        clients[i].queue = db_queue;
    }

    unsigned int sockfd;

    sockfd = create_socket(arguments.listenPort);
    if(sockfd < 0){
        fprintf(stderr, "Could not create socket\n");
        exit(-1);
    }
    fprintf(stdout, "Server: Listening for network connections!\n");
    while(true){
        int client;
        struct sockaddr_in addr;
        unsigned int len = sizeof(addr);
        // char client_addr[INET_ADDRSTRLEN];

        client = accept((int)sockfd, (struct sockaddr*)&addr, &len);
        if(client < 0){
            fprintf(stderr, "Failed to accept client\n");
            break;
        }

        // Find first open
        // Spawn new Thread with client here
        for(i =0; i < MAX_CLIENTS && !clients[i].open; ++i);
        if(i == MAX_CLIENTS) continue; // Already at max clients

        clients[i].socketfd = client;
        clients[i].queue = db_queue;
        err = pthread_create(&clients[i].thread_id, NULL, client_thread, (void*)&(clients[i]));
        pthread_detach(clients[i].thread_id);
    }

    SSL_CTX_free(ssl_ctx);
    cleanup_openssl();
    close(sockfd);

    return 0;
}

/**
 * This thread handles all of the database operations. This includes, all standard CRUD operations
 * as well as the periodic synchronization with the datastore server.
 * @param data
 * @return
 */
void *handle_database_thread(void *data){
    db_info *info = (db_info*)data;
    struct queue_root *db_queue = info->queue;

    char request_data[BUFFER_SIZE];

    // Setup Database for operations
    char *valid_schema_query = "SELECT name FROM sqlite_master WHERE type='table' AND name='items' OR name='users'";
    sqlite3 *db = NULL;
    int retCode;
    sqlite3_stmt *stmt = NULL;

    retCode = sqlite3_open(info->database, &db);
    if(retCode != SQLITE_OK){
        fprintf(stderr, "FATAL: Cannot open database: %s\n", sqlite3_errmsg(db));
        sqlite3_close(db);
        exit(-1);
    }
    fprintf(stdout, "Server: Database opened!\n");

    retCode = sqlite3_prepare(db, valid_schema_query, -1, &stmt, 0);
    if(retCode != SQLITE_OK || stmt == NULL){
        fprintf(stderr, "Database: Invalid database schema. Missing user or items table");
        sqlite3_close(db);
        exit(-1);
    }

    // Check that the database has the correct schema. Doesn't check column names
    retCode = sqlite3_step(stmt);
    while(retCode != SQLITE_DONE){
        const char *table = (const char *)sqlite3_column_text(stmt, 0);
        if(strcmp(table, "items") != 0 && strcmp(table, "users") != 0){
            fprintf(stderr, "Database: Invalid schema. Missing user or items table");
            sqlite3_close(db);
            exit(-1);
        }
        retCode = sqlite3_step(stmt);
    }

    // Read the database every 10ms, operate if actions

    // Operations will be GET, PUT, DEL, and MOD[ify]
    // SYNC, AUTH, and TERM are also available.
    int flag= 1;
    while(flag){
        struct queue_head *msg = queue_get(db_queue);

        if(msg != NULL){
            char username[BUFFER_SIZE];
            char password[BUFFER_SIZE];

            // Only allocate a response if we have a valid message
            struct queue_head *response = malloc(sizeof(struct queue_head));

            if(sscanf(msg->operation, "AUTH %s %s", username, password) == 2){
                fprintf(stdout, "DB_THREAD: Authenticating user\n");
                retCode = db_login(db, username, password);
                if(retCode != 0)
                    INIT_QUEUE_HEAD(response, "FAILURE", NULL);
                else
                    INIT_QUEUE_HEAD(response, "SUCCESS", NULL);
            }

            if(sscanf(msg->operation, "GET %s", request_data) == 1) {
                // GET all items
                if (strcmp(request_data, "ALL") == 0) {
                    const char *sql = "SELECT * FROM items";
                    sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);

                    // marshal directly into string:
                    char * result = marshalItems(stmt);
                    INIT_QUEUE_HEAD(response, result, NULL);
                    free(result); // INIT_QUEUE_HEAD does a strdup

                    sqlite3_finalize(stmt);
                } else {
                    int id = atoi(request_data);

                    const char *sql = "SELECT * FROM items WHERE id=?";
                    sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
                    sqlite3_bind_int(stmt, 1, id);

                    fprintf(stdout, "Getting 1 where id = %d\n", id);

                    int ret = sqlite3_step(stmt);
                    Item item;
                    if (ret == SQLITE_ROW) {
                        new_item_from_row(stmt, &item);
                        // item found: serialize it into response

                        char* itemInfo = NULL;
                        itemInfo = serialize_item(&item, itemInfo);

                        char* responseString;
                        asprintf(&responseString, "SUCCESS\n%s", itemInfo);

                        // serialize_item(&item, request_data + strlen(request_data), BUFFER_SIZE - strlen(request_data));
                        INIT_QUEUE_HEAD(response, responseString, NULL);
                    } else {
                        INIT_QUEUE_HEAD(response, "FAILURE", NULL);
                    }

                    sqlite3_finalize(stmt);
                }
            }

            if(sscanf(msg->operation, "PUT %[^\x1e]", request_data) == 1){
                // Insert new item
                Item item;
                deserialize_item(request_data, &item);

                const char * sql = "INSERT INTO items "
                    "(name, armorPoints, healthPoints, manaPoints, sellPrice,"
                    " damage, critChance, range, description) "
                    "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)";
                sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
                sqlite3_bind_text(stmt, 1, item.name, strlen(item.name), NULL);
                sqlite3_bind_int(stmt, 2, item.armor);
                sqlite3_bind_int(stmt, 3, item.health);
                sqlite3_bind_int(stmt, 4, item.mana);
                sqlite3_bind_int(stmt, 5, item.sellPrice);
                sqlite3_bind_int(stmt, 6, item.damage);
                sqlite3_bind_double(stmt, 7, item.critChance);
                sqlite3_bind_int(stmt, 8, item.range);
                sqlite3_bind_text(stmt, 9, item.description, strlen(item.description), NULL);

                int ret = sqlite3_step(stmt);
                if (ret == SQLITE_DONE) {
                    item.id = sqlite3_last_insert_rowid(db);
                    // success
                    sprintf(request_data, "SUCCESS\n%d", item.id);
                    INIT_QUEUE_HEAD(response, strdup(request_data), NULL);
                }
                else {
                    INIT_QUEUE_HEAD(response, "FAILURE", NULL);
                }

                sqlite3_finalize(stmt);
            }

            if(sscanf(msg->operation, "MOD %[^\x1e]", request_data) == 1){
                // Modify existing item
                Item item;
                deserialize_item(request_data, &item);

                const char * sql = "UPDATE items SET "
                    "name=?, armorPoints=?, healthPoints=?, manaPoints=?, "
                    "sellPrice=?, damage=?, critChance=?, range=?, description=? "
                    "WHERE id=?";
                sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
                sqlite3_bind_text(stmt, 1, item.name, strlen(item.name), NULL);
                sqlite3_bind_int(stmt, 2, item.armor);
                sqlite3_bind_int(stmt, 3, item.health);
                sqlite3_bind_int(stmt, 4, item.mana);
                sqlite3_bind_int(stmt, 5, item.sellPrice);
                sqlite3_bind_int(stmt, 6, item.damage);
                sqlite3_bind_double(stmt, 7, item.critChance);
                sqlite3_bind_int(stmt, 8, item.range);
                sqlite3_bind_text(stmt, 9, item.description, strlen(item.description), NULL);
                sqlite3_bind_int(stmt, 10, item.id);

                int ret = sqlite3_step(stmt);
                if (ret == SQLITE_DONE) {
                    // success
                    INIT_QUEUE_HEAD(response, "SUCCESS", NULL);
                }
                else {
                    // failure
                    INIT_QUEUE_HEAD(response, "FAILURE", NULL);
                }

                sqlite3_finalize(stmt);
            }

            if(sscanf(msg->operation, "DEL %s", request_data) == 1){
                // Delete existing
                int id = atoi(request_data);

                const char * sql = "DELETE FROM items WHERE id=?";
                sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
                sqlite3_bind_int(stmt, 1, id);

                int ret = sqlite3_step(stmt);
                if (ret == SQLITE_DONE) {
                    // success
                    INIT_QUEUE_HEAD(response, "SUCCESS", NULL);
                }
                else {
                    // item not found case
                    INIT_QUEUE_HEAD(response, "FAILURE", NULL);
                }

                sqlite3_finalize(stmt);
            }

            if(sscanf(msg->operation, "TERM %s", request_data) == 1){
                flag = 0;
            }

            if(strcmp(msg->operation, "SYNC") == 0){
                SSL_CTX * ssl_ctx = NULL;
                SSL * ssl = NULL;
                int backupSockFd;
                int dbFileFd = 0;
                char success = 1;

                fprintf(stdout, "Beginning synchronization!\n");

                // NOTE: this doesn't loop. We use it for an early-return on error
                while (1) {
                    // Close database handle.
                    sqlite3_close(db);

                    ssl_ctx = create_new_client_context();
                    ssl = SSL_new(ssl_ctx);

                    // Open connection to remote server
                    backupSockFd = create_client_socket(info->backupServer, info->backupPort);
                    if (backupSockFd < 0) {
                        // error message has already been displayed
                        success = 0;
                        break;
                    }
                    SSL_set_fd(ssl, backupSockFd);
                    if (SSL_connect(ssl) != 1) {
                        fprintf(stderr, "Could not establish secure connection\n");
                        ERR_print_errors_fp(stderr);
                        success = 0;
                        break;
                    }

                    // Open as standard file,
                    dbFileFd = open(info->database, O_RDONLY);
                    if (dbFileFd < 0) {
                        fprintf(stderr, "Error reopening database file for sync: %s\n", strerror(errno));
                        success = 0;
                        break;
                    }

                    char buffer[BUFFER_SIZE];

                    // stream it to the server
                    sprintf(buffer, "REPLICATE %s\n", info->backupPsk);
                    SSL_write(ssl, buffer, strlen(buffer));

                    int rcount;
                    while ((rcount = read(dbFileFd, buffer, BUFFER_SIZE)) > 0) {
                        rcount = SSL_write(ssl, buffer, rcount);
                        if (rcount < 0) {
                            fprintf(stderr, "Error writing to socket\n");
                            ERR_print_errors_fp(stderr);
                            success = 0;
                            break;
                        }
                    }
                    if (rcount < 0)
                        break;
                    SSL_shutdown(ssl);
                    // get success response back
                    while ((rcount = SSL_read(ssl, buffer, BUFFER_SIZE)) > 0)
                        ;

                    if (rcount < 0) {
                        fprintf(stderr, "Error reading from socket\n");
                        ERR_print_errors_fp(stderr);
                        success = 0;
                    }

                    if (strncmp(buffer, "SUCCESS", strlen("SUCCESS")) != 0) {
                        fprintf(stderr, "Non-success response received from server\n");
                        success = 0;
                    }

                    break;
                }

                // shut down connection to remote server
                if (ssl)
                    SSL_free(ssl);
                if (ssl_ctx)
                    SSL_CTX_free(ssl_ctx);
                if(backupSockFd)
                    close(backupSockFd);

                // close file
                if(dbFileFd)
                    close(dbFileFd);

                // reopen as database again.
                retCode = sqlite3_open(info->database, &db);
                if(retCode != SQLITE_OK){
                    fprintf(stderr, "Error opening database after sync\n");
                    success = 0;
                }

                if(success)
                    INIT_QUEUE_HEAD(response, "SUCCESS", NULL);
                else
                    INIT_QUEUE_HEAD(response, "FAILURE", NULL);
            }

            // Response here
            if(msg->response_queue != NULL)
                queue_put(response, msg->response_queue);
            // msg needs to be freed and response should be de-referenced
            free_queue_message(msg);
            response = NULL;
        }

        usleep(10000); // Wait 10 ms between reads
    }

    sqlite3_close(db);

    return NULL;
}

/**
 * Each client gets its own thread to recieve network requests from.
 * Requests are relayed to the database thread through a thread safe queue.
 *
 * @param data
 * @return
 */
void *client_thread(void *data){
    char buffer[BUFFER_SIZE];
    client_data *client_info = (client_data*)data;
    int opFlag = 0;

    SSL *ssl = SSL_new(client_info->ctx);
    if(ssl == NULL){
        fprintf(stderr, "Error creating new SSL\n");
    }
    int socketfd = client_info->socketfd;

    if(SSL_set_fd(ssl, socketfd) < 0){
        fprintf(stderr, "Could not bind to secure socket: %s\n", strerror(errno));
        pthread_exit(NULL);
    }

    if(SSL_accept(ssl) <= 0){
        fprintf(stderr, "Server: Could not establish a secure connection:\n");
        ERR_print_errors_fp(stderr);
        pthread_exit(NULL);
    }

    // Prep the message and return queue
    struct queue_root *msgQueue = ALLOC_QUEUE_ROOT();
    struct queue_head *query = malloc_aligned(sizeof(struct queue_head));

    bzero(buffer, BUFFER_SIZE);
    // Listen for AUTH request
    int rcount = SSL_read(ssl, buffer, BUFFER_SIZE);
    if(rcount < 0){
        fprintf(stderr, "Could not read from client: %s\n", strerror(errno));
    }

    // But for now, lets just send it to the database thread for some PoC
    INIT_QUEUE_HEAD(query, buffer, msgQueue);
    queue_put(query, client_info->queue);
    query = NULL; // Remove reference to it

    // Check the response queue until a response is received;
    struct queue_head *response = malloc_aligned(sizeof(struct queue_head));
    do{
        response = queue_get(msgQueue);
        usleep(10000);
    }while(response == NULL);

    fprintf(stdout, "CLIENT_THREAD_%d Message Received: %s\n", socketfd, response->operation);

    bzero(buffer, BUFFER_SIZE);
    strncpy(buffer, response->operation, BUFFER_SIZE);
    int validLogin = 1;
    if(strcmp("FAILURE", response->operation) == 0){
        validLogin = 0;
    }
    SSL_write(ssl, buffer, (int)strlen(buffer));

    while(validLogin){

        bzero(buffer, BUFFER_SIZE);
        if(SSL_read(ssl, buffer, BUFFER_SIZE) < 0) {
            fprintf(stderr, "Error reading from client: %s\n", strerror(errno));
            validLogin = 0;
            continue;
        }
        if(strlen(buffer) == 0){
            continue;
        }

        fprintf(stdout, "Message received: %s\n", buffer);

        query = (struct queue_head*)malloc(sizeof(struct queue_head));
        INIT_QUEUE_HEAD(query, buffer, msgQueue);
        // fprintf(stdout, "%s\n", query->operation);
        queue_put(query, client_info->queue);


        if(sscanf(query->operation, "GET %s", buffer) == 1){
            opFlag = CLIENT_GET;
        }
        else if(sscanf(query->operation, "PUT %s", buffer) == 1){
            opFlag = CLIENT_PUT;
        }
        else if(sscanf(query->operation, "MOD %s", buffer) == 1){
            opFlag = CLIENT_MOD;
        }
        else if(sscanf(query->operation, "DEL %s", buffer) == 1){
            opFlag = CLIENT_DEL;
        }

        response = NULL;
        do{
            response = queue_get(msgQueue);
            usleep(10000);
        } while( response == NULL);

        switch(opFlag){
            case CLIENT_GET:
            case CLIENT_PUT:
            case CLIENT_MOD:
            case CLIENT_DEL:
                if((rcount = SSL_write(ssl, response->operation, (int)strlen(response->operation))) < 0){
                    fprintf(stderr, "Error writing to client: %s\n", strerror(errno));
                    validLogin = 0;
                    continue;
                }
                fprintf(stdout, "wrote %d bytes\n", rcount);

                break;
            default:
                fprintf(stderr, "Not supported yet\n");
                break;
        }

        // Free the response and get ready for the next one
        free_queue_message(response);
    }

    if(response->operation) {
        if(SSL_write(ssl, response->operation, strlen(response->operation)) < 0){
            fprintf(stderr, "SERVER: Error writing to client: %s\n", strerror(errno));
        }
    }

    SSL_free(ssl);
    close(client_info->socketfd);
    client_info->open = 1;
    pthread_exit(NULL);
}

/**
 * This method is a rudimentary implentation of a backup timer.
 * It sleeps for a time defined by the user, and then sends a message
 * to the database thread to conduct the backup operation.
 *
 * @param data timer info.
 * @return NULL
 */
void* timer_thread_handler(void *data){
    fprintf(stdout, "Initializing Backup thread\n");
    timer_info *info = (timer_info*)data;

    struct queue_head *sync_message = malloc_aligned(sizeof(struct queue_head));

    while(1){
        sleep(info->interval);
        free(sync_message);
        sync_message= malloc_aligned(sizeof(struct queue_head));
        if(sync_message == NULL){
            fprintf(stderr, "Could not create synchronization message");
            exit(-1);
        }
        INIT_QUEUE_HEAD(sync_message, "SYNC", NULL);
        queue_put(sync_message, info->queue);
    }
}

/**
 * Given a username and password, this method gets the selected user's password
 * and checks that the provided password is the same.
 * @param db - handle to the database
 * @param username - user attempting logon
 * @param password - attempted login password
 * @return
 */
int db_login(sqlite3 *db, char *username, char *password) {
    const char *sql = "SELECT password FROM users where username=? LIMIT 1;";
    sqlite3_stmt *stmt;
    int retCode;
    sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    sqlite3_bind_text(stmt, 1, username, strlen(username), NULL);

    retCode = sqlite3_step(stmt);
    if(retCode == SQLITE_DONE){
        // No Such user exists
        return -1;
    }

    char *hash = malloc(sizeof(char)*BUFFER_SIZE);
    strncpy(hash, (char*)sqlite3_column_text(stmt, 0), BUFFER_SIZE);
    sqlite3_finalize(stmt);
    int r = authenticate(hash, password);

    free(hash);

    return r;
}

/**
 * Method that takes hash from database and password from user to
 * Determine if there is a valid login.
 *
 * @param hash
 * @param password]
 * @return
 */
int authenticate(const char *hash, char *password){
    char salt[SALT_LENGTH];
    strncpy(salt, hash, SALT_LENGTH);
    salt[11] = '\0';

    return strncmp(hash, crypt(password, salt), BUFFER_SIZE);
}

/**
 * Parses command line arguments
 * @param key
 * @param arg
 * @param state
 * @return
 */
static error_t parse_args(int key, char *arg, struct argp_state *state){
    struct Arguments *arguments = state->input;
    char *pEnd;
    int interval;


    switch(key){
        case 'l':
            arguments->listenPort = (int)strtol(arg, &pEnd,10);
            break;
        case 's':
            arguments->server = arg;
            break;
        case 'p':
            arguments->backupPort = (int)strtol(arg, &pEnd,10);
            break;
        case 'k':
            arguments->backupPsk = arg;
            break;
        case 'c':
            arguments->filename = arg;
            break;
        case 'd':
            arguments->database = arg;
            break;
        case 'i':
            interval = parse_interval(arg);
            if(interval <= 0){
                fprintf(stderr, "Invalid Timer value %s\n", arg);
            }
            arguments->interval = interval;
            break;
        default:
            return ARGP_ERR_UNKNOWN;
    }

    return 0;
}

/**
 * parse_conf_file reads a user defined file to pull program paramters
 *
 * @param args arguments struct
 * @return
 */
int parse_conf_file(void *args){
    struct Arguments *arguments = (struct Arguments *)args;
    char field[BUFFER_SIZE];
    char value[BUFFER_SIZE];
    int val;
    char *stop;

    FILE *file;
    file = fopen(arguments->filename, "r");
    if(file == NULL){
        fprintf(stderr, "Error opening config file: %s\n", strerror(errno));
        return -1;
    }

    while(fscanf(file, "%127[^=]=%127[^\n]%*c", field, value) == 2){
        stop = NULL;
        if(strcmp(field, "PORT") == 0){
            val = strtol(value, &stop, 10);
            if(stop == NULL || *stop != '\0'){
                fprintf(stderr, "Error interpreting port: %s\n", value);
                fclose(file);
                return -1;
            }
            arguments->listenPort = val;
        }

        if(strcmp(field, "BACKUP_SERVER") == 0){
            arguments->server = strdup(value);
        }

        if(strcmp(field, "BACKUP_PORT") == 0){
            val = strtol(value, &stop, 10);
            if(stop == NULL || *stop != '\0'){
                fprintf(stderr, "Error interpreting Backup server port: %s\n", value);
                fclose(file);
                return -1;
            }
            arguments->backupPort = val;
        }

        if(strcmp(field, "BACKUP_PSK") == 0){
            arguments->backupPsk = strdup(value);
        }

        if(strcmp(field, "DATABASE") == 0){
            arguments->database = strdup(value);
        }

        if(strcmp(field, "INTERVAL") == 0){
            val = parse_interval(value);
            if(val < 0){
                fprintf(stderr, "Invalid time interval: %s\n", value);
                fclose(file);
                return -1;
            }
            arguments->interval = val;
        }

        bzero(field, BUFFER_SIZE);
        bzero(value, BUFFER_SIZE);
    }


    return 0;
}

/**
 * Method to interpret the interval period format
 * @param interval
 * @return
 */
int parse_interval(char* interval){
    // Split on the ':'
    char *stop;
    char *token;
    int num;
    int mult;

    token = strtok(interval, ":");
    num = strtol(token, &stop, 10);
    if(stop == NULL){
        return -1; // Invalid Number
    }
    token = strtok(NULL, ":");
    if(strlen(token) != 1){
        return -1;
    }
    switch(token[0]){
        // Hours
        case 'H':
        case 'h':
            mult = 60*60;
            break;
        // Minutes
        case 'M':
        case 'm':
            mult = 60;
            break;
        // Seconds
        case 'S':
        case 's':
            mult = 1;
            break;
        default:
            return -1;
    }


    return num * mult;
}

/**
 * Marshalls all items from a GET ALL request directly into a serialized message
 * @param stmt
 * @return
 */
char * marshalItems(sqlite3_stmt *stmt){
    size_t cur_size =1024*4;
    size_t mem_size = 1024*4;
    char* result = (char*)malloc(sizeof(char)*mem_size); // Allocate 4Kb
    bzero(result, sizeof(char)*mem_size);
    strncat(result, "SUCCESS ", 9);

    int r;
    r = sqlite3_step(stmt);
    while(r == SQLITE_ROW ){
        char* curItem;
        asprintf(&curItem, "%d\n%s\n%d\n%d\n%d\n%d\n%d\n%f\n%d\n%s%c",
                 sqlite3_column_int(stmt, 0), // id
                 (const char*)sqlite3_column_text(stmt, 1), // name
                 sqlite3_column_int(stmt, 2), // armor
                 sqlite3_column_int(stmt, 3), // health
                 sqlite3_column_int(stmt, 4), // mana
                 sqlite3_column_int(stmt, 5), // sellPrice
                 sqlite3_column_int(stmt, 6), // damage
                 sqlite3_column_double(stmt, 7), // critical
                 sqlite3_column_int(stmt, 8), //range
                 (const char*)sqlite3_column_text(stmt, 9),
                 RECORD_SEPARATOR
         );

        // Need to append
        if(strlen(curItem) + strlen(result) >= cur_size){ // Amount of data is too large, we need to add more memory
            cur_size += mem_size;
            result = realloc(result, cur_size);
        }
        strncat(result, curItem, strlen(curItem));
        free(curItem);
        r = sqlite3_step(stmt);
    }

    // Replace last character with group separator
    // -1 should be \x0, -2 should be RECORD_SEPARATOR
    result[strlen(result)-1] = GROUP_SEPARATOR;

    return result;
}

/**
 * Given a SQLITE_ROW, convert all relevant fields into an Item struct
 * @param stmt
 * @param item
 */
void new_item_from_row(sqlite3_stmt * stmt, Item * item) {
    item->id = sqlite3_column_int(stmt, 0);
    snprintf(item->name, BUFFER_SIZE, "%s", (const char*)sqlite3_column_text(stmt, 1));
    item->armor = sqlite3_column_int(stmt, 2);
    item->health = sqlite3_column_int(stmt, 3);
    item->mana = sqlite3_column_int(stmt, 4); // mana
    item->sellPrice = sqlite3_column_int(stmt, 5); // sell price
    item->damage = sqlite3_column_int(stmt, 6);
    item->critChance = sqlite3_column_double(stmt, 7);
    item->range = sqlite3_column_int(stmt, 8); // range
    snprintf(item->description, BUFFER_SIZE, "%s", (const char*)sqlite3_column_text(stmt, 9));
}
