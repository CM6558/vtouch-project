/* vtouchsupervise: restart the worker with heartbeat monitoring. */
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
extern int vtouchmerge_worker(int heartbeat_fd,int argc,char **argv);
static volatile sig_atomic_t stop_flag;
static void stop(int s){(void)s;stop_flag=1;}
static void sleep_seconds(int seconds){struct timespec ts={seconds,0};while(!stop_flag&&nanosleep(&ts,&ts)<0&&errno==EINTR){}}
int main(int argc,char **argv){int pipefd[2],backoff=1,status;pid_t pid;char c;
 signal(SIGTERM,stop);signal(SIGINT,stop);signal(SIGPIPE,SIG_IGN);
 while(!stop_flag){time_t last_hb;if(pipe(pipefd)<0)return 1;pid=fork();if(pid<0){close(pipefd[0]);close(pipefd[1]);return 1;}
  if(pid==0){close(pipefd[0]);return vtouchmerge_worker(pipefd[1],argc,argv);}
  close(pipefd[1]);if(fcntl(pipefd[0],F_SETFL,O_NONBLOCK)<0){kill(pid,SIGTERM);waitpid(pid,&status,0);close(pipefd[0]);return 1;}
  last_hb=time(NULL);for(;;){ssize_t n=read(pipefd[0],&c,1);if(n>0){last_hb=time(NULL);backoff=1;}if(n<0&&errno!=EAGAIN&&errno!=EINTR)break;if(waitpid(pid,&status,WNOHANG)==pid)break;if(stop_flag){kill(pid,SIGTERM);waitpid(pid,&status,0);break;}if(time(NULL)-last_hb>10){kill(pid,SIGTERM);waitpid(pid,&status,0);break;}sleep_seconds(1);}
  close(pipefd[0]);if(stop_flag)break;
  if(WIFEXITED(status)) fprintf(stderr,"vtouchmerge worker exited; code=%d; restart in %ds\n",WEXITSTATUS(status),backoff);
  else if(WIFSIGNALED(status)) fprintf(stderr,"vtouchmerge worker killed by signal=%d; restart in %ds\n",WTERMSIG(status),backoff);
  else fprintf(stderr,"vtouchmerge worker exited abnormally; restart in %ds\n",backoff);
  fflush(stderr);sleep_seconds(backoff);if(backoff<30)backoff*=2;
 }
 return 0;
}
