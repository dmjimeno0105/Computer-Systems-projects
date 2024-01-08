Student Information
-------------------
nathand
dmjimeno0105
How to execute the shell
------------------------
1. Move to the src directory
2. Input 'make' to compile code
3. Input './cush' in the terminal
4. Enter commands into shell
Important Notes
---------------
We passed all basic and advanced functionality. We also chose to implement custom prompt for the simple builtin. For the advanced builtin we did history.
<Any important notes about your system>
Description of Base Functionality
---------------------------------
<describe your IMPLEMENTATION of the following commands:
jobs, fg, bg, kill, stop, \ˆC, \ˆZ >

Jobs: Iterates through the job list and prints out every job contained.

fg: Finds a job from the jid and gives it terminal control. Also changes the status of a job to FOREGROUND then sends a signal for the job to continue. 
    It prints out the job at the end.

bg: Finds a job from the jid, changes the status to BACKGROUND. Then sends a signal for the job to continue in the background. It then prints out the job.

kill: Takes in the jid and finds the matching job. Changes the job status to KILLED It then sends a signal to kill the job.

stop: Takes in the jid and finds the matching job. Sends a kill signal to the pgrp. Stopping the job. Changes the status of the job to STOPPED.

\^C: The handle_child_status first finds the pid of the process and what job its in. After detecting that \^C has been pressed with WIFSIGNALED(status) and WTERMSIG(status) == SIGINT 
    it changes the status of the job to TERMINATED. Decreasing the number of processes alive.

\^Z: The handle_child_status first finds the pid of the process and what job its in.
After detecting that \^Z has been pressed with WIFSTOPPED(status) and signal == SIGTSTP the job status is changed to STOPPED. It then prints out the job.

Description of Extended Functionality
-------------------------------------
<describe your IMPLEMENTATION of the following functionality:
I/O, Pipes, Exclusive Access >
List of Additional Builtins Implemented
---------------------------------------
I/O: Used pipe->iored_input and pipe->iored_output to find whether io redirection was needed. We did this by checking if they were not NULL. 
     Based on that, we used posix_spawn_file_actions_addopen to redirect the I/O.

Pipes: We used helper methods to divide up the command line so that we can properly deal with jobs and the pipes. The handle_job goes through the pipe connecting
    them by iterating through an array of pipes. Then using helper methods like connect_pipeline and close_pipelines the pipes are connected and closed properly.

Exclusive Access: The handle_child_status first finds the job from the given pid. Then by detecting WIFSTOPPED(status) it has detected that the process has stopped so we save the terminal state by using termstate_save(). 
    After that, we verify that the process needs to regain control of the terminal by checking signal == SIGTTOU || signal == SIGTTIN the status of the job is then changed to NEEDSTERMINAL. 
    When the process regains terminal control using termstate_give_terminal_to(), the saved_tty_state of the respective job is resumed.
(Written by Your Team)
Simple Builtin
    Custom Prompt: Instead of the prompt showing a simple "<cush>" it will show the "user@hostname directory"
Complex Builtin
    History: Using the GNU History library our code implements history features. Such as showing a list of previous commands when 
    inputting "history" into the shell. Up and down arrows to quickly input previous commands. And event designators to 
