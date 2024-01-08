/*
 * cush - the customizable shell.
 *
 * Developed by Godmar Back for CS 3214 Summer 2020 
 * Virginia Tech.  Augmented to use posix_spawn in Fall 2021.
 */
#define _GNU_SOURCE    1
#include <stdio.h>
#include <readline/readline.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <sys/wait.h>
#include <assert.h>
// custom includes
#include <sys/stat.h>
#include <fcntl.h>
#include <spawn.h>
#include <readline/history.h>

/* Since the handed out code contains a number of unused functions. */
#pragma GCC diagnostic ignored "-Wunused-function"

#include "termstate_management.h"
#include "signal_support.h"
#include "shell-ast.h"
#include "utils.h"

enum job_status {
    FOREGROUND,     /* job is running in foreground.  Only one job can be
                       in the foreground state. */
    BACKGROUND,     /* job is running in background */
    STOPPED,        /* job is stopped via SIGSTOP */
    NEEDSTERMINAL,  /* job is stopped because it was a background job
                       and requires exclusive terminal access */
    EXITED,
    TERMINATED,
    KILLED,
    DNE,            /* Does Not Exist */
    FPE,            /* Floating Point Exception */
    SEGV,           /* Segmentation Fault*/
    ABRT            /* Aborted */
};

struct job {
    struct list_elem elem;   /* Link element for jobs list. */
    struct ast_pipeline *pipe;  /* The pipeline of commands this job represents */
    int     jid;             /* Job id. */
    enum job_status status;  /* Job status. */ 
    int  num_processes_alive;   /* The number of processes that we know to be alive */
    struct termios saved_tty_state;  /* The state of the terminal when this job was 
                                        stopped after having been in foreground */

    /* Add additional fields here if needed. */
    struct list pids;         // list of pids from job
    pid_t pgrp;               // process group id
};

struct pid {
    pid_t pid;
    struct list_elem elem;
};

static void handle_child_status(pid_t pid, int status);
// custom declerations
static void handle_command_line(struct list *pipes);
static void handle_job(struct ast_pipeline *pipe, struct job *job);
static bool is_builtin(struct ast_command *command);
static void handle_builtin(struct ast_command *command);
static void do_fg(char** argv);
static void do_bg(char** argv);
static void do_jobs();
static void do_stop(char** argv);
static void do_kill(char** argv);
static void do_exit();
static struct job * get_job_from_pid(pid_t pid);
static void open_pipelines(int num_pipelines, int *pipelines);
static void close_pipelines(int num_pipelines, int *pipelines);
static void connect_pipeline(posix_spawn_file_actions_t *file_actions, int i, int num_pipelines, int *pipelines, struct list_elem *e, struct ast_command *command, struct ast_pipeline *pipe);
static bool to_delete_job(struct job *job);
#define NUM_BUILTINS 7
char * builtins[NUM_BUILTINS] = { "jobs", "fg", "bg", "kill", "exit", "stop", "history"};
const size_t INITIAL_CAPACITY = 10;

static void
usage(char *progname)
{
    printf("Usage: %s -h\n"
        " -h            print this help\n",
        progname);

    exit(EXIT_SUCCESS);
}

/* Build a prompt */
static char *
build_prompt(void)
{
    char prompt[100];
    char *username = getenv("USER");
    char host_name[100];
    char system_name[100];
    char directory[100];
    getcwd(directory, 100);
    
    gethostname(host_name,100);

    int i =0;
    while(host_name[i] != '\0' && host_name[i] != '.'){
        system_name[i] = host_name[i];
        i++;
    }
    system_name[i] = '\0';
    snprintf(prompt,300,"<%s@%s %s>$ ",username,system_name,basename(directory));
    return strdup(prompt);
}

/* Utility functions for job list management.
 * We use 2 data structures: 
 * (a) an array jid2job to quickly find a job based on its id
 * (b) a linked list to support iteration
 */
#define MAXJOBS (1<<16)
static struct list job_list;

static struct job * jid2job[MAXJOBS];

/* Return job corresponding to jid */
static struct job * 
get_job_from_jid(int jid)
{
    if (jid > 0 && jid < MAXJOBS && jid2job[jid] != NULL)
        return jid2job[jid];

    return NULL;
}

/* Add a new job to the job list */
static struct job *
add_job(struct ast_pipeline *pipe)
{
    struct job * job = malloc(sizeof *job);
    job->pipe = pipe;
    job->num_processes_alive = 0;
    list_push_back(&job_list, &job->elem);
    for (int i = 1; i < MAXJOBS; i++) {
        if (jid2job[i] == NULL) {
            jid2job[i] = job;
            job->jid = i;
            return job;
        }
    }
    fprintf(stderr, "Maximum number of jobs exceeded\n");
    abort();
    return NULL;
}

/* Delete a job.
 * This should be called only when all processes that were
 * forked for this job are known to have terminated.
 */
static void
delete_job(struct job *job)
{
    int jid = job->jid;
    assert(jid != -1);
    jid2job[jid]->jid = -1;
    jid2job[jid] = NULL;
    ast_pipeline_free(job->pipe);
    struct list_elem * e = list_begin(&job->pids); 
    for (; e != list_end(&job->pids);) {
        struct pid *child = list_entry(e, struct pid, elem);
        e = list_remove(e);
        free(child);
    }
    free(job);
}

static void delete_all_finished_jobs(struct list *job_list);
static void delete_all_finished_jobs(struct list *job_list) {
    struct list_elem * e = list_begin(job_list); 
    for (; e != list_end(job_list);) {
        struct job *curr = list_entry(e, struct job, elem);
        if (to_delete_job(curr)) {
            e = list_remove(e); // remove from job_list
            delete_job(curr); // remove from jid2job
        }
        else {
            e = list_next(e);
        }
    }
}

static const char *
get_status(enum job_status status)
{
    switch (status) {
    case FOREGROUND:
        return "Foreground";
    case BACKGROUND:
        return "Running";
    case STOPPED:
        return "Stopped";
    case NEEDSTERMINAL:
        return "Stopped (tty)";
    case EXITED:
        return "Exited";
    case TERMINATED:
        return "Terminated";
    case KILLED:
        return "Killed";
    case FPE:
        return "Floating Point Exception";
    case SEGV:
        return "Segmentation Fault";
    case ABRT:
        return "Aborted";
    default:
        return "Unknown";
    }
}

/* Print the command line that belongs to one job. */
static void
print_cmdline(struct ast_pipeline *pipeline)
{
    struct list_elem * e = list_begin (&pipeline->commands); 
    for (; e != list_end (&pipeline->commands); e = list_next(e)) {
        struct ast_command *cmd = list_entry(e, struct ast_command, elem);
        if (e != list_begin(&pipeline->commands))
            printf("| ");
        char **p = cmd->argv;
        printf("%s", *p++);
        while (*p)
            printf(" %s", *p++);
    }
}

/* Print a job */
static void
print_job(struct job *job)
{
    printf("[%d]\t%s\t\t(", job->jid, get_status(job->status));
    print_cmdline(job->pipe);
    printf(")\n");
}

/*
 * Suggested SIGCHLD handler.
 *
 * Call waitpid() to learn about any child processes that
 * have exited or changed status (been stopped, needed the
 * terminal, etc.)
 * Just record the information by updating the job list
 * data structures.  Since the call may be spurious (e.g.
 * an already pending SIGCHLD is delivered even though
 * a foreground process was already reaped), ignore when
 * waitpid returns -1.
 * Use a loop with WNOHANG since only a single SIGCHLD 
 * signal may be delivered for multiple children that have 
 * exited. All of them need to be reaped.
 */
static void
sigchld_handler(int sig, siginfo_t *info, void *_ctxt)
{
    pid_t child;
    int status;

    assert(sig == SIGCHLD);

    while ((child = waitpid(-1, &status, WUNTRACED|WNOHANG)) > 0) {
        handle_child_status(child, status);
    }
}

/**
 * Returns true if job needs to be deleted
*/
static bool to_delete_job(struct job *job) {
    enum job_status status = job->status;

    switch (status) {
    case EXITED:
        return true;
    case TERMINATED:
        return true;
    case KILLED:
        return true;
    case FPE:
        return true;
    case SEGV:
        return true;
    case ABRT:
        return true;
    case DNE:
        return true;
    default:
        return false;
    }
}

/* Wait for all processes in this job to complete, or for
 * the job no longer to be in the foreground.
 * You should call this function from a) where you wait for
 * jobs started without the &; and b) where you implement the
 * 'fg' command.
 * 
 * Implement handle_child_status such that it records the 
 * information obtained from waitpid() for pid 'child.'
 *
 * If a process exited, it must find the job to which it
 * belongs and decrement num_processes_alive.
 *
 * However, note that it is not safe to call delete_job
 * in handle_child_status because wait_for_job assumes that
 * even jobs with no more num_processes_alive haven't been
 * deallocated.  You should postpone deleting completed
 * jobs from the job list until when your code will no
 * longer touch them.
 *
 * The code below relies on `job->status` having been set to FOREGROUND
 * and `job->num_processes_alive` having been set to the number of
 * processes successfully forked for this job.
 */
static void
wait_for_job(struct job *job)
{
    assert(signal_is_blocked(SIGCHLD));

    while (job->status == FOREGROUND && job->num_processes_alive > 0) {
        int status;

        pid_t child = waitpid(-1, &status, WUNTRACED);

        // When called here, any error returned by waitpid indicates a logic
        // bug in the shell.
        // In particular, ECHILD "No child process" means that there has
        // already been a successful waitpid() call that reaped the child, so
        // there's likely a bug in handle_child_status where it failed to update
        // the "job" status and/or num_processes_alive fields in the required
        // fashion.
        // Since SIGCHLD is blocked, there cannot be races where a child's exit
        // was handled via the SIGCHLD signal handler.
        if (child != -1)
            handle_child_status(child, status);
        else
            utils_fatal_error("waitpid failed, see code for explanation");
    }
}

static void
handle_child_status(pid_t pid, int status)
{
    assert(signal_is_blocked(SIGCHLD));

    /* To be implemented. 
     * Step 1. Given the pid, determine which job this pid is a part of
     *         (how to do this is not part of the provided code.)
     * Step 2. Determine what status change occurred using the
     *         WIF*() macros.
     * Step 3. Update the job status accordingly, and adjust 
     *         num_processes_alive if appropriate.
     *         If a process was stopped, save the terminal state.
     */
    
    struct job *curr = get_job_from_pid(pid);
    if (curr != NULL) {
        if (WIFSTOPPED(status)) {
            int signal = WSTOPSIG(status);
            curr->status = STOPPED;
            termstate_save(&curr->saved_tty_state);
            // for Ctrl-Z
            if (signal == SIGTSTP) {
                print_job(curr);
            }
            // for kill -STOP
            else if (signal == SIGSTOP) {
            }
            // when non-foreground wants terminal access
            else if (signal == SIGTTOU || signal == SIGTTIN) {
                curr->status = NEEDSTERMINAL;
            }
        }
        else if (WIFEXITED(status)) {
            // process exits via exit()
            if (curr->status == FOREGROUND)
                termstate_sample();
            curr->num_processes_alive--;
            if (curr->num_processes_alive == 0)
                curr->status = EXITED;
        }
        else if (WIFSIGNALED(status)) {
            int signal = WTERMSIG(status);
            curr->status = TERMINATED;
            curr->num_processes_alive--;
            // for Ctrl-C
            if (signal == SIGINT) {
            }
            // for kill
            else if (signal == SIGTERM) {
                print_job(curr);
            }
            // for kill -9
            else if (signal == SIGKILL) {
                curr->status = KILLED;
                print_job(curr);
            }
            // process has been terminated (general case)
            else {
                if (signal == SIGFPE) {
                    curr->status = FPE;
                    print_job(curr);
                }
                else if (signal == SIGSEGV) {
                    curr->status = SEGV;
                    print_job(curr);
                }
                else if (signal == SIGABRT) {
                    curr->status = ABRT;
                    print_job(curr);
                }
            }
        }
    }
}

// custom helper functions

/**
 * Get job associated with pid
*/
static struct job * get_job_from_pid(pid_t pid) {
    struct list_elem * e = list_begin(&job_list); 
    for (; e != list_end(&job_list); e = list_next(e)) {
        struct job *job = list_entry(e, struct job, elem);

        struct list_elem *e2 = list_begin(&job->pids);
        for(; e2 != list_end(&job->pids); e2 = list_next(e2)) {
            struct pid *curr = list_entry(e2, struct pid, elem); 
            if (curr->pid == pid)
                return job;
        }
    }

    return NULL;
}

static void
do_fg(char** argv)
{
    struct job *curr = get_job_from_jid(atoi(argv[1]));
    termstate_give_terminal_to(&curr->saved_tty_state, curr->pgrp);
    
     // signal a process to continue
    if (killpg(curr->pgrp, SIGCONT) == 0) {
        print_cmdline(curr->pipe);
        printf("\n");
        curr->status = FOREGROUND;
        // move a background job into the foreground
        signal_block(SIGCHLD);
        wait_for_job(curr);
        signal_unblock(SIGCHLD);
    }
    else
        perror("");
}

static void
do_bg(char** argv)
{
    struct job *curr = get_job_from_jid(atoi(argv[1]));
    curr->status = BACKGROUND;
    // signal a process to continue
    if (killpg(curr->pgrp, SIGCONT) == 0) {
        print_job(curr);
    }
    else
        perror("");
}

static void
do_jobs()
{
    // display the status of jobs that were started in the current shell environment
    struct list_elem * e = list_begin(&job_list); 
    for (; e != list_end(&job_list); e = list_next(e)) {
        struct job *curr = list_entry(e, struct job, elem);
        print_job(curr);
    }
}

static void
do_stop(char** argv)
{
    struct job *curr = get_job_from_jid(atoi(argv[1]));
    if (killpg(curr->pgrp, SIGSTOP) == 0) {
        curr->status = STOPPED;
    }
    else
        perror("");
}

static void
do_kill(char** argv)
{
    struct job *curr = get_job_from_jid(atoi(argv[1]));
    if (killpg(curr->pgrp, SIGKILL) == 0) {
        curr->status = KILLED;
    }
    else
        perror("");
}

static void
do_exit()
{
    exit(0);
}

static void
do_history(){
    HIST_ENTRY ** the_history_list;
    //Counter for 
    int order = 1;
    if(history_list() != NULL){
        the_history_list = history_list();
        //Goes through the char* line in the HIST_ENTRY struct
        for(int i = 0; i < history_length; i++){
            printf("\t%d %s\n",order,the_history_list[i]->line);
            order++;
        }
    }
}

static bool
is_builtin(struct ast_command *command)
{
    int i = 0;
    for (; i < NUM_BUILTINS; i++) {
        if (strcmp(command->argv[0], builtins[i]) == 0)
            return true;
    }
    return false;
}

static void
handle_builtin(struct ast_command *command)
{
    if (strcmp(command->argv[0], "fg") == 0)
        do_fg(command->argv);
    else if (strcmp(command->argv[0], "bg") == 0)
        do_bg(command->argv);
    else if (strcmp(command->argv[0], "jobs") == 0)
        do_jobs();
    else if (strcmp(command->argv[0], "stop") == 0)
        do_stop(command->argv);
    else if (strcmp(command->argv[0], "kill") == 0)
        do_kill(command->argv);
    else if (strcmp(command->argv[0], "exit") == 0)
        do_exit();
    else if (strcmp(command->argv[0], "history") == 0)
        do_history();
}

static void
open_pipelines(int num_pipelines, int *pipelines)
{
    for (int i = 0; i < num_pipelines*2; i+=2) {
        if (pipe2(pipelines+i, O_CLOEXEC) == 0) {
            // pipline now open
        }
        else
            perror("");
    }
}

static void
close_pipelines(int num_pipelines, int *pipelines)
{
    for (int i = 0; i < num_pipelines*2; i++) {
        if (close(pipelines[i]) == 0) {
            // write-end pipeline now closed
        }
        else
            perror("");
    }
}

/**
 * Specific helper function for handle_job()
*/
static void
connect_pipeline(posix_spawn_file_actions_t *file_actions, int i, int num_pipelines, 
int *pipelines, struct list_elem *e, struct ast_command *command, struct ast_pipeline *pipe)
{
    // write end of first pipeline: command01 >>| ...
    if (e == list_front(&pipe->commands)) {
        posix_spawn_file_actions_adddup2(file_actions, pipelines[i+1], STDOUT_FILENO);
    }
    // read end and write end of middle pipelines: command01 |<< ... >>| commandN
    else if (e != list_front(&pipe->commands) && e != list_back(&pipe->commands)) {
        posix_spawn_file_actions_adddup2(file_actions, pipelines[i-2], STDIN_FILENO);
        posix_spawn_file_actions_adddup2(file_actions, pipelines[i+1], STDOUT_FILENO);
    }
    // read end of last pipeline: ... |<< commandN
    else if (e == list_back(&pipe->commands)) {
        posix_spawn_file_actions_adddup2(file_actions, pipelines[i-2], STDIN_FILENO);
    }
}

/**
 * command01 | command02 | command03
*/
static void
handle_job(struct ast_pipeline *pipe, struct job *job)
{
    int num_pipelines = list_size(&pipe->commands)-1;
    int *pipelines = malloc((num_pipelines*2)*sizeof(int));
    open_pipelines(num_pipelines, pipelines);

    int i = 0;
    struct list_elem * e = list_begin(&pipe->commands); 
    for (; e != list_end(&pipe->commands); e = list_next(e)) {
        struct ast_command *command = list_entry(e, struct ast_command, elem);

        posix_spawnattr_t attr;
        posix_spawn_file_actions_t file_actions;
        posix_spawnattr_init(&attr);
        posix_spawn_file_actions_init(&file_actions);
        if (job->status == FOREGROUND) {
            posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETPGROUP | POSIX_SPAWN_TCSETPGROUP);
        }
        else {
            posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETPGROUP);
        }
        if (e == list_front(&pipe->commands)){
            // child process will get new job pgrp id
            posix_spawnattr_setpgroup(&attr, 0);
            if (job->status == FOREGROUND)
                // gives job terminal control
                posix_spawnattr_tcsetpgrp_np(&attr, termstate_get_tty_fd());
        }    
        else {
            // following processes will get same job pgrp id made from "posix_spawnattr_setpgroup(&attr, 0)"
            posix_spawnattr_setpgroup(&attr, job->pgrp);
        }

        // a[i] == *(a+i)
        // connect pipelines
        if (num_pipelines >= 1)
            connect_pipeline(&file_actions, i, num_pipelines, pipelines, e, command, pipe);
        i+=2;


        // setup io redirection
        // command01 < input.txt | ...
        if (pipe->iored_input != NULL && e == list_begin(&pipe->commands)) {
            posix_spawn_file_actions_addopen(&file_actions, STDIN_FILENO, pipe->iored_input, O_RDONLY, S_IRWXU);
        }
        if (pipe->iored_output != NULL && e == list_rbegin(&pipe->commands)) {
            // ... | commandN >> output.txt
            if (pipe->append_to_output) {
                posix_spawn_file_actions_addopen(&file_actions, STDOUT_FILENO, pipe->iored_output, O_CREAT | O_WRONLY | O_APPEND, 0600);
            }
            // ... | commandN > output.txt
            else {
                posix_spawn_file_actions_addopen(&file_actions, STDOUT_FILENO, pipe->iored_output, O_CREAT | O_WRONLY | O_TRUNC, 0600);
            }
        }

        if (command->dup_stderr_to_stdout) {
            posix_spawn_file_actions_adddup2(&file_actions, STDOUT_FILENO, STDERR_FILENO);
        }

        pid_t pid;
        if (e == list_front(&pipe->commands)) {
            if (posix_spawnp(&pid, command->argv[0], &file_actions, &attr, command->argv, environ) == 0) {
                job->pgrp = pid;
            }
            else {
                job->status = DNE;
                fprintf(stderr, "%s: No such file or directory\n", command->argv[0]);
            }
        }
        else {
            if (posix_spawnp(&pid, command->argv[0], &file_actions, &attr, command->argv, environ) == 0) {
            }
            else {
                job->status = DNE;
                fprintf(stderr, "%s: No such file or directory\n", command->argv[0]);
            }
        }
        
        struct pid * child = malloc(sizeof *child);
        child->pid = pid;
        list_push_back(&job->pids, &child->elem);
        job->num_processes_alive++;
        posix_spawnattr_destroy(&attr);
        posix_spawn_file_actions_destroy(&file_actions);
    }

    close_pipelines(num_pipelines, pipelines);
    free(pipelines);
}

/**
 * command01 | command02 | command03; command01 | command02
*/
static void
handle_command_line(struct list *pipes)
{
    struct list_elem * e = list_begin(pipes); 
    for (; e != list_end(pipes); e = list_remove(e)) {
        struct ast_pipeline *pipe = list_entry(e, struct ast_pipeline, elem);
        struct ast_command *command = list_entry(list_front(&pipe->commands), struct ast_command, elem);
        // in this project it is safe to assume that if a builtin is ever run, it always be by itself
        if (is_builtin(command)) {
            handle_builtin(command);
        }
        else {
            struct job *job = add_job(pipe);
            list_init(&job->pids);
            if (pipe->bg_job) {
                termstate_save(&job->saved_tty_state);
                job->status = BACKGROUND;
                handle_job(pipe, job);
                printf("[%d] %d\n", job->jid, job->pgrp);
            } else {
                job->status = FOREGROUND;
                handle_job(pipe, job);
                signal_block(SIGCHLD); // block to avoid race conditions with the asynchronous signal handler
                wait_for_job(job);
                signal_unblock(SIGCHLD);
            }
        }
    }
}

int
main(int ac, char *av[])
{
    int opt;

    /* Process command-line arguments. See getopt(3) */
    while ((opt = getopt(ac, av, "h")) > 0) {
        switch (opt) {
        case 'h':
            usage(av[0]);
            break;
        }
    }
    using_history();
    list_init(&job_list);
    signal_set_handler(SIGCHLD, sigchld_handler);
    termstate_init();

    /* Read/eval loop. */
    for (;;) {

        /* If you fail this assertion, you were about to enter readline()
         * while SIGCHLD is blocked.  This means that your shell would be
         * unable to receive SIGCHLD signals, and thus would be unable to
         * wait for background jobs that may finish while the
         * shell is sitting at the prompt waiting for user input.
         */
        assert(!signal_is_blocked(SIGCHLD));

        /* If you fail this assertion, you were about to call readline()
         * without having terminal ownership.
         * This would lead to the suspension of your shell with SIGTTOU.
         * Make sure that you call termstate_give_terminal_back_to_shell()
         * before returning here on all paths.
         */
        assert(termstate_get_current_terminal_owner() == getpgrp());

        /* Do not output a prompt unless shell's stdin is a terminal */
        char * prompt = isatty(0) ? build_prompt() : NULL;
        char * cmdline = readline(prompt);
        char* history_input;
        free (prompt);

        if (cmdline == NULL)  /* User typed EOF */
            break;


        int expand = history_expand(cmdline, &history_input);
        if(expand < 0 || expand == 2){
            free(history_input);
            continue;
        }
        struct ast_command_line * cline = ast_parse_command_line(history_input);
        add_history(history_input);
        free (cmdline);
        free(history_input);
        if (cline == NULL)                  /* Error in command line */
            continue;
        if (list_empty(&cline->pipes)) {    /* User hit enter */
            ast_command_line_free(cline);
            continue;
        }
        handle_command_line(&cline->pipes);
        delete_all_finished_jobs(&job_list);
        termstate_give_terminal_back_to_shell();
        
        ast_command_line_free(cline);
    }
    return 0;
}
