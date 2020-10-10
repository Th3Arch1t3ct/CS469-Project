#include <stdio.h>

#include "../globals.h"
#include "login_window.h"
#include "network.h"

void DEBUG_network();

int main(int argc, char** argv){
    printf("Hello from client!\n");

    DEBUG_network();

//    gtk_init(&argc, &argv);
//
//    create_login_ui();
//    gtk_main();

    return 0;
}

void DEBUG_network() {
    char buffer[BUFFER_SIZE];
    if (database_connect(DEFAULT_SERVER, DEFAULT_SERVER_PORT) < 0) {
        fprintf(stderr, "Could not connect to server\n");
    }

    bzero(buffer, BUFFER_SIZE);
    //sprintf(buffer, "AUTH %s %s", "esnyder", "password");
    sprintf(buffer, "AUTH %s %s", "esnyder", "abcd123");
    // sprintf(buffer, "SYNC");

    SSL_write(ssl, buffer, strlen(buffer));

    bzero(buffer, BUFFER_SIZE);
    SSL_read(ssl, buffer, BUFFER_SIZE);

    fprintf(stdout, "MSG Received: %s\n", buffer);

    bzero(buffer, BUFFER_SIZE);
    sprintf(buffer, "GET ALL");
    SSL_write(ssl, buffer, strlen(buffer));

    int rcount = 1;
    fprintf(stdout, "Message Received: ");
    // Need to build one giant string
    size_t cur_size = 1024 * 4;
    size_t mem_size = 1024 * 4;
    char *allItems = (char *) malloc(sizeof(char) * mem_size);
    allItems[0] = '\0'; // Make it empty
    while (rcount > 0) {
        bzero(buffer, BUFFER_SIZE);
        rcount = SSL_read(ssl, buffer, BUFFER_SIZE);
        if (strlen(allItems) + strlen(buffer) >= cur_size) {
            cur_size += mem_size;
            allItems = realloc(allItems, cur_size);
        }
        strncat(allItems, buffer, strlen(buffer));

        fprintf(stdout, "%s\n", buffer);
        if (buffer[strlen(buffer) - 1] == GROUP_SEPARATOR)
            rcount = 0;

    }
    fprintf(stdout, "\nEnd Message\n\n");

    char* token;
    char str2[] = {RECORD_SEPARATOR, '\0'};
    token = strtok(allItems, str2);

    Item *items[MAX_ITEMS] = {0};
    int available = 0;
    Item *newItem;

    while(token != NULL){
        newItem = (Item*)malloc(sizeof(Item));
        deserialize_item(token, newItem);
        items[available++] = newItem;
        token = strtok(NULL, str2);
    }

    for(int i =0; i < available; i++){
        Item* cur = items[i];
        fprintf(stdout, "%s {%d, %d, %d, %d, %d, %f, %d, %s}\n",
                cur->name, cur->armor, cur->health, cur->mana,
                cur->sellPrice, cur->damage, cur->critChance, cur->range,
                cur->description);
    }

    for(int i  = 0; i < available; i++){
        if(items[i] != NULL)
            free(items[i]);
    }
}