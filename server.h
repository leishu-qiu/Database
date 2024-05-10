#include <pthread.h>

/*
 * Use the variables in this struct to synchronize your main thread with client
 * threads. Note that all client threads must have terminated before you clean
 * up the database.
 */
typedef struct server_control {
    pthread_mutex_t server_mutex;
    pthread_cond_t server_cond;
    int num_client_threads;
} server_control_t;

/*
 * Controls when the clients in the client thread list should be stopped and
 * let go.
 */
typedef struct client_control {
    pthread_mutex_t go_mutex;
    pthread_cond_t go;
    int stopped;
} client_control_t;

/*
 * The encapsulation of a client thread, i.e., the thread that handles
 * commands from clients.
 */
typedef struct client {
    pthread_t thread;
    FILE *cxstr;  // File stream for input and output

    // For client list
    struct client *prev;
    struct client *next;
} client_t;

/*
 * The encapsulation of a thread that handles signals sent to the server.
 * When SIGINT is sent to the server all client threads should be destroyed.
 */
typedef struct sig_handler {
    sigset_t set;
    pthread_t thread;
} sig_handler_t;


// Client threads' constructor and main method
void client_constructor(FILE *cxstr);
void *run_client(void *arg);

// Methods for client thread cleanup, destruction, and cancellation
void client_destructor(client_t *client);
void thread_cleanup(void *arg);
void delete_all();

// Methods for stop/go server commands
void client_control_wait();
void client_control_stop();
void client_control_release();

// SIGINT signal handling
sig_handler_t *sig_handler_constructor();
void *monitor_signal(void *arg);
void sig_handler_destructor(sig_handler_t *sighandler);