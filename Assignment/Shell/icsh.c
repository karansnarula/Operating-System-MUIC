#include <sys/wait.h>
#include <signal.h>
#include <setjmp.h>
#include <sys/types.h>
#include <fcntl.h>
#include <termios.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <readline/readline.h>
#include <unistd.h>

char **get_input(char *);
int cd(char *);
void sigint_handler(int);
int inBackground = 0; //new
static int exit_s;
char * pch;
int counter = 0;

static sigjmp_buf env;
static volatile sig_atomic_t jump_active = 0;

/* A process is a single process.  */
typedef struct process
{
  struct process *next;       /* next process in pipeline */
  char **argv;                /* for exec */
  pid_t pid;                  /* process ID */
  char completed;             /* true if process has completed */
  char stopped;               /* true if process has stopped */
  int status;                 /* reported status value */
} process;

/* A job is a pipeline of processes.  */
typedef struct job
{
  struct job *next;           /* next active job */
  char *command;              /* command line, used for messages */
  process *first_process;     /* list of processes in this job */
  pid_t pgid;                 /* process group ID */
  char notified;              /* true if user told about stopped job */
  struct termios tmodes;      /* saved terminal modes */
  int stdin, stdout, stderr;  /* standard i/o channels */
} job;

/* The active jobs are linked into a list.  This is its head.   */
job *first_job = NULL;

/* Find the active job with the indicated pgid.  */
job *
find_job (pid_t pgid)
{
  job *j;

  for (j = first_job; j; j = j->next)
    if (j->pgid == pgid)
      return j;
  return NULL;
}

/* Return true if all processes in the job have stopped or completed.  */
int
job_is_stopped (job *j)
{
  process *p;

  for (p = j->first_process; p; p = p->next)
    if (!p->completed && !p->stopped)
      return 0;
  return 1;
}

/* Return true if all processes in the job have completed.  */
int
job_is_completed (job *j)
{
  process *p;

  for (p = j->first_process; p; p = p->next)
    if (!p->completed)
      return 0;
  return 1;
}

/* Keep track of attributes of the shell.  */

#include <sys/types.h>
#include <termios.h>
#include <unistd.h>

pid_t shell_pgid;
struct termios shell_tmodes;
int shell_terminal;
int shell_is_interactive;


/* Make sure the shell is running interactively as the foreground job
   before proceeding. */

void
init_shell ()
{

  /* See if we are running interactively.  */
  shell_terminal = STDIN_FILENO;
  shell_is_interactive = isatty (shell_terminal);

  if (shell_is_interactive)
    {
      /* Loop until we are in the foreground.  */
      while (tcgetpgrp (shell_terminal) != (shell_pgid = getpgrp ()))
        kill (- shell_pgid, SIGTTIN);

      /* Ignore interactive and job-control signals.  */
      signal (SIGINT, SIG_IGN);
      signal (SIGQUIT, SIG_IGN);
      signal (SIGTSTP, SIG_IGN);
      signal (SIGTTIN, SIG_IGN);
      signal (SIGTTOU, SIG_IGN);
      signal (SIGCHLD, SIG_IGN);

      /* Put ourselves in our own process group.  */
      shell_pgid = getpid ();
      if (setpgid (shell_pgid, shell_pgid) < 0)
        {
          perror ("Couldn't put the shell in its own process group");
          exit (1);
        }

      /* Grab control of the terminal.  */
      tcsetpgrp (shell_terminal, shell_pgid);

      /* Save default terminal attributes for shell.  */
      tcgetattr (shell_terminal, &shell_tmodes);
    }
}

void
launch_process (process *p, pid_t pgid,
                int infile, int outfile, int errfile,
                int foreground)
{
  pid_t pid;

  if (shell_is_interactive)
    {
      /* Put the process into the process group and give the process group
         the terminal, if appropriate.
         This has to be done both by the shell and in the individual
         child processes because of potential race conditions.  */
      pid = getpid ();
      if (pgid == 0) pgid = pid;
      setpgid (pid, pgid);
      if (foreground)
        tcsetpgrp (shell_terminal, pgid);

      /* Set the handling for job control signals back to the default.  */
      signal (SIGINT, SIG_DFL);
      signal (SIGQUIT, SIG_DFL);
      signal (SIGTSTP, SIG_DFL);
      signal (SIGTTIN, SIG_DFL);
      signal (SIGTTOU, SIG_DFL);
      signal (SIGCHLD, SIG_DFL);
    }

  /* Set the standard input/output channels of the new process.  */
  if (infile != STDIN_FILENO)
    {
      dup2 (infile, STDIN_FILENO);
      close (infile);
    }
  if (outfile != STDOUT_FILENO)
    {
      dup2 (outfile, STDOUT_FILENO);
      close (outfile);
    }
  if (errfile != STDERR_FILENO)
    {
      dup2 (errfile, STDERR_FILENO);
      close (errfile);
    }

  /* Exec the new process.  Make sure we exit.  */
  execvp (p->argv[0], p->argv);
  perror ("execvp");
  exit (1);
}

/* Put job j in the foreground.  If cont is nonzero,
   restore the saved terminal modes and send the process group a
   SIGCONT signal to wake it up before we block.  */

void
wait_for_job (job *j)
{
  int status;
  pid_t pid;

  do
    pid = waitpid (WAIT_ANY, &status, WUNTRACED);
  while (!mark_process_status (pid, status)
         && !job_is_stopped (j)
         && !job_is_completed (j));

  exit_s = status;
}

void
put_job_in_foreground (job *j, int cont)
{
  /* Put the job into the foreground.  */
  tcsetpgrp (shell_terminal, j->pgid);

  /* Send the job a continue signal, if necessary.  */
  if (cont)
    {
      tcsetattr (shell_terminal, TCSADRAIN, &j->tmodes);
      if (kill (- j->pgid, SIGCONT) < 0)
        perror ("kill (SIGCONT)");
    }

  /* Wait for it to report.  */
  wait_for_job (j);

  /* Put the shell back in the foreground.  */
  tcsetpgrp (shell_terminal, shell_pgid);

  /* Restore the shell’s terminal modes.  */
  tcgetattr (shell_terminal, &j->tmodes);
  tcsetattr (shell_terminal, TCSADRAIN, &shell_tmodes);
}

/* Put a job in the background.  If the cont argument is true, send
   the process group a SIGCONT signal to wake it up.  */

void
put_job_in_background (job *j, int cont)
{
  /* Send the job a continue signal, if necessary.  */
  if (cont)
    if (kill (-j->pgid, SIGCONT) < 0)
      perror ("kill (SIGCONT)");
}


/* Store the status of the process pid that was returned by waitpid.
   Return 0 if all went well, nonzero otherwise.  */

int
mark_process_status (pid_t pid, int status)
{
  job *j;
  process *p;

  if (pid > 0)
    {
      /* Update the record for the process.  */
      for (j = first_job; j; j = j->next)
        for (p = j->first_process; p; p = p->next)
          if (p->pid == pid)
            {
              p->status = status;
              if (WIFSTOPPED (status))
                p->stopped = 1;
              else
                {
                  p->completed = 1;
                  if (WIFSIGNALED (status))
                    fprintf (stderr, "%d: Terminated by signal %d.\n",
                             (int) pid, WTERMSIG (p->status));
                }
              return 0;
             }
      fprintf (stderr, "No child process %d.\n", pid);
      return -1;
    }
  else {
    /* Other weird errors.  */
    perror ("waitpid");
    return -1;
  }
}

/* Check for processes that have status information available,
   without blocking.  */

void
update_status (void)
{
  int status;
  pid_t pid;

  do
    pid = waitpid (WAIT_ANY, &status, WUNTRACED|WNOHANG);
  while (!mark_process_status (pid, status));
}

/* Check for processes that have status information available,
   blocking until all processes in the given job have reported.  */

/* Format information about job status for the user to look at.  */

void
format_job_info (job *j, const char *status)
{
  fprintf (stderr, "%ld (%s): %s\n", (long)j->pgid, status, j->command);
}

/* Notify the user about stopped or terminated jobs.
   Delete terminated jobs from the active job list.  */

/* Mark a stopped job J as being running again.  */

void
mark_job_as_running (job *j)
{
  process *p;

  for (p = j->first_process; p; p = p->next)
    p->stopped = 0;
  j->notified = 0;
}

/* Continue the job J.  */

void
continue_job (job *j, int foreground)
{
  mark_job_as_running (j);
  if (foreground)
    put_job_in_foreground (j, 1);
  else
    put_job_in_background (j, 1);
}





// void output_redirection(char ** arguements,int n)
// {
//      int i;
//      int fd;
//      for(i=0;i<n;i++)
//     if(strcmp(arguements[i],">")==0)      //finding the position of ">"
//     {
//          fd=open(arguements[i+1],O_CREAT | O_WRONLY,0777);
//          arguements[i]='\0';
//          break;
//     }
//      int stdout_original=dup(1);        //saving the original stdout
//      dup2(fd,1);            //redirecting stdout to the fd
//      execute_command(*arguements,arguements,1);
//      dup2(stdout_original,1);         //restoring the original stdout
// }






int main() {
    char **command;
    char *input;
    pid_t child_pid;
    int stat_loc;
    pid_t jobs[1000];
    int index = 0;
    // process *p;

    /* Setup SIGINT */
    struct sigaction s;
    s.sa_handler = sigint_handler;
    sigemptyset(&s.sa_mask);
    s.sa_flags = SA_RESTART;
    sigaction(SIGINT, &s, NULL);

    while (1) {
        if (sigsetjmp(env, 1) == 42) {
            printf("\n");
            continue;
        }
        jump_active = 1;
        inBackground = 0;

        input = readline("icsh> ");
        if (input == NULL) {  /* Exit on Ctrl-D */
            printf("\n");
            exit(0);
        }

        command = get_input(input);

        if (!command[0]) {      /* Handle empty commands */
            free(input);
            free(command);
            continue;
        }

        if(strcmp(command[0], "Exit") == 0 && !inBackground){ //new
            printf("\n");
            exit(0);
        }

        if((strcmp(command[0], "echo") == 0 && strcmp(command[1], "$?") == 0) && !inBackground){ //new
            printf("status: %d\n",exit_s);
        }

        // if(strcmp(command[1],">") == 0 && !inBackground){
        //   int fd;
        //   int ret;
        //   fd = open(command[2],O_CREAT | O_APPEND | O_WRONLY);
        //   if(fd < 0){
        //     perror("open");
        //     exit(1);
        //   }
        //   ret = dup2(fd,1);
        //   if(ret < 0){
        //     perror("dup2");
        //     exit(1);
        //   }
        //   close(fd);
        // }

        // if(strcmp(command[1],"<") == 0 && !inBackground){
            
        // }

        if (strcmp(command[0], "cd") == 0 && !inBackground) { //new
            if (cd(command[1]) < 0) {
                perror(command[1]);
            }
            /* Skip the fork */
            continue;
        }

        child_pid = fork();
        if (child_pid < 0) {
            perror("Fork failed");
            exit(1);
        }

        if (child_pid == 0) {
            struct sigaction s_child;
            s_child.sa_handler = sigint_handler;
            sigemptyset(&s_child.sa_mask);
            s_child.sa_flags = SA_RESTART;
            sigaction(SIGINT, &s_child, NULL);

            /* Never returns if the call is successful */
            if (execvp(command[0], command) < 0) {
                perror(command[0]);
                exit(1);
            }
        } else {
            if(inBackground == 0){ //This is in foreground
                waitpid(child_pid, &stat_loc, WUNTRACED);
            }else{
                jobs[index] = child_pid;
                printf("[%d] %d\n", index + 1 ,child_pid);
                waitpid(child_pid, &stat_loc, WUNTRACED);
                index++;
            }
        }

        if (!input)
            free(input);
        if (!command)
            free(command);
    }

    return 0;
}

char **get_input(char *input) {
    char **command = malloc(100 * sizeof(char *));
    if (command == NULL) {
        perror("malloc failed");
        exit(1);
    }

    char *separator = " ";
    char *parsed;
    int index = 0;

    parsed = strtok(input, separator);
    while (parsed != NULL) {
        if(strcmp(parsed, "&") == 0){
            inBackground = 1;
        }
        command[index] = parsed;
        index++;

        parsed = strtok(NULL, separator);
    }
    command[index] = NULL;
    return command;
}

int cd(char *path) {
    return chdir(path);
}

void sigint_handler(int signo) {
    if (!jump_active) {
        return;
    }
    siglongjmp(env, 42);
}