```
 ____        _        _
|  _ \  __ _| |_ __ _| |__   __ _ ___  ___
| | | |/ _` | __/ _` | '_ \ / _` / __|/ _ \
| |_| | (_| | || (_| | |_) | (_| \__ \  __/
|____/ \__,_|\__\__,_|_.__/ \__,_|___/\___|
```

In this database project, a server is created to manage a database of key-value pairs over a TCP network. Multiple concurrent users are able to connect to the server to search for items in the database, add new entries, and remove existing entries. The database organizes a collection of nodes in a binary search tree, and it applies hand-over-hand fine-grained locking to ensure multiple client threads to operate on different parts of the tree at the same time. The database supports add, remove, and query values request from client threads.

# server.c
The usage of server is "./server <port number>". In server.c, a listener thread is created to listen for incoming client connections and handle user input. The port number is associated with a socket, which a client uses to establish a connection with server. The server supports following commands: "p" for printing the binary search tree to stdout or "p <file>" to a specified text file; "s" for stopping all the client thread; "g" for resuming all the client threads. SIGPIPE is blocked in the server to make sure termination of client will not cause server to abort. A SIGINT signal handler is created using sig_handler_constructor() to monitor SIGINT. Upon receiving the signal, all clients are terminated, but the server can still receive new connections. Upon receiving EOF, the signal handler is destroyed, the "stop_accepting" fag will be set, and the server will wait for all the client threads to finish by using pthread_cond_wait(). When there's no active client, the database is cleaned up and the listener is canceled and joined. 

# additional helper function
An additional helper function in server.c is cleanup_unlock_mutex(), which is a wrapper function around pthread_mutex_unlock() to be called by pthread_cleanup_push(). It takes an argument mutex to be passed into pthread_mutex_unlock().

# db.c
The client interface allows the following commands, which are supported by the database: a <key> <value> to add new pair, q <key> to query value, d <key> to delete, and f <file> to executes the sequence of commands contained in the file. The database allows multiple threads to add, remove, and query at the same time by hand-over-hand fine-grained locking implementation.

# function signature change
Since add, remove, and query all call search() in db.c, the signature of search() is changed to be able to identify if the caller wants to search through the tree or modify the tree. The new signature is: node_t *search(char *key, node_t *parent, node_t **parentp, int rw). The last argument is an integer indicating read or write type. 


