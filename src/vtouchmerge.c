/* vtouchmerge: merge a discovered Type-B touchscreen with virtual contacts. */
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/input.h>
#include <linux/uinput.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#define MAX_PHYS 64
#define MAX_VIRT 32
#define MAX_LINE 512
#define DEFAULT_SOCK "/data/local/tmp/vtouch-merge.sock"
static volatile sig_atomic_t stop_flag;
static int input_fd=-1,u_fd=-1,listen_fd=-1,ctl_fd=-1,hb_fd=-1;
static char sock_path[PATH_MAX]=DEFAULT_SOCK;
static int vslots=10,phys_slots,total_slots,axmin[2],axmax[2],selected_slot;
static int logical_width,logical_height;
struct contact { int id,x,y,down,pending_up; };
static int next_tracking_id = 1;
static struct contact phys[MAX_PHYS],virt[MAX_VIRT];
#ifndef VT_MERGE_LIBRARY
static void on_signal(int s){(void)s;stop_flag=1;}
#endif
static int parse_long(const char *s,long lo,long hi,int *out){char *e;long v;if(!s||!*s)return -1;errno=0;v=strtol(s,&e,10);if(errno||*e||v<lo||v>hi)return -1;*out=(int)v;return 0;}
static int logical_to_raw(int logical,int axis,int *raw){int size=axis?logical_height:logical_width;long span=axmax[axis]-axmin[axis];long value;if(size<2||logical<0||logical>=size)return -1;value=(long)axmin[axis]+((long)logical*span+(size-1)/2)/(size-1);if(value<axmin[axis])value=axmin[axis];if(value>axmax[axis])value=axmax[axis];*raw=(int)value;return 0;}
static int bit(const unsigned long *b,int n){return (int)((b[n/(8*sizeof(unsigned long))]>>(n%(8*sizeof(unsigned long))))&1UL);}
static int validate_device(const char *p,int *slots,int *xmin,int *xmax,int *ymin,int *ymax){
 unsigned long ev[(EV_MAX+8)/(8*sizeof(unsigned long))],abs[(ABS_MAX+8)/(8*sizeof(unsigned long))],prop[(INPUT_PROP_MAX+8)/(8*sizeof(unsigned long))];struct input_absinfo a;int f;
 memset(ev,0,sizeof ev);memset(abs,0,sizeof abs);memset(prop,0,sizeof prop);f=open(p,O_RDONLY|O_NONBLOCK|O_CLOEXEC);if(f<0){fprintf(stderr,"vtouchmerge: open %s failed: %s\\n",p,strerror(errno));return -1;}
 if(ioctl(f,EVIOCGBIT(0,sizeof ev),ev)<0||ioctl(f,EVIOCGBIT(EV_ABS,sizeof abs),abs)<0||ioctl(f,EVIOCGPROP(sizeof prop),prop)<0){fprintf(stderr,"vtouchmerge: capability ioctl failed for %s: %s\\n",p,strerror(errno));close(f);return -1;}
 if(!bit(abs,ABS_MT_SLOT)||!bit(abs,ABS_MT_TRACKING_ID)||!bit(abs,ABS_MT_POSITION_X)||!bit(abs,ABS_MT_POSITION_Y)){fprintf(stderr,"vtouchmerge: not Type-B touchscreen: %s ev=%d abs(slot=%d tid=%d x=%d y=%d)\\n",p,bit(ev,EV_ABS),bit(abs,ABS_MT_SLOT),bit(abs,ABS_MT_TRACKING_ID),bit(abs,ABS_MT_POSITION_X),bit(abs,ABS_MT_POSITION_Y));close(f);return -1;}
 if(ioctl(f,EVIOCGABS(ABS_MT_SLOT),&a)<0||a.minimum<0||a.maximum>=MAX_PHYS){close(f);return -1;}*slots=a.maximum-a.minimum+1;
 if(ioctl(f,EVIOCGABS(ABS_MT_POSITION_X),&a)<0||a.maximum<=a.minimum){close(f);return -1;}*xmin=a.minimum;*xmax=a.maximum;
 if(ioctl(f,EVIOCGABS(ABS_MT_POSITION_Y),&a)<0||a.maximum<=a.minimum){close(f);return -1;}*ymin=a.minimum;*ymax=a.maximum;close(f);return 0;
}
static int discover(char *out,size_t n){int k;for(k=0;k<64;k++){snprintf(out,n,"/dev/input/event%d",k);if(validate_device(out,&phys_slots,&axmin[0],&axmax[0],&axmin[1],&axmax[1])==0)return 0;}return -1;}
static int emit(int t,int c,int v){struct input_event e;ssize_t n;memset(&e,0,sizeof e);e.type=t;e.code=c;e.value=v;do n=write(u_fd,&e,sizeof e);while(n<0&&errno==EINTR);return n==(ssize_t)sizeof e?0:-1;}
static int syn(void){return emit(EV_SYN,SYN_REPORT,0);}
static int setup_uinput(void){struct uinput_setup s;struct uinput_abs_setup a;int i;
 u_fd=open("/dev/uinput",O_WRONLY|O_NONBLOCK|O_CLOEXEC);if(u_fd<0)return -1;
 if(ioctl(u_fd,UI_SET_EVBIT,EV_SYN)<0||ioctl(u_fd,UI_SET_EVBIT,EV_KEY)<0||ioctl(u_fd,UI_SET_EVBIT,EV_ABS)<0||ioctl(u_fd,UI_SET_KEYBIT,BTN_TOUCH)<0||ioctl(u_fd,UI_SET_KEYBIT,BTN_TOOL_FINGER)<0||ioctl(u_fd,UI_SET_PROPBIT,INPUT_PROP_DIRECT)<0)goto fail;
 for(i=0;i<total_slots;i++)if(ioctl(u_fd,UI_SET_ABSBIT,ABS_MT_SLOT)<0)goto fail;
 if(ioctl(u_fd,UI_SET_ABSBIT,ABS_MT_TRACKING_ID)<0||ioctl(u_fd,UI_SET_ABSBIT,ABS_MT_POSITION_X)<0||ioctl(u_fd,UI_SET_ABSBIT,ABS_MT_POSITION_Y)<0||ioctl(u_fd,UI_SET_ABSBIT,ABS_MT_TOOL_TYPE)<0)goto fail;
 memset(&s,0,sizeof s);s.id.bustype=BUS_VIRTUAL;strncpy((char *)s.name,"vtouch-merged",UINPUT_MAX_NAME_SIZE-1);if(ioctl(u_fd,UI_DEV_SETUP,&s)<0)goto fail;
 memset(&a,0,sizeof a);a.code=ABS_MT_SLOT;a.absinfo.maximum=total_slots-1;if(ioctl(u_fd,UI_ABS_SETUP,&a)<0)goto fail;
 a.code=ABS_MT_TRACKING_ID;a.absinfo.maximum=65535;if(ioctl(u_fd,UI_ABS_SETUP,&a)<0)goto fail;
 a.code=ABS_MT_POSITION_X;a.absinfo.minimum=axmin[0];a.absinfo.maximum=axmax[0];if(ioctl(u_fd,UI_ABS_SETUP,&a)<0)goto fail;
 a.code=ABS_MT_POSITION_Y;a.absinfo.minimum=axmin[1];a.absinfo.maximum=axmax[1];if(ioctl(u_fd,UI_ABS_SETUP,&a)<0)goto fail;
 a.code=ABS_MT_TOOL_TYPE;a.absinfo.maximum=MT_TOOL_PALM;if(ioctl(u_fd,UI_ABS_SETUP,&a)<0)goto fail;if(ioctl(u_fd,UI_DEV_CREATE)<0)goto fail;return 0;
fail: ioctl(u_fd,UI_DEV_DESTROY);close(u_fd);u_fd=-1;return -1;
}
static void cleanup(void){int i;if(ctl_fd>=0){close(ctl_fd);ctl_fd=-1;}if(listen_fd>=0){close(listen_fd);listen_fd=-1;unlink(sock_path);}if(input_fd>=0){
#ifndef VT_MERGE_TEST
 ioctl(input_fd,EVIOCGRAB,0);
#endif
 close(input_fd);input_fd=-1;}if(u_fd>=0){ioctl(u_fd,UI_DEV_DESTROY);close(u_fd);u_fd=-1;}if(hb_fd>=0){close(hb_fd);hb_fd=-1;}for(i=0;i<MAX_PHYS;i++)memset(&phys[i],0,sizeof phys[i]);for(i=0;i<MAX_VIRT;i++)memset(&virt[i],0,sizeof virt[i]);}
static int make_socket(void){struct sockaddr_un a;mode_t old;listen_fd=socket(AF_UNIX,SOCK_STREAM|SOCK_CLOEXEC,0);if(listen_fd<0)return -1;memset(&a,0,sizeof a);a.sun_family=AF_UNIX;if(strlen(sock_path)>=sizeof a.sun_path)goto fail;unlink(sock_path);strncpy(a.sun_path,sock_path,sizeof a.sun_path-1);if(bind(listen_fd,(struct sockaddr *)&a,sizeof a)<0)goto fail;old=umask(007);if(chmod(sock_path,0660)<0){umask(old);goto fail;}umask(old);if(listen(listen_fd,1)<0)goto fail;return 0;fail:close(listen_fd);listen_fd=-1;unlink(sock_path);return -1;}
static int any_down(void){int i;for(i=0;i<phys_slots;i++)if(phys[i].down)return 1;for(i=0;i<vslots;i++)if(virt[i].down)return 1;return 0;}
static int emit_frame(void){int i;if(u_fd<0)return -1;for(i=0;i<phys_slots;i++)if(phys[i].pending_up){if(emit(EV_ABS,ABS_MT_SLOT,i)||emit(EV_ABS,ABS_MT_TRACKING_ID,-1))return -1;}for(i=0;i<phys_slots;i++)if(phys[i].down){if(emit(EV_ABS,ABS_MT_SLOT,i)||emit(EV_ABS,ABS_MT_TRACKING_ID,phys[i].id)||emit(EV_ABS,ABS_MT_POSITION_X,phys[i].x)||emit(EV_ABS,ABS_MT_POSITION_Y,phys[i].y)||emit(EV_ABS,ABS_MT_TOOL_TYPE,MT_TOOL_FINGER))return -1;}for(i=0;i<vslots;i++){if(virt[i].pending_up){if(emit(EV_ABS,ABS_MT_SLOT,phys_slots+i)||emit(EV_ABS,ABS_MT_TRACKING_ID,-1))return -1;}else if(virt[i].down){if(emit(EV_ABS,ABS_MT_SLOT,phys_slots+i)||emit(EV_ABS,ABS_MT_TRACKING_ID,virt[i].id)||emit(EV_ABS,ABS_MT_POSITION_X,virt[i].x)||emit(EV_ABS,ABS_MT_POSITION_Y,virt[i].y)||emit(EV_ABS,ABS_MT_TOOL_TYPE,MT_TOOL_FINGER))return -1;}}if(emit(EV_KEY,BTN_TOUCH,any_down())||emit(EV_KEY,BTN_TOOL_FINGER,any_down())||syn())return -1;for(i=0;i<phys_slots;i++)phys[i].pending_up=0;for(i=0;i<vslots;i++)virt[i].pending_up=0;return 0;}
static void owner_reset(void){int i;for(i=0;i<vslots;i++)if(virt[i].down){virt[i].down=0;virt[i].pending_up=1;}if(emit_frame()<0)stop_flag=1;}
static int set_virtual(struct contact *state,int slot,const char *name,int x,int y){if(!strcmp(name,"down")){if(state[slot].down||state[slot].pending_up)return -1;state[slot].id=next_tracking_id++;if(next_tracking_id>65535)next_tracking_id=1;state[slot].down=1;}else if(!strcmp(name,"move")){if(!state[slot].down)return -1;}else if(!strcmp(name,"up")){if(!state[slot].down)return -1;state[slot].down=0;state[slot].pending_up=1;}else return -1;state[slot].x=x;state[slot].y=y;return 0;}
static int command(char *line,int *frame_open,int *seen){char *t,*st;int slot,x,y;static struct contact staged[MAX_VIRT];static int staged_id;line[strcspn(line,"\r\n")]=0;t=strtok_r(line," \t",&st);if(!t)return -1;
 if(!strcmp(t,"ping")){dprintf(ctl_fd,"pong\n");return 0;}if(!strcmp(t,"res")){dprintf(ctl_fd,"res %d %d raw %d %d %d %d\n",logical_width,logical_height,axmin[0],axmax[0],axmin[1],axmax[1]);return 0;}if(!strcmp(t,"reset")){owner_reset();dprintf(ctl_fd,"ok\n");return 0;}
 if(!strcmp(t,"up")){char *ss=strtok_r(NULL," \t",&st);if(*frame_open||!ss||strtok_r(NULL," \t",&st)||parse_long(ss,0,vslots-1,&slot)||set_virtual(virt,slot,t,virt[slot].x,virt[slot].y)||emit_frame()<0){dprintf(ctl_fd,"err point\n");return -1;}dprintf(ctl_fd,"ok\n");return 0;}
 if(!strcmp(t,"down")||!strcmp(t,"move")){char *ss=strtok_r(NULL," \t",&st),*sx=strtok_r(NULL," \t",&st),*sy=strtok_r(NULL," \t",&st);int lx,ly;if(*frame_open||!ss||!sx||!sy||strtok_r(NULL," \t",&st)||parse_long(ss,0,vslots-1,&slot)||parse_long(sx,0,logical_width-1,&lx)||parse_long(sy,0,logical_height-1,&ly)||logical_to_raw(lx,0,&x)||logical_to_raw(ly,1,&y)||set_virtual(virt,slot,t,x,y)||emit_frame()<0){dprintf(ctl_fd,"err point\n");return -1;}dprintf(ctl_fd,"ok\n");return 0;}
 if(!strcmp(t,"begin_frame")){if(*frame_open||strtok_r(NULL," \t",&st)){dprintf(ctl_fd,"err frame\n");return -1;}memcpy(staged,virt,sizeof staged);staged_id=next_tracking_id;*frame_open=1;memset(seen,0,(size_t)vslots);dprintf(ctl_fd,"ok\n");return 0;}
 if(!strcmp(t,"point")){char *ss=strtok_r(NULL," \t",&st),*state=strtok_r(NULL," \t",&st),*sx=strtok_r(NULL," \t",&st),*sy=strtok_r(NULL," \t",&st);int lx,ly;if(!*frame_open||!ss||!state||!sx||!sy||strtok_r(NULL," \t",&st)||parse_long(ss,0,vslots-1,&slot)||parse_long(sx,0,logical_width-1,&lx)||parse_long(sy,0,logical_height-1,&ly)||logical_to_raw(lx,0,&x)||logical_to_raw(ly,1,&y)||seen[slot]||set_virtual(staged,slot,state,x,y)){dprintf(ctl_fd,"err point\n");return -1;}seen[slot]=1;dprintf(ctl_fd,"ok\n");return 0;}
 if(!strcmp(t,"end_frame")){if(!*frame_open||strtok_r(NULL," \t",&st)){dprintf(ctl_fd,"err frame\n");return -1;}memcpy(virt,staged,sizeof virt);next_tracking_id=staged_id;if(emit_frame()<0){memcpy(virt,staged,sizeof virt);*frame_open=0;dprintf(ctl_fd,"err frame\n");return -1;}*frame_open=0;dprintf(ctl_fd,"ok\n");return 0;}dprintf(ctl_fd,"err unknown\n");return -1;}
static void physical_events(void){struct input_event e;ssize_t n;while((n=read(input_fd,&e,sizeof e))==(ssize_t)sizeof e){if(e.type==EV_ABS&&e.code==ABS_MT_SLOT){selected_slot=e.value;if(selected_slot<0||selected_slot>=phys_slots)selected_slot=0;}else if(e.type==EV_ABS&&selected_slot<phys_slots){if(e.code==ABS_MT_TRACKING_ID){if(e.value<0){phys[selected_slot].down=0;phys[selected_slot].pending_up=1;}else{phys[selected_slot].id=e.value;phys[selected_slot].down=1;}}else if(e.code==ABS_MT_POSITION_X)phys[selected_slot].x=e.value;else if(e.code==ABS_MT_POSITION_Y)phys[selected_slot].y=e.value;}if(e.type==EV_SYN&&e.code==SYN_REPORT&&emit_frame()<0)stop_flag=1;}if(n<0&&(errno==ENODEV||errno==EIO))stop_flag=1;}
static void apply_args(int argc,char **argv){int i,n;for(i=1;i<argc;i++){if(!strcmp(argv[i],"-s")&&i+1<argc){strncpy(sock_path,argv[++i],sizeof sock_path-1);sock_path[sizeof sock_path-1]=0;}else if(!strcmp(argv[i],"-v")&&i+1<argc&&parse_long(argv[++i],1,MAX_VIRT,&n)==0)vslots=n;else if(!strcmp(argv[i],"-w")&&i+1<argc&&parse_long(argv[++i],2,100000,&logical_width)==0){}else if(!strcmp(argv[i],"-h")&&i+1<argc&&parse_long(argv[++i],2,100000,&logical_height)==0){}else if(strcmp(argv[i],"-s")&&strcmp(argv[i],"-v")&&strcmp(argv[i],"-w")&&strcmp(argv[i],"-h")){fprintf(stderr,"usage: %s [-s socket] [-v virtual-slots] -w width -h height\n",argv[0]);}}}
int vtouchmerge_worker(int heartbeat_fd,int argc,char **argv){char dev[PATH_MAX],line[MAX_LINE],b[256];size_t used=0;struct pollfd p[3];int frame=0,seen[MAX_VIRT];hb_fd=heartbeat_fd;apply_args(argc,argv);if(logical_width<2||logical_height<2){fprintf(stderr,"vtouchmerge: logical display size required (-w width -h height)\n");return 2;}memset(phys,0,sizeof phys);if(discover(dev,sizeof dev)<0)return 2;total_slots=phys_slots+vslots;if(setup_uinput()<0)return 3;input_fd=open(dev,O_RDONLY|O_NONBLOCK|O_CLOEXEC);if(input_fd<0){cleanup();return 4;}
#ifndef VT_MERGE_TEST
 if(ioctl(input_fd,EVIOCGRAB,1)<0){cleanup();return 5;}
#endif
 if(make_socket()<0){cleanup();return 6;}if(hb_fd>=0)write(hb_fd,"R",1);
 while(!stop_flag){p[0]=(struct pollfd){input_fd,POLLIN|POLLHUP|POLLERR,0};p[1]=(struct pollfd){listen_fd,POLLIN,0};p[2]=(struct pollfd){ctl_fd,-1,0};if(ctl_fd>=0)p[2].events=POLLIN|POLLHUP;int r=poll(p,ctl_fd>=0?3:2,1000);if(hb_fd>=0)write(hb_fd,"H",1);if(r<0){if(errno==EINTR)continue;break;}if(p[0].revents&POLLIN)physical_events();if(p[0].revents&(POLLHUP|POLLERR))break;if(ctl_fd<0&&(p[1].revents&POLLIN)){ctl_fd=accept4(listen_fd,NULL,NULL,SOCK_CLOEXEC|SOCK_NONBLOCK);used=0;frame=0;}if(ctl_fd>=0&&(p[2].revents&(POLLHUP|POLLERR))){owner_reset();close(ctl_fd);ctl_fd=-1;}if(ctl_fd>=0&&(p[2].revents&POLLIN)){ssize_t n=read(ctl_fd,b,sizeof b);if(n<=0){owner_reset();close(ctl_fd);ctl_fd=-1;}else{size_t i;for(i=0;i<(size_t)n;i++){if(b[i]=='\n'){line[used]=0;command(line,&frame,seen);used=0;}else if(used<sizeof line-1&&b[i]>=32&&b[i]<=126)line[used++]=b[i];else if(used>=sizeof line-1){used=0;dprintf(ctl_fd,"err line too long\n");}}}}}cleanup();return 0;}
#ifndef VT_MERGE_LIBRARY
int main(int argc,char **argv){signal(SIGTERM,on_signal);signal(SIGINT,on_signal);signal(SIGPIPE,SIG_IGN);return vtouchmerge_worker(-1,argc,argv);}
#endif
