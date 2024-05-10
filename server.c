#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "./comm.h"
#include "./db.h"
#include "./server.h"

client_t *thread_list_head;
pthread_mutex_t thread_list_mutex = PTHREAD_MUTEX_INITIALIZER;
client_control_t cl_ctrl = {PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER,
                            0};
server_control_t sv_ctrl = {PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER,
                            0};
// flag to mark if server has stopped accepting clients
int stop_accepting = 0;

//------------------------------------------------------------------------------------------------
// Client threads' constructor and main method

// Called by listener (in comm.c) to create a new client thread
void client_constructor(FILE *cxstr)
{
    client_t *client = (client_t *)malloc(sizeof(client_t));
    if (client == NULL)
    {
        perror("malloc");
        exit(1);
    }
    client->cxstr = cxstr;
    client->next = NULL;
    client->prev = NULL;

    int err;
    if ((err = pthread_create(&client->thread, 0, run_client,
                              (void *)client)) != 0)
    {
        handle_error_en(err, "pthread_create error");
    }

    int detach_err;
    if ((detach_err = pthread_detach(client->thread)) != 0)
    {
        handle_error_en(detach_err, "pthread_detach error");
    }
}

// Code executed by a client thread
void *run_client(void *arg)
{
    client_t *client = (client_t *)arg;

    if (stop_accepting)
    {
        client_destructor(client);
        return NULL;
    }

    pthread_mutex_lock(&thread_list_mutex);
    if (thread_list_head == NULL)
    {
        thread_list_head = client;
    }
    else
    {
        client_t *cur = thread_list_head;
        while (cur->next != NULL)
        {
            cur = cur->next;
        }
        cur->next = client;
        client->prev = cur;
        client->next = NULL;
    }
    pthread_mutex_lock(&sv_ctrl.server_mutex);
    sv_ctrl.num_client_threads++;
    pthread_mutex_unlock(&sv_ctrl.server_mutex);
    pthread_mutex_unlock(&thread_list_mutex);

    char response[BUFLEN];
    memset(response, '\0', BUFLEN);
    char command[BUFLEN];
    memset(command, '\0', BUFLEN);

    pthread_cleanup_push(thread_cleanup, (void *)client);

    while (comm_serve(client->cxstr, response, command) == 0)
    {
        client_control_wait();
        interpret_command(command, response, BUFLEN);
    }
    pthread_cleanup_pop(1);

    return NULL;
}

//------------------------------------------------------------------------------------------------
// Methods for client thread cleanup, destruction, and cancellation

void client_destructor(client_t *client)
{
    comm_shutdown(client->cxstr);
    free(client);
}

// Cleanup routine for client threads, called on cancels and exit.
void thread_cleanup(void *arg)
{
    client_t *client = (client_t *)arg;
    pthread_mutex_lock(&thread_list_mutex);
    if (thread_list_head == client)
    {
        thread_list_head = client->next;
        if (thread_list_head != NULL)
        {
            thread_list_head->prev = NULL;
        }
    }
    else if (client->next == NULL)
    {
        client->prev->next = NULL;
    }
    else
    {
        client->prev->next = client->next;
        client->next->prev = client->prev;
    }
    pthread_mutex_unlock(&thread_list_mutex);
    client_destructor(client);

    pthread_mutex_lock(&sv_ctrl.server_mutex);
    sv_ctrl.num_client_threads--;

    // check if it's the last client
    if (sv_ctrl.num_client_threads == 0)
    {
        int err;
        if ((err = pthread_cond_signal(&sv_ctrl.server_cond)) != 0)
        {
            handle_error_en(err, "pthread_cond_signal err");
        }
    }
    pthread_mutex_unlock(&sv_ctrl.server_mutex);
}

void delete_all()
{
    pthread_mutex_lock(&thread_list_mutex);
    client_t *cur = thread_list_head;
    while (cur != NULL)
    {
        int err;
        if ((err = pthread_cancel(cur->thread)) != 0)
        {
            handle_error_en(err, "pthread_cancel error");
        }
        cur = cur->next;
    }
    pthread_mutex_unlock(&thread_list_mutex);
}

//------------------------------------------------------------------------------------------------
// Methods for stop/go server commands

// Wrapper function around pthread_mutex_unlock for pthread_cleanup_push
void cleanup_unlock_mutex(void *mutex)
{
    pthread_mutex_unlock((pthread_mutex_t *)mutex);
}

// Called by client threads to wait until progress is permitted
void client_control_wait()
{
    pthread_mutex_lock(&cl_ctrl.go_mutex);
    // part 3B
    pthread_cleanup_push(cleanup_unlock_mutex, &cl_ctrl.go_mutex);
    // client call pthread_cond_wait on the condition that all clients are
    // stopped
    while (cl_ctrl.stopped)
    {
        int err_cond_wait;
        if ((err_cond_wait =
                 pthread_cond_wait(&cl_ctrl.go, &cl_ctrl.go_mutex)) != 0)
        {
            handle_error_en(err_cond_wait, "pthread_cond_wait err");
        }
    }
    pthread_cleanup_pop(1);
}

// Called by main thread to stop client threads
void client_control_stop()
{
    pthread_mutex_lock(&cl_ctrl.go_mutex);
    cl_ctrl.stopped = 1;
    pthread_mutex_unlock(&cl_ctrl.go_mutex);
}

// Called by main thread to resume client threads
void client_control_release()
{
    // We want it in client_control_release so that the actions of setting the
    // state to go and signalling the condition variable are atomic.
    pthread_mutex_lock(&cl_ctrl.go_mutex);
    cl_ctrl.stopped = 0;
    int err_cond_broadcast;
    if ((err_cond_broadcast = pthread_cond_broadcast(&cl_ctrl.go)) != 0)
    {
        handle_error_en(err_cond_broadcast, "pthread_cond_broadcast err");
    }
    pthread_mutex_unlock(&cl_ctrl.go_mutex);
}

//------------------------------------------------------------------------------------------------
// SIGINT signal handling

// Code executed by the signal handler thread. 'man 7 signal' and 'man sigwait'
// are both helpful for implementing this function.
// All of the server's client threads should terminate on SIGINT; the server
// (this includes the listener thread), however, should not!
void *monitor_signal(void *arg)
{
    (void)arg;
    sig_handler_t *handler = (sig_handler_t *)arg;
    int sig;
    while (1)
    {
        int err;
        if ((err = sigwait(&handler->set, &sig)) != 0)
        {
            perror("sigwait err");
            exit(1);
        }
        if (sig == SIGINT)
        {
            fprintf(stdout, "SIGINT received, cancelling all clients\n");
            delete_all();
        }
    }
    return NULL;
}

sig_handler_t *sig_handler_constructor()
{
    sig_handler_t *handler = (sig_handler_t *)malloc(sizeof(sig_handler_t));
    if (handler == NULL)
    {
        perror("malloc err");
        exit(1);
    }
    sigemptyset(&handler->set);
    sigaddset(&handler->set, SIGINT);
    int err_sigmask;
    if ((err_sigmask = pthread_sigmask(SIG_BLOCK, &handler->set, NULL)) != 0)
    {
        handle_error_en(err_sigmask, "pthread_sigmask");
    }
    int err_cr;
    if ((err_cr = pthread_create(&handler->thread, 0, monitor_signal,
                                 handler)) != 0)
    {
        handle_error_en(err_cr, "pthread_create err");
    }
    return handler;
}

void sig_handler_destructor(sig_handler_t *sighandler)
{
    (void)sighandler;

    int err_cancel;
    if ((err_cancel = pthread_cancel(sighandler->thread)) != 0)
    {
        handle_error_en(err_cancel, "pthread_cancel err");
    }
    int err_join;
    if ((err_join = pthread_join(sighandler->thread, NULL)) != 0)
    {
        handle_error_en(err_join, "pthread_join err");
    }
    free(sighandler);
}

//------------------------------------------------------------------------------------------------
// Main function

// The arguments to the server should be the port number.
int main(int argc, char *argv[])
{
    if (argc != 2)
    {
        fprintf(stderr, "Usage: ./server <port>\n");
        exit(1);
    }

    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGPIPE);
    int err_sigmsk;
    if ((err_sigmsk = pthread_sigmask(SIG_BLOCK, &set, NULL)) != 0)
    {
        handle_error_en(err_sigmsk, "pthread_sigmask err");
    }
    sig_handler_t *handler = sig_handler_constructor();

    pthread_t listener;
    listener = start_listener(atoi(argv[1]), client_constructor);

    while (1)
    {
        char buf[512];
        char *tokens[512];
        ssize_t n;
        n = read(STDIN_FILENO, buf, sizeof(buf));
        buf[n] = 0;
        if (n == 1 && buf[0] == '\n')
        {
            continue;
        }
        else if (n == 0)
        {
            // EOF
            break;
        }
        else if (n < 0)
        {
            perror("read err");
            exit(1);
        }

        char *token;
        int i = 0;
        char *str = buf;
        while ((token = strtok(str, " \t\n")) != NULL)
        {
            tokens[i] = token;
            i++;
            str = NULL;
        }

        if (strcmp(tokens[0], "p") == 0)
        {
            db_print(tokens[1]);
        }
        else if (strncmp(tokens[0], "s", 1) == 0)
        {
            fprintf(stdout, "stopping all clients\n");
            client_control_stop();
        }
        else if (strncmp(tokens[0], "g", 1) == 0)
        {
            fprintf(stdout, "releasing all clients\n");
            client_control_release();
        }
    }
    sig_handler_destructor(handler);

    stop_accepting = 1;
    delete_all();
    pthread_mutex_lock(&sv_ctrl.server_mutex);
    pthread_cleanup_push(cleanup_unlock_mutex, &sv_ctrl.server_mutex);
    while (sv_ctrl.num_client_threads > 0)
    {
        int err_wait;
        if ((err_wait = pthread_cond_wait(&sv_ctrl.server_cond,
                                          &sv_ctrl.server_mutex)) != 0)
        {
            handle_error_en(err_wait, "pthread_cond_wait err");
        }
    }
    pthread_cleanup_pop(1);
    db_cleanup();
    pthread_cancel(listener);
    pthread_join(listener, NULL);
    pthread_exit(NULL);

    return 0;
}
