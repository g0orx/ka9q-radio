//
// Web interface for ka9q-radio
//
// Uses Onion Web Framwork (https://github.com/davidmoreno/onion)
//
// John Melton G0ORX (N6LYT)
//
// Beware this is a very early test version
//
// Copyright 2023, John Melton, G0ORX
//

#define _GNU_SOURCE 1

#include <onion/log.h>
#include <onion/onion.h>
#include <onion/dict.h>
#include <onion/sessions.h>
#include <onion/websocket.h>
#include <string.h>
#include <errno.h>
#include <getopt.h>
#include <pthread.h>
#include <unistd.h>
#include <ctype.h>
#include <sysexits.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

#include "misc.h"
#include "multicast.h"
#include "status.h"
#include "radio.h"
#include "config.h"

// no handlers in /usr/local/include??
onion_handler *onion_handler_export_local_new(const char *localpath);

int Ctl_fd,Status_fd;
pthread_mutex_t ctl_mutex;
pthread_t ctrl_task;
pthread_t audio_task;

struct session {
  bool spectrum_active;
  bool audio_active;
  onion_websocket *ws;
  pthread_mutex_t ws_mutex;
  uint32_t ssrc;
  pthread_t poll_task;
  uint32_t poll_tag;
  pthread_t spectrum_task;
  pthread_mutex_t spectrum_mutex;
  uint32_t spectrum_tag;
  uint32_t center_frequency;
  uint32_t frequency;
  uint32_t bin_width;
  float tc;
  int bins;
  onion_response * res;
  struct session *next;
  struct session *previous;
};
  
#define START_SESSION_ID 1000

int init_connections(char *multicast_group);
extern int init_control(struct session *sp);
extern void control_set_frequency(struct session *sp,char *str);
extern void control_set_mode(struct session *sp,char *str);
int init_demod(struct channel *channel);
void control_get_powers(struct session *sp,float frequency,int bins,float bin_bw,float tc);
int extract_powers(float *power,int npower,uint64_t *time,double *freq,double *bin_bw,int32_t const ssrc,uint8_t const * const buffer,int length);
void control_poll(struct session *sp);
void *spectrum_thread(void *arg);
void *ctrl_thread(void *arg);
void *poll_thread(void *arg);
int decode_radio_status(struct channel *channel,uint8_t const *buffer,int length);

struct frontend Frontend;
static struct sockaddr_storage Metadata_source_address;      // Source of metadata
static struct sockaddr_storage Metadata_dest_address;      // Dest of metadata (typically multicast)

static int const DEFAULT_IP_TOS = 48;
static int const DEFAULT_MCAST_TTL = 1;

uint64_t Metadata_packets;
uint32_t Ssrc = 1000;
struct channel Channel;
uint64_t Block_drops;
int Mcast_ttl = DEFAULT_MCAST_TTL;
int IP_tos = DEFAULT_IP_TOS;
const char *App_path;
int64_t Timeout = BILLION;
uint16_t rtp_seq=0;

#define MAX_BINS 1620

onion_connection_status websocket_cb(void *data, onion_websocket * ws,
                                               ssize_t data_ready_len);

onion_connection_status audio_source(void *data, onion_request * req,
                                          onion_response * res);
onion_connection_status stream_audio(void *data, onion_request * req,
                                          onion_response * res);
static void *audio_thread(void *arg);
onion_connection_status home(void *data, onion_request * req,
                                          onion_response * res);
onion_connection_status status(void *data, onion_request * req,
                                          onion_response * res);

pthread_mutex_t session_mutex;
static int nsessions=0;
static struct session *sessions=NULL;

void add_session(struct session *sp) {
  pthread_mutex_lock(&session_mutex);
  if(sessions==NULL) {
    sessions=sp;
  } else {
    sessions->previous=sp;
    sp->next=sessions;
    sessions=sp;
  }
  nsessions++;
  pthread_mutex_unlock(&session_mutex);
//fprintf(stderr,"%s: ssrc=%d first=%p ws=%p nsessions=%d\n",__FUNCTION__,sp->ssrc,sessions,sp->ws,nsessions);
}

void delete_session(struct session *sp) {
  pthread_mutex_lock(&session_mutex);
//fprintf(stderr,"%s: sp=%p src=%d ws=%p\n",__FUNCTION__,sp,sp->ssrc,sp->ws);
  if(sp->next!=NULL) {
    sp->next->previous=sp->previous;
  }
  if(sp->previous!=NULL) {
    sp->previous->next=sp->next;
  }
  if(sessions==sp) {
    sessions=sp->next;
  }
  nsessions--;
//fprintf(stderr,"%s: sp=%p ssrc=%d first=%p ws=%p nsessions=%d\n",__FUNCTION__,sp,sp->ssrc,sessions,sp->ws,nsessions);
  free(sp);
  pthread_mutex_unlock(&session_mutex);
}

struct session *find_session_from_websocket(onion_websocket *ws) {
  pthread_mutex_lock(&session_mutex);
//fprintf(stderr,"%s: first=%p ws=%p\n",__FUNCTION__,sessions,ws);
  struct session *sp=sessions;
  while(sp!=NULL) {
    if(sp->ws==ws) {
      break;
    }
    sp=sp->next;
  }
//fprintf(stderr,"%s: ws=%p sp=%p\n",__FUNCTION__,ws,sp);
  pthread_mutex_unlock(&session_mutex);
  return sp;
}

struct session *find_session_from_ssrc(int ssrc) {
  pthread_mutex_lock(&session_mutex);
//fprintf(stderr,"%s: first=%p ssrc=%d\n",__FUNCTION__,sessions,ssrc);
  struct session *sp=sessions;
  while(sp!=NULL) {
    if(sp->ssrc==ssrc) {
      break;
    }
    sp=sp->next;
  }
//fprintf(stderr,"%s: ssrc=%d sp=%p\n",__FUNCTION__,ssrc,sp);
  pthread_mutex_unlock(&session_mutex);
  return sp;
}

void websocket_closed(struct session *sp) {
//fprintf(stderr,"%s: audio_active=%d spectrum_active=%d\n",__FUNCTION__,sp->audio_active,sp->spectrum_active);
    pthread_mutex_lock(&sp->ws_mutex);
    control_set_frequency(sp,"0");
    sp->audio_active=false;
    if(sp->spectrum_active) {
      sp->spectrum_active=false;
      pthread_join(sp->spectrum_task,NULL);
    }
    pthread_mutex_unlock(&sp->ws_mutex);
}

onion_connection_status websocket_cb(void *data, onion_websocket * ws,
                                               ssize_t data_ready_len) {
  char tmp[MAX_BINS];
  if (data_ready_len > sizeof(tmp))
    data_ready_len = sizeof(tmp) - 1;

  //fprintf(stderr,"websocket_cb: ws=%p len=%ld\n",ws,data_ready_len);

  struct session *sp=find_session_from_websocket(ws);
  if(sp==NULL) {
    ONION_ERROR("Error did not find session for: ws=%p", ws);
    return OCS_NEED_MORE_DATA;
  }

  int len = onion_websocket_read(ws, tmp, data_ready_len);
  if (len <= 0) {
    // client has gone away - need to cleanup
    ONION_ERROR("Error reading data: %d: %s (%d) ws=%p", errno, strerror(errno),
                data_ready_len,ws);
    websocket_closed(sp);
    delete_session(sp);
    return OCS_CLOSE_CONNECTION;
  }
  tmp[len] = 0;

  //ONION_INFO("Read from websocket: %d: %s", len, tmp);


  char *token=strtok(tmp,":");
  if(strlen(token)==1) {
    switch(*token) {
      case 'S':
      case 's':
        char *temp=malloc(16);
        sprintf(temp,"S:%d",sp->ssrc);
        pthread_mutex_lock(&sp->ws_mutex);
        onion_websocket_set_opcode(sp->ws,OWS_TEXT);
        int r=onion_websocket_write(sp->ws,temp,strlen(temp));
        if(r!=strlen(temp)) {
          fprintf(stderr,"%s: S: response failed: %d\n",__FUNCTION__,r);
        }
        pthread_mutex_unlock(&sp->ws_mutex);
        free(temp);
        // client is ready - start spectrum thread
        if(pthread_create(&sp->spectrum_task,NULL,spectrum_thread,sp) == -1){
          perror("pthread_create: spectrum_thread");
        }
        break;
      case 'A':
      case 'a':
        token=strtok(NULL,":");
        if(strcmp(token,"START")==0) {
          sp->audio_active=true;
        } else if(strcmp(&tmp[2],"STOP")==0) {
          sp->audio_active=false;
        }
        break;
      case 'F':
      case 'f':
        sp->frequency=atoll(&tmp[2]);
        int32_t min_f=sp->center_frequency-((sp->bin_width*sp->bins)/2);
        int32_t max_f=sp->center_frequency+((sp->bin_width*sp->bins)/2);
        if(sp->frequency<min_f || sp->frequency>max_f) {
          sp->center_frequency=sp->frequency;
          min_f=sp->center_frequency-((sp->bin_width*sp->bins)/2);
          max_f=sp->center_frequency+((sp->bin_width*sp->bins)/2);
        }
        if(min_f<0) {
          sp->center_frequency=(sp->bin_width*sp->bins)/2;
        } else if(max_f>32200000) {
          sp->center_frequency=32200000-(sp->bin_width*sp->bins)/2;
        }
        control_set_frequency(sp,&tmp[2]);
        break;
      case 'M':
      case 'm':
        control_set_mode(sp,&tmp[2]);
        control_poll(sp);
        break;
      case 'Z':
      case 'z':
        token=strtok(NULL,":");
        if(strcmp(token,"+")==0) {
          pthread_mutex_lock(&sp->spectrum_mutex);
          switch(sp->bin_width)  {
            case 20000:
              sp->bin_width=16000;
              break;
            case 16000:
              sp->bin_width=8000;
              break;
            case 8000:
              sp->bin_width=4000;
              break;
            case 4000:
              sp->bin_width=2000;
              break;
            case 2000:
              sp->bin_width=1000;
              break;
            case 1000:
              sp->bin_width=800;
              break;
            case 800:
              sp->bin_width=400;
              break;
            case 400:
              sp->bin_width=200;
              break;
            case 200:
              sp->bin_width=80;
              break;
            case 80:
              sp->bin_width=40;
              break;
          }
          pthread_mutex_unlock(&sp->spectrum_mutex);
//fprintf(stderr,"%s: bins=%d bin_width=%d\n",__FUNCTION__,sp->bins,sp->bin_width);
          // check frequency is within zoomed span
          // if not the center on the frequency
          int32_t min_f=sp->center_frequency-((sp->bin_width*sp->bins)/2);
          int32_t max_f=sp->center_frequency+((sp->bin_width*sp->bins)/2);
          if(sp->frequency<min_f || sp->frequency>max_f) {
            sp->center_frequency=sp->frequency;
            min_f=sp->center_frequency-((sp->bin_width*sp->bins)/2);
            max_f=sp->center_frequency+((sp->bin_width*sp->bins)/2);
          }
          if(min_f<0) {
            sp->center_frequency=(sp->bin_width*sp->bins)/2;
          } else if(max_f>32200000) {
            sp->center_frequency=32200000-(sp->bin_width*sp->bins)/2;
          }
        } else if(strcmp(token,"-")==0) {
          pthread_mutex_lock(&sp->spectrum_mutex);
          switch(sp->bin_width)  {
            case 16000:
              sp->bin_width=20000;
              break;
            case 8000:
              sp->bin_width=16000;
              break;
            case 4000:
              sp->bin_width=8000;
              break;
            case 2000:
              sp->bin_width=4000;
              break;
            case 1000:
              sp->bin_width=2000;
              break;
            case 800:
              sp->bin_width=1000;
              break;
            case 400:
              sp->bin_width=800;
              break;
            case 200:
              sp->bin_width=400;
              break;
            case 80:
              sp->bin_width=200;
              break;
            case 40:
              sp->bin_width=80;
              break;
          }
          pthread_mutex_unlock(&sp->spectrum_mutex);
//fprintf(stderr,"%s: bins=%d bin_width=%d\n",__FUNCTION__,sp->bins,sp->bin_width);
          // check frequency is within zoomed span
          // if not the center on the frequency
          int32_t min_f=sp->center_frequency-((sp->bin_width*sp->bins)/2);
          int32_t max_f=sp->center_frequency+((sp->bin_width*sp->bins)/2);
          if(sp->frequency<min_f || sp->frequency>max_f) {
            sp->center_frequency=sp->frequency;
            min_f=sp->center_frequency-((sp->bin_width*sp->bins)/2);
            max_f=sp->center_frequency+((sp->bin_width*sp->bins)/2);
          }
          if(min_f<0) {
            sp->center_frequency=(sp->bin_width*sp->bins)/2;
          } else if(max_f>32200000) {
            sp->center_frequency=32200000-(sp->bin_width*sp->bins)/2;
          }
        } else if(strcmp(token,"c")==0) {
          sp->center_frequency=sp->frequency;
        }
        break;
    }
  }
  
  return OCS_NEED_MORE_DATA;
}

int main(int argc,char **argv) {
  char *port="8081";
  const char *dirname="./html";
  const char *mcast="web.local";

  App_path=argv[0];
  {
    int c;
    while((c = getopt(argc,argv,"d:p:m:h")) != -1){
      switch(c) {
        case 'd':
          dirname=optarg;
          break;
        case 'p':
          port=optarg;
          break;
        case 'm':
          mcast=optarg;
          break;
        case 'h':
        default:
          fprintf(stderr,"Usage: %s\n",App_path);
          fprintf(stderr,"       %s [-d directory] [-p port] [-m mcast_address]\n",App_path);
          exit(EX_USAGE);
          break;
      }
    }
  }

  for(int i=1;i<argc;i++) {
    if ((strcmp(argv[i], "--port") == 0) || (strcmp(argv[i], "-p") == 0)) {
      port = argv[++i];
    } else {
      dirname = argv[i];
    }
  }

  pthread_mutex_init(&session_mutex,NULL);
  init_connections(mcast);

  onion *o = onion_new(O_THREADED);
  onion_url *urls=onion_root_url(o);
  onion_set_port(o, port);
  onion_set_hostname(o, "0.0.0.0");
  onion_handler *pages = onion_handler_export_local_new(dirname);
  onion_handler_add(onion_url_to_handler(urls), pages);
  onion_url_add(urls, "status", status);
  onion_url_add(urls, "^$", home);

  onion_listen(o);

  onion_free(o);
  return 0;
}

onion_connection_status status(void *data, onion_request * req,
                                          onion_response * res) {
    char text[1024];
    onion_response_write0(res,
      "<!DOCTYPE html>"
      "<html>"
        "<head>"
        "  <title>G0ORX Web SDR - Status</title>"
        "  <meta charset=\"UTF-8\" />"
        "</head>"
        "<body>"
        "  <h1>G0ORX Web SDR - Status</h1>");
    sprintf(text,"<b>Sessions: %d</b>",nsessions);
    onion_response_write0(res, text);

    if(nsessions!=0) {
      onion_response_write0(res, "<table border=1>"
         "<tr>"
         "<th>ssrc</th>"
         "<th>frequency range(Hz)</th>"
         "<th>frequency(Hz)</th>"
         "<th>center frequency(Hz)</th>"
         "<th>bins</th>"
         "<th>bin width(Hz)</th>"
         "<th>Audio</th>"
         "</tr>");
     
      struct session *sp = sessions;
      while(sp!=NULL) {
        int32_t min_f=sp->center_frequency-((sp->bin_width*sp->bins)/2);
        int32_t max_f=sp->center_frequency+((sp->bin_width*sp->bins)/2);
        sprintf(text,"<tr><td>%d</td><td>%d to %d</td><td>%d</td><td>%d</td><td>%d</td><td>%d</td><td>%s</td></tr>",sp->ssrc,min_f,max_f,sp->frequency,sp->center_frequency,sp->bins,sp->bin_width,sp->audio_active?"Enabled":"Disabled");
        onion_response_write0(res, text);
        sp=sp->next;
      }
      onion_response_write0(res, "</table>");
    }

    onion_response_write0(res,
        "</body>"
        "</html>");
    return OCS_PROCESSED;
}

onion_connection_status home(void *data, onion_request * req,
                                          onion_response * res) {
  onion_websocket *ws = onion_websocket_new(req, res);
  //fprintf(stderr,"%s: ws=%p\n",__FUNCTION__,ws);
  if(ws==NULL) {
    onion_response_write0(res,
      "<!DOCTYPE html>"
      "<html>"
        "<head>"
        "  <title>G0ORX Web SDR</title>"
        "  <meta charset=\"UTF-8\" />"
        "  <meta http-equiv=\"refresh\" content=\"0; URL=radio.html\" />"
        "</head>"
        "<body>"
        "</body>"
        "</html>");
    return OCS_PROCESSED;
  }

  // setup a control
  //fprintf(stderr,"%s: init_control ws=%p\n",__FUNCTION__,ws);

  int i;
  struct session *sp=calloc(1,sizeof(*sp));
  if(nsessions==0) {
    sp->ssrc=START_SESSION_ID;
  } else {
    for(i=0;i<nsessions;i++) {
      struct session *s=find_session_from_ssrc(START_SESSION_ID+(i*2));
      if(s==NULL) {
        break;
      }
    }
    sp->ssrc=START_SESSION_ID+(i*2);
  }
//  sp->ssrc=session_id;
//  session_id=session_id+2;
  sp->ws=ws;
  sp->spectrum_active=true;
  sp->audio_active=false;
  sp->frequency=16200000;
  sp->center_frequency=16200000; // mid of 32.4MHz - assuming we are running at 64.8MHz sample rate
  sp->bins=MAX_BINS;
  sp->bin_width=20000; // width of a pixel in hz
  sp->tc=1.0;
  sp->res=NULL;
  sp->next=NULL;
  sp->previous=NULL;
  pthread_mutex_init(&sp->ws_mutex,NULL);
  pthread_mutex_init(&sp->spectrum_mutex,NULL);
  add_session(sp);
  init_control(sp);
  //fprintf(stderr,"%s: onion_websocket_set_callback: websocket_cb\n",__FUNCTION__);
  onion_websocket_set_callback(ws, websocket_cb);

  return OCS_WEBSOCKET; 
}

static void *audio_thread(void *arg) {
  struct session *sp;
  struct packet *pkt = malloc(sizeof(*pkt));

  //fprintf(stderr,"%s\n",__FUNCTION__);

  char const *mcast_address_text = "web.local";

  int input_fd;
  {
    char iface[1024];
    struct sockaddr sock;
    resolve_mcast(mcast_address_text,&sock,DEFAULT_RTP_PORT,iface,sizeof(iface));
    input_fd = listen_mcast(&sock,iface);
  }

  if(input_fd==-1) {
    pthread_exit(NULL);
  }

  while(1) {
    struct sockaddr_storage sender;
    socklen_t socksize = sizeof(sender);
    int size = recvfrom(input_fd,&pkt->content,sizeof(pkt->content),0,(struct sockaddr *)&sender,&socksize);

    if(size == -1){
      if(errno != EINTR){ // Happens routinely, e.g., when window resized
        perror("recvfrom");
        fprintf(stderr,"address=%s\n",mcast_address_text);
        usleep(1000);
      }
      continue;  // Reuse current buffer
    }
    if(size <= RTP_MIN_SIZE)
      continue; // Must be big enough for RTP header and at least some data

    // Convert RTP header to host format
    uint8_t const *dp = ntoh_rtp(&pkt->rtp,pkt->content);
    pkt->data = dp;
    pkt->len = size - (dp - pkt->content);
    if(pkt->rtp.pad){
      pkt->len -= dp[pkt->len-1];
      pkt->rtp.pad = 0;
    }
    if(pkt->len <= 0)
      continue; // Used to be an assert, but would be triggered by bogus packets


    sp=find_session_from_ssrc(pkt->rtp.ssrc);
//fprintf(stderr,"%s: sp=%p ssrc=%d\n",__FUNCTION__,sp,pkt->rtp.ssrc);
    if(sp!=NULL) {
      if(sp->audio_active) {
        //fprintf(stderr,"forward RTP: ws=%p ssrc=%d\n",sp->ws,pkt->rtp.ssrc);
        pthread_mutex_lock(&sp->ws_mutex);
        onion_websocket_set_opcode(sp->ws,OWS_BINARY);
        int r=onion_websocket_write(sp->ws,pkt->content,size);
        pthread_mutex_unlock(&sp->ws_mutex);
        if(r<=0) {
          fprintf(stderr,"%s: write failed: %d\n",__FUNCTION__,r);
        }
      }
    }  // not found
  }

  //fprintf(stderr,"EXIT %s\n",__FUNCTION__);
  return NULL;
}

int init_connections(char *multicast_group) {
  char iface[1024]; // Multicast interface

  pthread_mutex_init(&ctl_mutex,NULL);

  resolve_mcast(multicast_group,&Metadata_dest_address,DEFAULT_STAT_PORT,iface,sizeof(iface));
  Status_fd = listen_mcast(&Metadata_dest_address,iface);
  if(Status_fd == -1){
    fprintf(stderr,"Can't listen to mcast status %s\n",multicast_group);
    return(EX_IOERR);
  }

  Ctl_fd = connect_mcast(&Metadata_dest_address,iface,Mcast_ttl,IP_tos);
  if(Ctl_fd < 0){
    fprintf(stderr,"connect to mcast control failed: RX\n");
    return(EX_IOERR);
  }

  if(pthread_create(&ctrl_task,NULL,ctrl_thread,NULL) == -1){
    perror("pthread_create: ctrl_thread");
    //free(sp);
  }

  if(pthread_create(&audio_task,NULL,audio_thread,NULL) == -1){
    perror("pthread_create");
  }
}

int init_control(struct session *sp) {
  uint32_t sent_tag = 0;

//fprintf(stderr,"%s: Ssrc=%d\n",__FUNCTION__,sp->ssrc);
  // send a frequency to start with
  uint8_t cmdbuffer[PKTSIZE];
  uint8_t *bp = cmdbuffer;
  *bp++ = CMD; // Command

  encode_double(&bp,RADIO_FREQUENCY,16200000);
  encode_int(&bp,OUTPUT_SSRC,sp->ssrc); // Specific SSRC
  sent_tag = arc4random();
  encode_int(&bp,COMMAND_TAG,sent_tag); // Append a command tag
  encode_eol(&bp);
  int command_len = bp - cmdbuffer;
  pthread_mutex_lock(&ctl_mutex);
  if(send(Ctl_fd, cmdbuffer, command_len, 0) != command_len){
    fprintf(stderr,"command send error: %s\n",strerror(errno));
  }
  pthread_mutex_unlock(&ctl_mutex);

  bp = cmdbuffer;
  *bp++ = CMD; // Command

  encode_double(&bp,RADIO_FREQUENCY,16200000);
  encode_int(&bp,OUTPUT_SSRC,sp->ssrc+1); // Specific SSRC
  sent_tag = arc4random();
  encode_int(&bp,COMMAND_TAG,sent_tag); // Append a command tag
  encode_eol(&bp);
  command_len = bp - cmdbuffer;
  pthread_mutex_lock(&ctl_mutex);
  if(send(Ctl_fd, cmdbuffer, command_len, 0) != command_len){
    fprintf(stderr,"command send error: %s\n",strerror(errno));
  }
  pthread_mutex_unlock(&ctl_mutex);

  init_demod(&Channel);

  Frontend.frequency = Frontend.min_IF = Frontend.max_IF = NAN;

  return(EX_OK);
}

void control_set_frequency(struct session *sp,char *str) {
  uint8_t cmdbuffer[PKTSIZE];
  uint8_t *bp = cmdbuffer;
  double f;

  if(strlen(str) > 0){
    *bp++ = CMD; // Command
    f = fabs(parse_frequency(str,true)); // Handles funky forms like 147m435
    encode_double(&bp,RADIO_FREQUENCY,f);
    encode_int(&bp,OUTPUT_SSRC,sp->ssrc); // Specific SSRC
    encode_int(&bp,COMMAND_TAG,arc4random()); // Append a command tag
    encode_eol(&bp);
    int const command_len = bp - cmdbuffer;
    pthread_mutex_lock(&ctl_mutex);
    if(send(Ctl_fd, cmdbuffer, command_len, 0) != command_len){
      fprintf(stderr,"command send error: %s\n",strerror(errno));
    }
    pthread_mutex_unlock(&ctl_mutex);
  }
}

void control_set_mode(struct session *sp,char *str) {
  uint8_t cmdbuffer[PKTSIZE];
  uint8_t *bp = cmdbuffer;

  if(strlen(str) > 0) {
    *bp++ = CMD; // Command
    encode_string(&bp,PRESET,str,strlen(str));
    encode_int(&bp,OUTPUT_SSRC,sp->ssrc); // Specific SSRC
    encode_int(&bp,COMMAND_TAG,arc4random()); // Append a command tag
    encode_eol(&bp);
    int const command_len = bp - cmdbuffer;
    pthread_mutex_lock(&ctl_mutex);
    if(send(Ctl_fd, cmdbuffer, command_len, 0) != command_len){
      fprintf(stderr,"command send error: %s\n",strerror(errno));
    }
    pthread_mutex_unlock(&ctl_mutex);
  }
}

void control_get_powers(struct session *sp,float frequency,int bins,float bin_bw,float tc) {
  uint8_t cmdbuffer[PKTSIZE];
  uint8_t *bp = cmdbuffer;
  *bp++ = CMD; // Command
  encode_int(&bp,OUTPUT_SSRC,sp->ssrc+1);
  uint32_t tag = random();
  encode_int(&bp,COMMAND_TAG,tag);
  encode_int(&bp,DEMOD_TYPE,SPECT_DEMOD);
  encode_float(&bp,RADIO_FREQUENCY,frequency);
  encode_int(&bp,BIN_COUNT,bins);
  encode_float(&bp,NONCOHERENT_BIN_BW,bin_bw);
  encode_float(&bp,INTEGRATE_TC,tc);
  encode_eol(&bp);

  sp->spectrum_tag=tag;

//fprintf(stderr,"%s: ssrc=%d tag=%d\n",__FUNCTION__,sp->ssrc+1,sp->spectrum_tag);

  int const command_len = bp - cmdbuffer;
  pthread_mutex_lock(&ctl_mutex);
  if(send(Ctl_fd, cmdbuffer, command_len, 0) != command_len) {
    perror("command send: Spectrum");
  }
  pthread_mutex_unlock(&ctl_mutex);
}

void control_poll(struct session *sp) {
  uint8_t cmdbuffer[128];
  uint8_t *bp = cmdbuffer;
  *bp++ = 1; // Command

  sp->poll_tag = random();
  encode_int(&bp,COMMAND_TAG,sp->poll_tag);
  encode_int(&bp,OUTPUT_SSRC,sp->ssrc); // poll specific SSRC, or request ssrc list with ssrc = 0
  encode_eol(&bp);
  int const command_len = bp - cmdbuffer;
  pthread_mutex_lock(&ctl_mutex);
  if(send(Ctl_fd, cmdbuffer, command_len, 0) != command_len) {
    perror("command send: Poll");
  }
  pthread_mutex_unlock(&ctl_mutex);
}

int extract_powers(float *power,int npower,uint64_t *time,double *freq,double *bin_bw,int32_t const ssrc,uint8_t const * const buffer,int length){
#if 0  // use later
  double l_lo1 = 0,l_lo2 = 0;
#endif
  int l_ccount = 0;
  uint8_t const *cp = buffer;
  int l_count;

//fprintf(stderr,"%s: length=%d\n",__FUNCTION__,length);
  while(cp - buffer < length){
    enum status_type const type = *cp++; // increment cp to length field

    if(type == EOL)
      break; // End of list

    unsigned int optlen = *cp++;
    if(optlen & 0x80){
      // length is >= 128 bytes; fetch actual length from next N bytes, where N is low 7 bits of optlen
      int length_of_length = optlen & 0x7f;
      optlen = 0;
      while(length_of_length > 0){
        optlen <<= 8;
        optlen |= *cp++;
        length_of_length--;
      }
    }
    if(cp - buffer + optlen >= length)
      break; // Invalid length
    switch(type){
    case EOL: // Shouldn't get here
      goto done;
    case GPS_TIME:
      *time = decode_int64(cp,optlen);
      break;
    case OUTPUT_SSRC: // Don't really need this, it's already been checked
      if(decode_int32(cp,optlen) != ssrc)
        return -1; // Not what we want
      break;
    case DEMOD_TYPE:
     {
        const int i = decode_int(cp,optlen);
        if(i != SPECT_DEMOD)
          return -3; // Not what we want
      }
      break;
    case RADIO_FREQUENCY:
      *freq = decode_double(cp,optlen);
      break;
#if 0  // Use this to fine-tweak freq later
    case FIRST_LO_FREQUENCY:
      l_lo1 = decode_double(cp,optlen);
      break;
    case SECOND_LO_FREQUENCY: // ditto
      l_lo2 = decode_double(cp,optlen);
      break;
#endif
    case BIN_DATA:
      l_count = optlen/sizeof(float);
      if(l_count > npower)
        return -2; // Not enough room in caller's array
      // Note these are still in FFT order
      for(int i=0; i < l_count; i++){
        power[i] = decode_float(cp,sizeof(float));
        cp += sizeof(float);
      }
      break;
    case NONCOHERENT_BIN_BW:
      *bin_bw = decode_float(cp,optlen);
      break;
    case BIN_COUNT: // Do we check that this equals the length of the BIN_DATA tlv?
      l_ccount = decode_int(cp,optlen);
      break;
    default:
      break;
    }
    cp += optlen;
  }
 done:
  ;
//fprintf(stderr,"%s: l_ccount=%d l_count=%d\n",__FUNCTION__,l_ccount,l_count);
  //assert(l_ccount == l_count);
  return l_ccount;
}

int init_demod(struct channel *channel){
  memset(channel,0,sizeof(*channel));
  channel->tune.second_LO = NAN;
  channel->tune.freq = channel->tune.shift = NAN;
  channel->filter.min_IF = channel->filter.max_IF = channel->filter.kaiser_beta = NAN;
  channel->output.headroom = channel->linear.hangtime = channel->linear.recovery_rate = NAN;
  channel->sig.bb_power = channel->sig.snr = channel->sig.foffset = NAN;
  channel->fm.pdeviation = channel->linear.cphase = channel->linear.lock_timer = NAN;
  channel->output.gain = NAN;
  channel->tp1 = channel->tp2 = NAN;

  channel->output.data_fd = channel->output.rtcp_fd = -1;
  return 0;
}

void *spectrum_thread(void *arg) {
  struct session *sp = (struct session *)arg;
  //fprintf(stderr,"%s: %d\n",__FUNCTION__,sp->ssrc);
  while(sp->spectrum_active) {
    pthread_mutex_lock(&sp->spectrum_mutex);
    control_get_powers(sp,(float)sp->center_frequency,sp->bins,(float)sp->bin_width,sp->tc);
    pthread_mutex_unlock(&sp->spectrum_mutex);
    if(usleep(100000) !=0) {
      perror("spectrum_thread: usleep(100000)");
    }
  }
  //fprintf(stderr,"%s: %d EXIT\n",__FUNCTION__,sp->ssrc);
  return NULL;
}

void *poll_thread(void *arg) {
  struct session *sp = (struct session *)arg;
  //fprintf(stderr,"%s\n",__FUNCTION__);
  while(1) {
    pthread_mutex_lock(&ctl_mutex);
    send_poll(Ctl_fd,sp->ssrc);
    pthread_mutex_unlock(&ctl_mutex);
    if(usleep(250000) !=0) {
      perror("poll_thread: usleep(50000)");
    }
  }
  //fprintf(stderr,"%s: EXIT\n",__FUNCTION__);
  return NULL;
}

void *ctrl_thread(void *arg) {
  struct session *sp;
  socklen_t ssize = sizeof(Metadata_source_address);
  uint8_t buffer[PKTSIZE/sizeof(float)];
  uint8_t output_buffer[PKTSIZE];
  float powers[PKTSIZE / sizeof(float)];
  uint64_t time;
  double r_freq;
  double r_bin_bw;
//fprintf(stderr,"%s\n",__FUNCTION__);

  realtime();

  while(1) {
    int length = recvfrom(Status_fd,buffer,sizeof(buffer),0,(struct sockaddr *)&Metadata_source_address,&ssize);
    if(length > 2 && (enum pkt_type)buffer[0] == STATUS) {
      uint32_t ssrc=get_ssrc(buffer+1,length-1);
      uint32_t tag=get_tag(buffer+1,length-1);
//fprintf(stderr,"%s: ssrc=%d tag=%d\n",__FUNCTION__,ssrc,tag);
      if(ssrc%2==1) {
        sp=find_session_from_ssrc(ssrc-1);
      } else {
        sp=find_session_from_ssrc(ssrc);
      }
      if(sp!=NULL) {
//fprintf(stderr,"%s: ws=%p ssrc=%d tag=%d poll_tag=%d spectrum_tag=%d\n",__FUNCTION__,sp->ws,ssrc,tag,sp->poll_tag,sp->spectrum_tag);
          if(tag==sp->poll_tag) {
            decode_radio_status(&Channel,buffer+1,length-1);
            struct rtp_header rtp;
            memset(&rtp,0,sizeof(rtp));
            rtp.type = 0x7E; // radio data
            rtp.version = RTP_VERS;
            rtp.ssrc = sp->ssrc;
            rtp.marker = true; // Start with marker bit on to reset playout buffer
            rtp.seq = rtp_seq++; // ??????
            uint8_t *bp=(uint8_t *)hton_rtp((char *)output_buffer,&rtp);
            int header_size=bp-&output_buffer[0];
            int length=(PKTSIZE-header_size)/sizeof(float);
            //encode_float(&bp,BASEBAND_POWER,Channel.sig.bb_power);
            encode_float(&bp,LOW_EDGE,Channel.filter.min_IF);
            encode_float(&bp,HIGH_EDGE,Channel.filter.max_IF);
            pthread_mutex_lock(&sp->ws_mutex);
            onion_websocket_set_opcode(sp->ws,OWS_BINARY);
            int size=(uint8_t*)bp-&output_buffer[0];
            int r=onion_websocket_write(sp->ws,(char *)output_buffer,size);
            if(r<=0) {
              fprintf(stderr,"%s: write failed: %d\n",__FUNCTION__,r);
            }
            pthread_mutex_unlock(&sp->ws_mutex);
          } else if(tag==sp->spectrum_tag) { // spectrum
//fprintf(stderr,"forward spectrum: tag=%d ws=%p\n",tag,sp->ws);
            struct rtp_header rtp;
            memset(&rtp,0,sizeof(rtp));
            rtp.type = 0x7F; // spectrum data
            rtp.version = RTP_VERS;
            rtp.ssrc = sp->ssrc;
            rtp.marker = true; // Start with marker bit on to reset playout buffer
            rtp.seq = rtp_seq++;
            uint8_t *bp=(uint8_t *)hton_rtp((char *)output_buffer,&rtp);

            uint32_t *ip=(uint32_t*)bp;
            *ip++=htonl(sp->center_frequency);
            *ip++=htonl(sp->frequency);
            *ip++=htonl(sp->bin_width);

            int header_size=(uint8_t*)ip-&output_buffer[0];
            int length=(PKTSIZE-header_size)/sizeof(float);
            int npower = extract_powers(powers,length, &time,&r_freq,&r_bin_bw,sp->ssrc+1,buffer+1,length-1);
            if(npower < 0){
              continue; // Invalid for some reason
            }

            int mid=npower/2;
            float *fp=(float*)ip;

            // below center
            for(int i=mid; i < npower; i++) {
              *fp++=(powers[i] == 0) ? 120.0 : 10*log10(powers[i]);
            }
            // above center
            for(int i=0; i < mid; i++) {
              *fp++=(powers[i] == 0) ? 120.0 : 10*log10(powers[i]);
            }
  
            // send the spectrum data to the client
            pthread_mutex_lock(&sp->ws_mutex);
            onion_websocket_set_opcode(sp->ws,OWS_BINARY);
            int size=(uint8_t*)fp-&output_buffer[0];
            int r=onion_websocket_write(sp->ws,(char *)output_buffer,size);
            if(r<=0) {
              fprintf(stderr,"%s: write failed: %d(size=%d)\n",__FUNCTION__,r,size);
            }
            pthread_mutex_unlock(&sp->ws_mutex);
          } // else unknown tag
      } // sp!=NULL
    } else if(length > 2 && (enum pkt_type)buffer[0] == STATUS) {
fprintf(stderr,"%s: type=0x%02X\n",__FUNCTION__,buffer[0]);
    }
  }
//fprintf(stderr,"%s: EXIT\n",__FUNCTION__);
}

// Decode incoming status message from the radio program, convert and fill in fields in local channel structure
// Leave all other fields unchanged, as they may have local uses (e.g., file descriptors)
// Note that we use some fields in channel differently than in the radio (e.g., dB vs ratios)
int decode_radio_status(struct channel *channel,uint8_t const *buffer,int length){
  uint8_t const *cp = buffer;
  while(cp - buffer < length){
    enum status_type type = *cp++; // increment cp to length field

    if(type == EOL)
      break; // end of list
    
    unsigned int optlen = *cp++;
    if(cp - buffer + optlen >= length)
      break; // invalid length; we can't continue to scan
    switch(type){
    case EOL:
      break;
    case CMD_CNT:
      channel->commands = decode_int32(cp,optlen);
      break;
    case DESCRIPTION:
      FREE(Frontend.description);
      Frontend.description = decode_string(cp,optlen);
      break;
    case GPS_TIME:
      Frontend.timestamp = decode_int64(cp,optlen);
      break;
    case INPUT_SAMPRATE:
      Frontend.samprate = decode_int(cp,optlen);
      break;
    case INPUT_SAMPLES:
      Frontend.samples = decode_int64(cp,optlen);
      break;
    case OUTPUT_DATA_SOURCE_SOCKET:
      decode_socket(&channel->output.data_source_address,cp,optlen);
      break;
    case OUTPUT_DATA_DEST_SOCKET:
      decode_socket(&channel->output.data_dest_address,cp,optlen);
      break;
    case OUTPUT_SSRC:
      channel->output.rtp.ssrc = decode_int32(cp,optlen);
      break;
    case OUTPUT_TTL:
      Mcast_ttl = decode_int8(cp,optlen);
      break;
    case OUTPUT_SAMPRATE:
      channel->output.samprate = decode_int(cp,optlen);
      break;
    case OUTPUT_DATA_PACKETS:
      channel->output.rtp.packets = decode_int64(cp,optlen);
      break;
    case OUTPUT_METADATA_PACKETS:
      Metadata_packets = decode_int64(cp,optlen);      
      break;
    case FILTER_BLOCKSIZE:
      Frontend.L = decode_int(cp,optlen);
      break;
    case FILTER_FIR_LENGTH:
      Frontend.M = decode_int(cp,optlen);
      break;
    case LOW_EDGE:
      channel->filter.min_IF = decode_float(cp,optlen);
      break;
    case HIGH_EDGE:
      channel->filter.max_IF = decode_float(cp,optlen);
      break;
    case FE_LOW_EDGE:
      Frontend.min_IF = decode_float(cp,optlen);
      break;
    case FE_HIGH_EDGE:
      Frontend.max_IF = decode_float(cp,optlen);
      break;
    case FE_ISREAL:
      Frontend.isreal = decode_int8(cp,optlen) ? true: false;
      break;
    case AD_BITS_PER_SAMPLE:
      Frontend.bitspersample = decode_int(cp,optlen);
      break;
    case IF_GAIN:
      Frontend.if_gain = decode_int8(cp,optlen);
      break;
    case LNA_GAIN:
      Frontend.lna_gain = decode_int8(cp,optlen);
      break;
    case MIXER_GAIN:
      Frontend.mixer_gain = decode_int8(cp,optlen);
      break;
    case KAISER_BETA:
      channel->filter.kaiser_beta = decode_float(cp,optlen);
      break;
    case FILTER_DROPS:
      Block_drops = decode_int(cp,optlen);
      break;
    case IF_POWER:
      Frontend.if_power = dB2power(decode_float(cp,optlen));
      break;
    case BASEBAND_POWER:
      channel->sig.bb_power = dB2power(decode_float(cp,optlen)); // dB -> power
      break;
    case NOISE_DENSITY:
      channel->sig.n0 = dB2power(decode_float(cp,optlen));
      break;
    case DEMOD_SNR:
      channel->sig.snr = dB2power(decode_float(cp,optlen));
      break;
    case FREQ_OFFSET:
      channel->sig.foffset = decode_float(cp,optlen);
      break;
    case PEAK_DEVIATION:
      channel->fm.pdeviation = decode_float(cp,optlen);
      break;
    case PLL_LOCK:
      channel->linear.pll_lock = decode_int8(cp,optlen);
      break;
    case PLL_BW:
      channel->linear.loop_bw = decode_float(cp,optlen);
      break;
    case PLL_SQUARE:
      channel->linear.square = decode_int8(cp,optlen);
      break;
    case PLL_PHASE:
      channel->linear.cphase = decode_float(cp,optlen);
      break;
    case ENVELOPE:
      channel->linear.env = decode_int8(cp,optlen);
      break;
    case OUTPUT_LEVEL:
      channel->output.energy = dB2power(decode_float(cp,optlen));
      break;
    case OUTPUT_SAMPLES:
      channel->output.samples = decode_int64(cp,optlen);
      break;
    case COMMAND_TAG:
      channel->command_tag = decode_int32(cp,optlen);
      break;
    case RADIO_FREQUENCY:
      channel->tune.freq = decode_double(cp,optlen);
      break;
    case SECOND_LO_FREQUENCY:
      channel->tune.second_LO = decode_double(cp,optlen);
      break;
    case SHIFT_FREQUENCY:
      channel->tune.shift = decode_double(cp,optlen);
      break;
    case FIRST_LO_FREQUENCY:
      Frontend.frequency = decode_double(cp,optlen);
      break;
    case DOPPLER_FREQUENCY:
      channel->tune.doppler = decode_double(cp,optlen);
      break;
    case DOPPLER_FREQUENCY_RATE:
      channel->tune.doppler_rate = decode_double(cp,optlen);
      break;
    case DEMOD_TYPE:
      channel->demod_type = decode_int(cp,optlen);
      break;
    case OUTPUT_CHANNELS:
      channel->output.channels = decode_int(cp,optlen);
      break;
    case INDEPENDENT_SIDEBAND:
      channel->filter.isb = decode_int8(cp,optlen);
      break;
    case THRESH_EXTEND:
      channel->fm.threshold = decode_int8(cp,optlen);
      break;
    case PLL_ENABLE:
      channel->linear.pll = decode_int8(cp,optlen);
      break;
    case GAIN:              // dB to voltage
      channel->output.gain = dB2voltage(decode_float(cp,optlen));
      break;
    case AGC_ENABLE:
      channel->linear.agc = decode_int8(cp,optlen);
      break;
    case HEADROOM:          // db to voltage
      channel->output.headroom = dB2voltage(decode_float(cp,optlen));
      break;
    case AGC_HANGTIME:      // s to samples
      channel->linear.hangtime = decode_float(cp,optlen);
      break;
    case AGC_RECOVERY_RATE: // dB/s to dB/sample to voltage/sample
      channel->linear.recovery_rate = dB2voltage(decode_float(cp,optlen));
      break;
    case AGC_THRESHOLD:   // dB to voltage
      channel->linear.threshold = dB2voltage(decode_float(cp,optlen));
      break;
    case TP1: // Test point
      channel->tp1 = decode_float(cp,optlen);
      break;
    case TP2:
      channel->tp2 = decode_float(cp,optlen);
      break;
    case SQUELCH_OPEN:
      channel->squelch_open = dB2power(decode_float(cp,optlen));
      break;
    case SQUELCH_CLOSE:
      channel->squelch_close = dB2power(decode_float(cp,optlen));
      break;
    case DEEMPH_GAIN:
      channel->deemph.gain = decode_float(cp,optlen);
      break;
    case DEEMPH_TC:
      channel->deemph.rate = 1e6*decode_float(cp,optlen);
      break;
    case PL_TONE:
      channel->fm.tone_freq = decode_float(cp,optlen);
      break;
    case PL_DEVIATION:
      channel->fm.tone_deviation = decode_float(cp,optlen);
      break;
    case NONCOHERENT_BIN_BW:
      channel->spectrum.bin_bw = decode_float(cp,optlen);
      break;
    case BIN_COUNT:
      channel->spectrum.bin_count = decode_int(cp,optlen);
      break;
    case BIN_DATA:
      break;
    case RF_GAIN:
      Frontend.rf_gain = decode_float(cp,optlen);
      break;
    case RF_ATTEN:
      Frontend.rf_atten = decode_float(cp,optlen);
      break;
    case BLOCKS_SINCE_POLL:
      channel->blocks_since_poll = decode_int64(cp,optlen);
      break;
    case PRESET:
      {
	char *p = decode_string(cp,optlen);
	strlcpy(channel->preset,p,sizeof(channel->preset));
	FREE(p);
      }
    default: // ignore others
      break;
    }
    cp += optlen;
  }
  return 0;
}
