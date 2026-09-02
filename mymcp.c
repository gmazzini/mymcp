// Gianluca Mazzini @2026- Version 1.08

#include <arpa/inet.h>
#include <cjson/cJSON.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define SERVER_NAME "mymcp"
#define SERVER_VERSION "1.08"
#define PROTOCOL_VERSION "2026-07-28"
#define WORK_DIR "/home/tools/mcp/work"
#ifndef JOBS_DIR
#define JOBS_DIR "/home/tools/mcp/work/jobs"
#endif
#ifndef LOG_FILE
#define LOG_FILE "/home/tools/mcp/mcp.log"
#endif
#define DEFAULT_ADDR "127.0.0.1"
#define DEFAULT_PORT 8000
#define BACKLOG 32
#define MAX_HEADER 32768
#define MAX_BODY 8388608
#define MAX_OUTPUT 200000
#define MAX_BLOB_CHUNK 1048576
#define MAX_LIST_FILES 10000
#define MAX_TAIL_LINES 1000
#define RUN_TIMEOUT 60
#define JOB_ID_LEN 16
#define CHAT_MAX 64
#define CLIENT_MAX 96
#define LOG_VALUE_MAX 2048
#define LOG_DETAIL_MAX 4608
#define LOG_LINE_MAX 5632
#define AGENT_ID_MAX 64
#define AGENT_TOKEN_MAX 255
#define AGENT_NAME_MAX 64
#define AGENT_REQUEST_ID_LEN 20
#define AGENT_WAIT_SECONDS 25
#define AGENT_CALL_TIMEOUT 30
#define AGENT_CALL_TIMEOUT_MAX 300
#define AGENT_CONFIG_DEFAULT "/home/tools/mcp/agent.conf"
#define AGENT_DIR_DEFAULT "/home/tools/mcp/agent"

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

typedef struct {
  char method[16],path[256],mcp_version[64],mcp_method[128],mcp_name[256];
  char agent_id[AGENT_ID_MAX+1],agent_action[16],agent_token[AGENT_TOKEN_MAX+1];
  char *body;
  size_t body_len;
} HttpRequest;

typedef struct {
  char job_id[JOB_ID_LEN+1],chat[CHAT_MAX+1],cwd[PATH_MAX],started_at[32];
  char stdout_path[PATH_MAX],stderr_path[PATH_MAX];
  char *command;
  pid_t pid,pgid;
  unsigned long long pid_start_time;
  double started_epoch;
  int stop_signal;
} JobMeta;

typedef struct {
  pid_t pid,pgrp;
  unsigned long long utime,stime,starttime;
  long rss_pages;
} ProcInfo;

typedef struct {
  pid_t *pids;
  int count,capacity;
  double cpu_percent;
  unsigned long long rss_bytes;
} GroupStats;

typedef struct {
  char job_id[JOB_ID_LEN+1];
  double started_epoch;
} JobIndex;

static double now_seconds(void) {
  struct timeval tv;

  gettimeofday(&tv,NULL);
  return (double)tv.tv_sec+(double)tv.tv_usec/1000000.0;
}

static int valid_chat(const char *chat) {
  size_t i,len;
  unsigned char c;

  if(chat==NULL) return 0;
  len=strlen(chat);
  if(len<1 || len>CHAT_MAX) return 0;
  for(i=0;i<len;i++) {
    c=(unsigned char)chat[i];
    if(!isalnum(c) && c!='_' && c!='-' && c!='.') return 0;
  }
  return 1;
}

static int check_log_file(void) {
  int fd;

  fd=open(LOG_FILE,O_WRONLY|O_CREAT|O_APPEND,0644);
  if(fd<0) return -1;
  close(fd);
  return 0;
}

static void log_quote_value(const char *src,char *dst,size_t dst_size) {
  size_t i,j;
  unsigned char c;
  int truncated;

  if(dst_size==0) return;
  i=0; j=0; truncated=0;
  if(j+1<dst_size) dst[j++]='"';
  if(src!=NULL) {
    for(i=0;src[i]!=0 && i<LOG_VALUE_MAX;i++) {
      c=(unsigned char)src[i];
      if(c=='\n' || c=='\r' || c=='\t' || iscntrl(c)) c=' ';
      if((c=='"' || c=='\\') && j+2<dst_size) dst[j++]='\\';
      if(j+1>=dst_size) { truncated=1; break; }
      dst[j++]=(char)c;
    }
    if(src[i]!=0) truncated=1;
  }
  if(truncated && j+4<dst_size) {
    dst[j++]='.'; dst[j++]='.'; dst[j++]='.';
  }
  if(j+1<dst_size) dst[j++]='"';
  dst[j]=0;
}

static void log_request_detail(const char *chat,const char *client,const char *method,
  const char *name,const char *detail,int rc,double elapsed_ms) {
  char timestamp[64],line[LOG_LINE_MAX];
  struct tm tmv;
  time_t now;
  int fd,n;

  now=time(NULL);
  localtime_r(&now,&tmv);
  strftime(timestamp,sizeof(timestamp),"%Y-%m-%dT%H:%M:%S%z",&tmv);
  if(name!=NULL && name[0]!=0 && detail!=NULL && detail[0]!=0)
    n=snprintf(line,sizeof(line),"%s chat=%s client=%s method=%s name=%s %s rc=%d elapsed_ms=%.0f\n",
      timestamp,chat!=NULL?chat:"-",client!=NULL?client:"unknown",method!=NULL?method:"-",
      name,detail,rc,elapsed_ms);
  else if(name!=NULL && name[0]!=0)
    n=snprintf(line,sizeof(line),"%s chat=%s client=%s method=%s name=%s rc=%d elapsed_ms=%.0f\n",
      timestamp,chat!=NULL?chat:"-",client!=NULL?client:"unknown",method!=NULL?method:"-",
      name,rc,elapsed_ms);
  else
    n=snprintf(line,sizeof(line),"%s chat=%s client=%s method=%s rc=%d elapsed_ms=%.0f\n",
      timestamp,chat!=NULL?chat:"-",client!=NULL?client:"unknown",method!=NULL?method:"-",
      rc,elapsed_ms);
  if(n<0 || (size_t)n>=sizeof(line)) return;
  fd=open(LOG_FILE,O_WRONLY|O_CREAT|O_APPEND,0644);
  if(fd<0) return;
  write(fd,line,(size_t)n);
  close(fd);
}

static void log_request(const char *chat,const char *client,const char *method,
  const char *name,int rc,double elapsed_ms) {
  log_request_detail(chat,client,method,name,NULL,rc,elapsed_ms);
}

static int send_all(int fd,const char *buf,size_t len) {
  ssize_t n;
  size_t sent;

  sent=0;
  for(;sent<len;) {
    n=send(fd,buf+sent,len-sent,0);
    if(n<0) {
      if(errno==EINTR) continue;
      return -1;
    }
    if(n==0) return -1;
    sent+=(size_t)n;
  }
  return 0;
}

static int path_is_inside_work(const char *path) {
  size_t n;

  n=strlen(WORK_DIR);
  if(strncmp(path,WORK_DIR,n)!=0) return 0;
  if(path[n]==0 || path[n]=='/') return 1;
  return 0;
}

static int safe_existing_path(const char *rel,char *out,size_t out_size,int want_dir) {
  char candidate[PATH_MAX],resolved[PATH_MAX];
  struct stat st;
  int n;

  if(rel==NULL || rel[0]==0 || rel[0]=='/') return -1;
  n=snprintf(candidate,sizeof(candidate),"%s/%s",WORK_DIR,rel);
  if(n<0 || (size_t)n>=sizeof(candidate)) return -1;
  if(realpath(candidate,resolved)==NULL) return -1;
  if(!path_is_inside_work(resolved)) return -1;
  if(stat(resolved,&st)!=0) return -1;
  if(want_dir && !S_ISDIR(st.st_mode)) return -1;
  if(!want_dir && !S_ISREG(st.st_mode)) return -1;
  if(strlen(resolved)+1>out_size) return -1;
  strcpy(out,resolved);
  return 0;
}

static int ensure_write_path(const char *rel,char *out,size_t out_size) {
  char tmp[PATH_MAX],current[PATH_MAX],final_path[PATH_MAX];
  char *p,*slash;
  struct stat st;
  size_t base_len;
  int n;

  if(rel==NULL || rel[0]==0 || rel[0]=='/') return -1;
  if(strlen(rel)>=sizeof(tmp)) return -1;
  strcpy(tmp,rel);
  p=tmp;
  for(;*p!=0;p++) {
    if(*p=='\\') *p='/';
  }
  p=tmp;
  for(;;) {
    if(p[0]=='.' && (p[1]==0 || p[1]=='/')) return -1;
    if(p[0]=='.' && p[1]=='.' && (p[2]==0 || p[2]=='/')) return -1;
    slash=strchr(p,'/');
    if(slash==NULL) break;
    if(slash==p) return -1;
    p=slash+1;
    if(*p==0) return -1;
  }
  strcpy(current,WORK_DIR);
  base_len=strlen(current);
  p=tmp;
  for(;;) {
    slash=strchr(p,'/');
    if(slash==NULL) break;
    *slash=0;
    if(strlen(current)+1+strlen(p)+1>sizeof(current)) return -1;
    current[base_len]='/';
    strcpy(current+base_len+1,p);
    base_len=strlen(current);
    if(lstat(current,&st)==0) {
      if(S_ISLNK(st.st_mode) || !S_ISDIR(st.st_mode)) return -1;
    } else {
      if(errno!=ENOENT) return -1;
      if(mkdir(current,0755)!=0) return -1;
    }
    p=slash+1;
  }
  n=snprintf(final_path,sizeof(final_path),"%s/%s",current,p);
  if(n<0 || (size_t)n>=sizeof(final_path)) return -1;
  if(lstat(final_path,&st)==0 && S_ISLNK(st.st_mode)) return -1;
  if(!path_is_inside_work(final_path)) return -1;
  if(strlen(final_path)+1>out_size) return -1;
  strcpy(out,final_path);
  return 0;
}

static char *read_file_alloc(const char *path,size_t max_bytes,size_t *size_out) {
  FILE *f;
  char *buf;
  long sz;
  size_t got,size;

  f=fopen(path,"rb");
  if(f==NULL) return NULL;
  if(fseek(f,0,SEEK_END)!=0) {
    fclose(f);
    return NULL;
  }
  sz=ftell(f);
  if(sz<0) {
    fclose(f);
    return NULL;
  }
  size=(size_t)sz;
  if(max_bytes>0 && size>max_bytes) size=max_bytes;
  if(fseek(f,0,SEEK_SET)!=0) {
    fclose(f);
    return NULL;
  }
  buf=(char *)malloc(size+1);
  if(buf==NULL) {
    fclose(f);
    return NULL;
  }
  got=fread(buf,1,size,f);
  fclose(f);
  buf[got]=0;
  if(size_out!=NULL) *size_out=got;
  return buf;
}

static char *base64_encode(const unsigned char *data,size_t len) {
  static const char table[]="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  char *out;
  size_t i,j,out_len;
  unsigned int a,b,c;

  out_len=((len+2)/3)*4;
  out=(char *)malloc(out_len+1);
  if(out==NULL) return NULL;
  for(i=0,j=0;i<len;i+=3) {
    a=data[i];
    b=i+1<len?data[i+1]:0;
    c=i+2<len?data[i+2]:0;
    out[j++]=table[(a>>2)&63];
    out[j++]=table[((a&3)<<4)|((b>>4)&15)];
    out[j++]=i+1<len?table[((b&15)<<2)|((c>>6)&3)]:'=';
    out[j++]=i+2<len?table[c&63]:'=';
  }
  out[j]=0;
  return out;
}

static int base64_value(unsigned char c) {
  if(c>='A' && c<='Z') return c-'A';
  if(c>='a' && c<='z') return c-'a'+26;
  if(c>='0' && c<='9') return c-'0'+52;
  if(c=='+') return 62;
  if(c=='/') return 63;
  return -1;
}

static unsigned char *base64_decode(const char *text,size_t *out_len) {
  unsigned char *out;
  size_t len,i,j,max_len;
  int a,b,c,d;
  unsigned int value;

  if(text==NULL || out_len==NULL) return NULL;
  len=strlen(text);
  if((len%4)!=0) return NULL;
  max_len=(len/4)*3;
  out=(unsigned char *)malloc(max_len+1);
  if(out==NULL) return NULL;
  j=0;
  for(i=0;i<len;i+=4) {
    a=base64_value((unsigned char)text[i]);
    b=base64_value((unsigned char)text[i+1]);
    c=text[i+2]=='='?-2:base64_value((unsigned char)text[i+2]);
    d=text[i+3]=='='?-2:base64_value((unsigned char)text[i+3]);
    if(a<0 || b<0 || c==-1 || d==-1 || (c==-2 && d!=-2) || ((c==-2 || d==-2) && i+4!=len)) {
      free(out);
      return NULL;
    }
    value=((unsigned int)a<<18)|((unsigned int)b<<12);
    if(c>=0) value|=(unsigned int)c<<6;
    if(d>=0) value|=(unsigned int)d;
    out[j++]=(unsigned char)((value>>16)&255);
    if(c>=0) out[j++]=(unsigned char)((value>>8)&255);
    if(d>=0) out[j++]=(unsigned char)(value&255);
  }
  *out_len=j;
  return out;
}

static int list_entries(cJSON *array,const char *abs_dir,const char *rel_dir,int recursive,int *count) {
  DIR *d;
  struct dirent *de;
  struct stat st;
  char abs_path[PATH_MAX],rel_path[PATH_MAX];
  cJSON *item;
  int n,rc;

  d=opendir(abs_dir);
  if(d==NULL) return -1;
  for(;;) {
    de=readdir(d);
    if(de==NULL) break;
    if(strcmp(de->d_name,".")==0 || strcmp(de->d_name,"..")==0) continue;
    n=snprintf(abs_path,sizeof(abs_path),"%s/%s",abs_dir,de->d_name);
    if(n<0 || (size_t)n>=sizeof(abs_path)) {
      closedir(d);
      return -1;
    }
    if(rel_dir[0]!=0) n=snprintf(rel_path,sizeof(rel_path),"%s/%s",rel_dir,de->d_name);
    else n=snprintf(rel_path,sizeof(rel_path),"%s",de->d_name);
    if(n<0 || (size_t)n>=sizeof(rel_path)) {
      closedir(d);
      return -1;
    }
    if(lstat(abs_path,&st)!=0) {
      closedir(d);
      return -1;
    }
    if(S_ISLNK(st.st_mode)) continue;
    if(!S_ISDIR(st.st_mode) && !S_ISREG(st.st_mode)) continue;
    if(*count>=MAX_LIST_FILES) {
      closedir(d);
      return -2;
    }
    item=cJSON_CreateObject();
    if(item==NULL) {
      closedir(d);
      return -1;
    }
    cJSON_AddStringToObject(item,"path",rel_path);
    if(S_ISDIR(st.st_mode)) cJSON_AddStringToObject(item,"type","dir");
    else cJSON_AddStringToObject(item,"type","file");
    cJSON_AddNumberToObject(item,"size",S_ISREG(st.st_mode)?(double)st.st_size:0.0);
    cJSON_AddNumberToObject(item,"mtime",(double)st.st_mtime);
    cJSON_AddItemToArray(array,item);
    (*count)++;
    if(S_ISDIR(st.st_mode) && recursive) {
      rc=list_entries(array,abs_path,rel_path,recursive,count);
      if(rc!=0) {
        closedir(d);
        return rc;
      }
    }
  }
  closedir(d);
  return 0;
}

static int write_text_file(const char *path,const char *text) {
  FILE *f;
  size_t len,written;

  f=fopen(path,"wb");
  if(f==NULL) return -1;
  len=strlen(text);
  written=fwrite(text,1,len,f);
  if(fclose(f)!=0) return -1;
  if(written!=len) return -1;
  return 0;
}

static void add_server_meta(cJSON *result) {
  cJSON *meta,*info;

  meta=cJSON_AddObjectToObject(result,"_meta");
  info=cJSON_AddObjectToObject(meta,"io.modelcontextprotocol/serverInfo");
  cJSON_AddStringToObject(info,"name",SERVER_NAME);
  cJSON_AddStringToObject(info,"version",SERVER_VERSION);
}

static cJSON *jsonrpc_result(cJSON *id,cJSON *result) {
  cJSON *root,*copy;

  root=cJSON_CreateObject();
  if(root==NULL) return NULL;
  cJSON_AddStringToObject(root,"jsonrpc","2.0");
  copy=id!=NULL?cJSON_Duplicate(id,1):cJSON_CreateNull();
  cJSON_AddItemToObject(root,"id",copy);
  cJSON_AddItemToObject(root,"result",result);
  return root;
}

static cJSON *jsonrpc_error(cJSON *id,int code,const char *message) {
  cJSON *root,*error,*copy;

  root=cJSON_CreateObject();
  if(root==NULL) return NULL;
  cJSON_AddStringToObject(root,"jsonrpc","2.0");
  copy=id!=NULL?cJSON_Duplicate(id,1):cJSON_CreateNull();
  cJSON_AddItemToObject(root,"id",copy);
  error=cJSON_AddObjectToObject(root,"error");
  cJSON_AddNumberToObject(error,"code",code);
  cJSON_AddStringToObject(error,"message",message);
  return root;
}

static cJSON *jsonrpc_unsupported(cJSON *id,const char *requested) {
  cJSON *root,*error,*data,*supported,*copy;

  root=cJSON_CreateObject();
  if(root==NULL) return NULL;
  cJSON_AddStringToObject(root,"jsonrpc","2.0");
  copy=id!=NULL?cJSON_Duplicate(id,1):cJSON_CreateNull();
  cJSON_AddItemToObject(root,"id",copy);
  error=cJSON_AddObjectToObject(root,"error");
  cJSON_AddNumberToObject(error,"code",-32022);
  cJSON_AddStringToObject(error,"message","Unsupported protocol version");
  data=cJSON_AddObjectToObject(error,"data");
  supported=cJSON_AddArrayToObject(data,"supported");
  cJSON_AddItemToArray(supported,cJSON_CreateString(PROTOCOL_VERSION));
  cJSON_AddStringToObject(data,"requested",requested!=NULL?requested:"");
  return root;
}

static int send_json(int fd,int status,cJSON *root) {
  char header[512],*json;
  const char *status_text;
  size_t len;
  int n,rc;

  status_text=status==200?"OK":status==400?"Bad Request":status==404?"Not Found":"Method Not Allowed";
  json=cJSON_PrintUnformatted(root);
  if(json==NULL) return -1;
  len=strlen(json);
  n=snprintf(header,sizeof(header),
    "HTTP/1.1 %d %s\r\n"
    "Content-Type: application/json\r\n"
    "MCP-Protocol-Version: %s\r\n"
    "Content-Length: %lu\r\n"
    "Connection: close\r\n\r\n",
    status,status_text,PROTOCOL_VERSION,(unsigned long)len);
  if(n<0 || (size_t)n>=sizeof(header)) {
    free(json);
    return -1;
  }
  rc=send_all(fd,header,(size_t)n);
  if(rc==0) rc=send_all(fd,json,len);
  free(json);
  return rc;
}

static int send_agent_json(int fd,int status,cJSON *root) {
  char header[512],*json;
  const char *status_text;
  size_t len;
  int n,rc;

  if(status==200) status_text="OK";
  else if(status==400) status_text="Bad Request";
  else if(status==403) status_text="Forbidden";
  else if(status==404) status_text="Not Found";
  else if(status==409) status_text="Conflict";
  else if(status==503) status_text="Service Unavailable";
  else status_text="Internal Server Error";
  json=cJSON_PrintUnformatted(root);
  if(json==NULL) return -1;
  len=strlen(json);
  n=snprintf(header,sizeof(header),
    "HTTP/1.1 %d %s\r\n"
    "Content-Type: application/json\r\n"
    "Content-Length: %lu\r\n"
    "Connection: close\r\n\r\n",
    status,status_text,(unsigned long)len);
  if(n<0 || (size_t)n>=sizeof(header)) {
    free(json);
    return -1;
  }
  rc=send_all(fd,header,(size_t)n);
  if(rc==0) rc=send_all(fd,json,len);
  free(json);
  return rc;
}

static int valid_agent_name(const char *name) {
  size_t i,len;
  unsigned char c;

  if(name==NULL) return 0;
  len=strlen(name);
  if(len<1 || len>AGENT_NAME_MAX) return 0;
  for(i=0;i<len;i++) {
    c=(unsigned char)name[i];
    if(!isalnum(c) && c!='_' && c!='-' && c!='.') return 0;
  }
  return 1;
}

static int valid_agent_id(const char *agent_id) {
  if(agent_id==NULL || strlen(agent_id)>AGENT_ID_MAX) return 0;
  return valid_agent_name(agent_id);
}

static int secure_token_equal(const char *a,const char *b) {
  size_t i,la,lb;
  unsigned char diff;

  if(a==NULL || b==NULL) return 0;
  la=strlen(a);
  lb=strlen(b);
  if(la!=lb) return 0;
  diff=0;
  for(i=0;i<la;i++) diff|=(unsigned char)a[i]^(unsigned char)b[i];
  return diff==0;
}

static const char *agent_config_path(void) {
  const char *p;

  p=getenv("MYMCP_AGENT_CONFIG");
  return p!=NULL && p[0]!=0?p:AGENT_CONFIG_DEFAULT;
}

static const char *agent_root_dir(void) {
  const char *p;

  p=getenv("MYMCP_AGENT_DIR");
  return p!=NULL && p[0]!=0?p:AGENT_DIR_DEFAULT;
}

static int agent_module_allowed(const char *modules,const char *module) {
  const char *p,*end;
  size_t len,module_len;

  if(modules==NULL || module==NULL) return 0;
  module_len=strlen(module);
  p=modules;
  for(;*p!=0;) {
    end=strchr(p,',');
    len=end!=NULL?(size_t)(end-p):strlen(p);
    if(len==module_len && strncmp(p,module,len)==0) return 1;
    if(end==NULL) break;
    p=end+1;
  }
  return 0;
}

static int agent_authenticate(const char *agent_id,const char *token,char *modules,size_t modules_size) {
  FILE *f;
  char line[2048],id[AGENT_ID_MAX+1],file_token[AGENT_TOKEN_MAX+1],file_modules[1024],*p;
  int fields;

  if(!valid_agent_id(agent_id) || token==NULL || token[0]==0 || strlen(token)>AGENT_TOKEN_MAX || modules_size==0) return -1;
  f=fopen(agent_config_path(),"r");
  if(f==NULL) return -1;
  for(;fgets(line,sizeof(line),f)!=NULL;) {
    p=line;
    for(;*p==' ' || *p=='\t';p++);
    if(*p==0 || *p=='\n' || *p=='#') continue;
    fields=sscanf(p,"%64s %255s %1023s",id,file_token,file_modules);
    if(fields!=3 || strcmp(id,agent_id)!=0) continue;
    if(!secure_token_equal(file_token,token)) continue;
    if(strlen(file_modules)+1>modules_size) {
      fclose(f);
      return -1;
    }
    strcpy(modules,file_modules);
    fclose(f);
    return 0;
  }
  fclose(f);
  return -1;
}

static int agent_module_configured(const char *module,const char *target_agent) {
  FILE *f;
  char line[2048],id[AGENT_ID_MAX+1],token[AGENT_TOKEN_MAX+1],modules[1024],*p;
  int fields,found;

  if(!valid_agent_name(module)) return 0;
  if(target_agent!=NULL && target_agent[0]!=0 && !valid_agent_id(target_agent)) return 0;
  f=fopen(agent_config_path(),"r");
  if(f==NULL) return 0;
  found=0;
  for(;fgets(line,sizeof(line),f)!=NULL;) {
    p=line;
    for(;*p==' ' || *p=='\t';p++);
    if(*p==0 || *p=='\n' || *p=='#') continue;
    fields=sscanf(p,"%64s %255s %1023s",id,token,modules);
    if(fields!=3) continue;
    if(target_agent!=NULL && target_agent[0]!=0 && strcmp(id,target_agent)!=0) continue;
    if(agent_module_allowed(modules,module)) {
      found=1;
      break;
    }
  }
  fclose(f);
  return found;
}

static int ensure_agent_dir_one(const char *path) {
  struct stat st;

  if(lstat(path,&st)==0) return S_ISDIR(st.st_mode) && !S_ISLNK(st.st_mode)?0:-1;
  if(errno!=ENOENT) return -1;
  if(mkdir(path,0700)!=0) return -1;
  return 0;
}

static int ensure_agent_dirs(void) {
  const char *root;
  char path[PATH_MAX];
  int n;

  root=agent_root_dir();
  if(root[0]!='/') return -1;
  if(ensure_agent_dir_one(root)!=0) return -1;
  n=snprintf(path,sizeof(path),"%s/queue",root);
  if(n<0 || (size_t)n>=sizeof(path) || ensure_agent_dir_one(path)!=0) return -1;
  n=snprintf(path,sizeof(path),"%s/running",root);
  if(n<0 || (size_t)n>=sizeof(path) || ensure_agent_dir_one(path)!=0) return -1;
  n=snprintf(path,sizeof(path),"%s/done",root);
  if(n<0 || (size_t)n>=sizeof(path) || ensure_agent_dir_one(path)!=0) return -1;
  return 0;
}

static int valid_agent_request_id(const char *request_id) {
  int i;

  if(request_id==NULL || strlen(request_id)!=AGENT_REQUEST_ID_LEN) return 0;
  if(strncmp(request_id,"req_",4)!=0) return 0;
  for(i=4;i<AGENT_REQUEST_ID_LEN;i++) if(!isxdigit((unsigned char)request_id[i])) return 0;
  return 1;
}

static int make_agent_request_id(char *out,size_t out_size) {
  unsigned char raw[8];
  int fd,n,i;

  if(out_size<AGENT_REQUEST_ID_LEN+1) return -1;
  fd=open("/dev/urandom",O_RDONLY);
  if(fd<0) return -1;
  n=(int)read(fd,raw,sizeof(raw));
  close(fd);
  if(n!=(int)sizeof(raw)) return -1;
  strcpy(out,"req_");
  for(i=0;i<8;i++) sprintf(out+4+i*2,"%02x",raw[i]);
  out[AGENT_REQUEST_ID_LEN]=0;
  return 0;
}

static int agent_file_path(const char *state,const char *request_id,char *out,size_t out_size) {
  int n;

  if(!valid_agent_request_id(request_id)) return -1;
  if(strcmp(state,"queue")!=0 && strcmp(state,"running")!=0 && strcmp(state,"done")!=0) return -1;
  n=snprintf(out,out_size,"%s/%s/%s.json",agent_root_dir(),state,request_id);
  if(n<0 || (size_t)n>=out_size) return -1;
  return 0;
}

static int write_json_atomic(const char *path,cJSON *root) {
  char tmp[PATH_MAX],*text;
  int n,rc;

  n=snprintf(tmp,sizeof(tmp),"%s.new.%ld",path,(long)getpid());
  if(n<0 || (size_t)n>=sizeof(tmp)) return -1;
  text=cJSON_PrintUnformatted(root);
  if(text==NULL) return -1;
  rc=write_text_file(tmp,text);
  free(text);
  if(rc!=0) {
    unlink(tmp);
    return -1;
  }
  if(rename(tmp,path)!=0) {
    unlink(tmp);
    return -1;
  }
  return 0;
}

static cJSON *read_json_path(const char *path) {
  char *text;
  cJSON *root;

  text=read_file_alloc(path,MAX_BODY,NULL);
  if(text==NULL) return NULL;
  root=cJSON_Parse(text);
  free(text);
  return root;
}

static int agent_enqueue(const char *chat,const char *target_agent,const char *module,const char *action,
  cJSON *payload,char *request_id,size_t request_id_size) {
  cJSON *root,*copy;
  char path[PATH_MAX];
  int attempts;

  if(ensure_agent_dirs()!=0) return -1;
  for(attempts=0;attempts<20;attempts++) {
    if(make_agent_request_id(request_id,request_id_size)!=0) return -1;
    if(agent_file_path("queue",request_id,path,sizeof(path))!=0) return -1;
    if(access(path,F_OK)!=0) break;
  }
  if(attempts==20) return -1;
  root=cJSON_CreateObject();
  if(root==NULL) return -1;
  cJSON_AddNumberToObject(root,"version",1);
  cJSON_AddStringToObject(root,"request_id",request_id);
  cJSON_AddStringToObject(root,"chat",chat);
  if(target_agent!=NULL && target_agent[0]!=0) cJSON_AddStringToObject(root,"target_agent",target_agent);
  cJSON_AddStringToObject(root,"module",module);
  cJSON_AddStringToObject(root,"action",action);
  copy=payload!=NULL?cJSON_Duplicate(payload,1):cJSON_CreateObject();
  if(copy==NULL) {
    cJSON_Delete(root);
    return -1;
  }
  cJSON_AddItemToObject(root,"payload",copy);
  cJSON_AddNumberToObject(root,"created_epoch",now_seconds());
  if(write_json_atomic(path,root)!=0) {
    cJSON_Delete(root);
    return -1;
  }
  cJSON_Delete(root);
  return 0;
}

static int request_filename_id(const char *name,char *request_id,size_t out_size) {
  size_t len,id_len;

  if(name==NULL) return -1;
  len=strlen(name);
  if(len<6 || strcmp(name+len-5,".json")!=0) return -1;
  id_len=len-5;
  if(id_len+1>out_size) return -1;
  memcpy(request_id,name,id_len);
  request_id[id_len]=0;
  return valid_agent_request_id(request_id)?0:-1;
}

static cJSON *agent_claim_next(const char *agent_id,const char *modules) {
  DIR *d;
  struct dirent *de;
  cJSON *root,*module_item,*target_item,*created_item;
  char dir_path[PATH_MAX],path[PATH_MAX],best_id[AGENT_REQUEST_ID_LEN+1],request_id[AGENT_REQUEST_ID_LEN+1];
  char queue_path[PATH_MAX],running_path[PATH_MAX];
  double created,best_created;
  int n,have_best;

  n=snprintf(dir_path,sizeof(dir_path),"%s/queue",agent_root_dir());
  if(n<0 || (size_t)n>=sizeof(dir_path)) return NULL;
  d=opendir(dir_path);
  if(d==NULL) return NULL;
  have_best=0;
  best_created=0.0;
  best_id[0]=0;
  for(; (de=readdir(d))!=NULL;) {
    if(request_filename_id(de->d_name,request_id,sizeof(request_id))!=0) continue;
    if(agent_file_path("queue",request_id,path,sizeof(path))!=0) continue;
    root=read_json_path(path);
    if(!cJSON_IsObject(root)) {
      cJSON_Delete(root);
      continue;
    }
    module_item=cJSON_GetObjectItemCaseSensitive(root,"module");
    target_item=cJSON_GetObjectItemCaseSensitive(root,"target_agent");
    created_item=cJSON_GetObjectItemCaseSensitive(root,"created_epoch");
    if(!cJSON_IsString(module_item) || !agent_module_allowed(modules,module_item->valuestring) || !cJSON_IsNumber(created_item)) {
      cJSON_Delete(root);
      continue;
    }
    if(cJSON_IsString(target_item) && strcmp(target_item->valuestring,agent_id)!=0) {
      cJSON_Delete(root);
      continue;
    }
    created=created_item->valuedouble;
    if(!have_best || created<best_created) {
      strcpy(best_id,request_id);
      best_created=created;
      have_best=1;
    }
    cJSON_Delete(root);
  }
  closedir(d);
  if(!have_best) return NULL;
  if(agent_file_path("queue",best_id,queue_path,sizeof(queue_path))!=0 ||
    agent_file_path("running",best_id,running_path,sizeof(running_path))!=0) return NULL;
  if(rename(queue_path,running_path)!=0) return NULL;
  root=read_json_path(running_path);
  if(!cJSON_IsObject(root)) {
    cJSON_Delete(root);
    rename(running_path,queue_path);
    return NULL;
  }
  cJSON_AddStringToObject(root,"agent_id",agent_id);
  cJSON_AddNumberToObject(root,"claimed_epoch",now_seconds());
  if(write_json_atomic(running_path,root)!=0) {
    cJSON_Delete(root);
    rename(running_path,queue_path);
    return NULL;
  }
  return root;
}

static cJSON *agent_done_read(const char *request_id) {
  char path[PATH_MAX];

  if(agent_file_path("done",request_id,path,sizeof(path))!=0) return NULL;
  return read_json_path(path);
}

static int agent_request_state(const char *request_id) {
  char path[PATH_MAX];

  if(agent_file_path("done",request_id,path,sizeof(path))==0 && access(path,F_OK)==0) return 3;
  if(agent_file_path("running",request_id,path,sizeof(path))==0 && access(path,F_OK)==0) return 2;
  if(agent_file_path("queue",request_id,path,sizeof(path))==0 && access(path,F_OK)==0) return 1;
  return 0;
}

static int agent_cancel_queued(const char *request_id) {
  char path[PATH_MAX];

  if(agent_file_path("queue",request_id,path,sizeof(path))!=0) return -1;
  if(unlink(path)==0 || errno==ENOENT) return 0;
  return -1;
}

static int agent_store_result(const char *agent_id,const char *request_id,const char *status,cJSON *result_item,
  const char *error_text) {
  cJSON *request,*done,*owner,*item,*copy;
  char running_path[PATH_MAX],done_path[PATH_MAX];

  if(agent_file_path("done",request_id,done_path,sizeof(done_path))!=0 ||
    agent_file_path("running",request_id,running_path,sizeof(running_path))!=0) return -1;
  done=read_json_path(done_path);
  if(cJSON_IsObject(done)) {
    owner=cJSON_GetObjectItemCaseSensitive(done,"agent_id");
    if(cJSON_IsString(owner) && strcmp(owner->valuestring,agent_id)==0) {
      cJSON_Delete(done);
      return 1;
    }
  }
  cJSON_Delete(done);
  request=read_json_path(running_path);
  if(!cJSON_IsObject(request)) {
    cJSON_Delete(request);
    return -2;
  }
  owner=cJSON_GetObjectItemCaseSensitive(request,"agent_id");
  if(!cJSON_IsString(owner) || strcmp(owner->valuestring,agent_id)!=0) {
    cJSON_Delete(request);
    return -2;
  }
  done=cJSON_CreateObject();
  if(done==NULL) {
    cJSON_Delete(request);
    return -1;
  }
  cJSON_AddStringToObject(done,"request_id",request_id);
  cJSON_AddStringToObject(done,"agent_id",agent_id);
  item=cJSON_GetObjectItemCaseSensitive(request,"chat");
  if(item!=NULL) cJSON_AddItemToObject(done,"chat",cJSON_Duplicate(item,1));
  item=cJSON_GetObjectItemCaseSensitive(request,"module");
  if(item!=NULL) cJSON_AddItemToObject(done,"module",cJSON_Duplicate(item,1));
  item=cJSON_GetObjectItemCaseSensitive(request,"action");
  if(item!=NULL) cJSON_AddItemToObject(done,"action",cJSON_Duplicate(item,1));
  cJSON_AddStringToObject(done,"status",status);
  if(strcmp(status,"ok")==0) {
    copy=result_item!=NULL?cJSON_Duplicate(result_item,1):cJSON_CreateNull();
    if(copy==NULL) {
      cJSON_Delete(done);
      cJSON_Delete(request);
      return -1;
    }
    cJSON_AddItemToObject(done,"result",copy);
  } else cJSON_AddStringToObject(done,"error",error_text!=NULL?error_text:"agent error");
  cJSON_AddNumberToObject(done,"completed_epoch",now_seconds());
  if(write_json_atomic(done_path,done)!=0) {
    cJSON_Delete(done);
    cJSON_Delete(request);
    return -1;
  }
  unlink(running_path);
  cJSON_Delete(done);
  cJSON_Delete(request);
  return 0;
}

static void free_http_request(HttpRequest *req) {
  if(req->body!=NULL) free(req->body);
  req->body=NULL;
}

static int read_http_request(int fd,HttpRequest *req) {
  char header[MAX_HEADER+1],line[4096],*end,*save,*p,*colon,*value;
  ssize_t n;
  size_t used,header_len,already,content_len,need;
  int first;

  memset(req,0,sizeof(*req));
  used=0;
  end=NULL;
  for(;used<MAX_HEADER;) {
    n=recv(fd,header+used,MAX_HEADER-used,0);
    if(n<0) {
      if(errno==EINTR) continue;
      return -1;
    }
    if(n==0) return -1;
    used+=(size_t)n;
    header[used]=0;
    end=strstr(header,"\r\n\r\n");
    if(end!=NULL) break;
  }
  if(end==NULL) return -1;
  header_len=(size_t)(end-header)+4;
  content_len=0;
  *end=0;
  save=NULL;
  p=strtok_r(header,"\r\n",&save);
  first=1;
  for(;p!=NULL;p=strtok_r(NULL,"\r\n",&save)) {
    if(first) {
      if(sscanf(p,"%15s %255s",req->method,req->path)!=2) return -1;
      first=0;
      continue;
    }
    if(strlen(p)>=sizeof(line)) return -1;
    strcpy(line,p);
    colon=strchr(line,':');
    if(colon==NULL) continue;
    *colon=0;
    value=colon+1;
    for(;*value==' ' || *value=='\t';value++);
    if(strcasecmp(line,"Content-Length")==0) content_len=(size_t)strtoul(value,NULL,10);
    else if(strcasecmp(line,"MCP-Protocol-Version")==0) snprintf(req->mcp_version,sizeof(req->mcp_version),"%s",value);
    else if(strcasecmp(line,"Mcp-Method")==0) snprintf(req->mcp_method,sizeof(req->mcp_method),"%s",value);
    else if(strcasecmp(line,"Mcp-Name")==0) snprintf(req->mcp_name,sizeof(req->mcp_name),"%s",value);
    else if(strcasecmp(line,"X-MCP-Agent")==0) snprintf(req->agent_id,sizeof(req->agent_id),"%s",value);
    else if(strcasecmp(line,"X-MCP-Agent-Action")==0) snprintf(req->agent_action,sizeof(req->agent_action),"%s",value);
    else if(strcasecmp(line,"X-MCP-Agent-Token")==0) snprintf(req->agent_token,sizeof(req->agent_token),"%s",value);
  }
  if(content_len>MAX_BODY) return -2;
  req->body=(char *)malloc(content_len+1);
  if(req->body==NULL) return -1;
  req->body_len=content_len;
  already=used-header_len;
  if(already>content_len) already=content_len;
  memcpy(req->body,((char *)end)+4,already);
  need=content_len-already;
  for(;need>0;) {
    n=recv(fd,req->body+already,need,0);
    if(n<0) {
      if(errno==EINTR) continue;
      free_http_request(req);
      return -1;
    }
    if(n==0) {
      free_http_request(req);
      return -1;
    }
    already+=(size_t)n;
    need-=(size_t)n;
  }
  req->body[content_len]=0;
  return 0;
}

static cJSON *agent_message(const char *status,const char *message) {
  cJSON *root;

  root=cJSON_CreateObject();
  if(root==NULL) return NULL;
  cJSON_AddStringToObject(root,"status",status);
  if(message!=NULL) cJSON_AddStringToObject(root,"message",message);
  return root;
}

static int handle_agent_connection(int fd,HttpRequest *http) {
  char modules[1024],request_id_copy[AGENT_REQUEST_ID_LEN+1];
  const char *request_id,*status_text,*error_text;
  cJSON *root,*response,*item,*result_item;
  double started;
  int rc;

  if(http->agent_id[0]==0 || http->agent_action[0]==0 || http->agent_token[0]==0) {
    response=agent_message("error","incomplete agent headers");
    send_agent_json(fd,403,response);
    cJSON_Delete(response);
    return 403;
  }
  if(agent_authenticate(http->agent_id,http->agent_token,modules,sizeof(modules))!=0) {
    response=agent_message("error","agent authentication failed");
    send_agent_json(fd,403,response);
    cJSON_Delete(response);
    return 403;
  }
  if(ensure_agent_dirs()!=0) {
    response=agent_message("error","agent runtime unavailable");
    send_agent_json(fd,503,response);
    cJSON_Delete(response);
    return 503;
  }
  if(strcmp(http->agent_action,"wait")==0) {
    started=now_seconds();
    for(;;) {
      root=agent_claim_next(http->agent_id,modules);
      if(root!=NULL) {
        cJSON_AddStringToObject(root,"status","request");
        send_agent_json(fd,200,root);
        cJSON_Delete(root);
        return 200;
      }
      if(now_seconds()-started>=(double)AGENT_WAIT_SECONDS) break;
      usleep(100000);
    }
    response=agent_message("idle",NULL);
    send_agent_json(fd,200,response);
    cJSON_Delete(response);
    return 200;
  }
  if(strcmp(http->agent_action,"result")!=0) {
    response=agent_message("error","unknown agent action");
    send_agent_json(fd,400,response);
    cJSON_Delete(response);
    return 400;
  }
  root=cJSON_ParseWithLength(http->body,http->body_len);
  if(!cJSON_IsObject(root)) {
    cJSON_Delete(root);
    response=agent_message("error","invalid result body");
    send_agent_json(fd,400,response);
    cJSON_Delete(response);
    return 400;
  }
  item=cJSON_GetObjectItemCaseSensitive(root,"request_id");
  request_id=cJSON_IsString(item)?item->valuestring:NULL;
  item=cJSON_GetObjectItemCaseSensitive(root,"status");
  status_text=cJSON_IsString(item)?item->valuestring:NULL;
  if(!valid_agent_request_id(request_id) || status_text==NULL ||
    (strcmp(status_text,"ok")!=0 && strcmp(status_text,"error")!=0)) {
    cJSON_Delete(root);
    response=agent_message("error","invalid request_id or status");
    send_agent_json(fd,400,response);
    cJSON_Delete(response);
    return 400;
  }
  result_item=cJSON_GetObjectItemCaseSensitive(root,"result");
  item=cJSON_GetObjectItemCaseSensitive(root,"error");
  error_text=cJSON_IsString(item)?item->valuestring:NULL;
  if(strcmp(status_text,"error")==0 && error_text==NULL) {
    cJSON_Delete(root);
    response=agent_message("error","error status requires error text");
    send_agent_json(fd,400,response);
    cJSON_Delete(response);
    return 400;
  }
  strcpy(request_id_copy,request_id);
  rc=agent_store_result(http->agent_id,request_id_copy,status_text,result_item,error_text);
  cJSON_Delete(root);
  if(rc==-2) {
    response=agent_message("error","request not owned by this agent");
    send_agent_json(fd,409,response);
    cJSON_Delete(response);
    return 409;
  }
  if(rc<0) {
    response=agent_message("error","cannot store agent result");
    send_agent_json(fd,503,response);
    cJSON_Delete(response);
    return 503;
  }
  response=agent_message("accepted",NULL);
  cJSON_AddStringToObject(response,"request_id",request_id_copy);
  if(rc==1) cJSON_AddBoolToObject(response,"duplicate",1);
  send_agent_json(fd,200,response);
  cJSON_Delete(response);
  return 200;
}

static cJSON *make_text_content(const char *text) {
  cJSON *array,*item;

  array=cJSON_CreateArray();
  item=cJSON_CreateObject();
  cJSON_AddStringToObject(item,"text",text!=NULL?text:"");
  cJSON_AddStringToObject(item,"type","text");
  cJSON_AddItemToArray(array,item);
  return array;
}

static cJSON *tool_result_string(const char *text,int is_error) {
  cJSON *result,*structured;

  result=cJSON_CreateObject();
  cJSON_AddItemToObject(result,"content",make_text_content(text));
  cJSON_AddBoolToObject(result,"isError",is_error?1:0);
  cJSON_AddStringToObject(result,"resultType","complete");
  structured=cJSON_AddObjectToObject(result,"structuredContent");
  cJSON_AddStringToObject(structured,"result",text!=NULL?text:"");
  add_server_meta(result);
  return result;
}

static cJSON *tool_result_json(cJSON *data,int is_error) {
  cJSON *result,*content,*item;
  char *text;

  text=cJSON_PrintUnformatted(data);
  result=cJSON_CreateObject();
  content=cJSON_AddArrayToObject(result,"content");
  item=cJSON_CreateObject();
  cJSON_AddStringToObject(item,"text",text!=NULL?text:"");
  cJSON_AddStringToObject(item,"type","text");
  cJSON_AddItemToArray(content,item);
  cJSON_AddBoolToObject(result,"isError",is_error?1:0);
  cJSON_AddStringToObject(result,"resultType","complete");
  cJSON_AddItemToObject(result,"structuredContent",data);
  add_server_meta(result);
  if(text!=NULL) free(text);
  return result;
}

static cJSON *schema_string(void) {
  cJSON *s;

  s=cJSON_CreateObject();
  cJSON_AddStringToObject(s,"type","string");
  return s;
}

static cJSON *schema_integer(int default_value,int has_default) {
  cJSON *s;

  s=cJSON_CreateObject();
  cJSON_AddStringToObject(s,"type","integer");
  if(has_default) cJSON_AddNumberToObject(s,"default",default_value);
  return s;
}

static cJSON *schema_boolean(int default_value,int has_default) {
  cJSON *s;

  s=cJSON_CreateObject();
  cJSON_AddStringToObject(s,"type","boolean");
  if(has_default) cJSON_AddBoolToObject(s,"default",default_value);
  return s;
}

static cJSON *new_tool(const char *name,const char *description) {
  cJSON *tool,*schema,*props,*chat,*required;

  tool=cJSON_CreateObject();
  cJSON_AddStringToObject(tool,"description",description);
  schema=cJSON_AddObjectToObject(tool,"inputSchema");
  cJSON_AddStringToObject(schema,"type","object");
  props=cJSON_AddObjectToObject(schema,"properties");
  chat=cJSON_AddObjectToObject(props,"chat");
  cJSON_AddStringToObject(chat,"type","string");
  cJSON_AddNumberToObject(chat,"minLength",1);
  cJSON_AddNumberToObject(chat,"maxLength",CHAT_MAX);
  cJSON_AddStringToObject(chat,"pattern","^[A-Za-z0-9_.-]+$");
  cJSON_AddStringToObject(chat,"description","Required chat name. Ask the user for it before the first tool call if it is not already known.");
  required=cJSON_AddArrayToObject(schema,"required");
  cJSON_AddItemToArray(required,cJSON_CreateString("chat"));
  cJSON_AddStringToObject(tool,"name",name);
  return tool;
}

static void require_field(cJSON *tool,const char *name) {
  cJSON *schema,*required;

  schema=cJSON_GetObjectItemCaseSensitive(tool,"inputSchema");
  required=cJSON_GetObjectItemCaseSensitive(schema,"required");
  if(required==NULL) required=cJSON_AddArrayToObject(schema,"required");
  cJSON_AddItemToArray(required,cJSON_CreateString(name));
}

static void add_property(cJSON *tool,const char *name,cJSON *schema) {
  cJSON *input,*props;

  input=cJSON_GetObjectItemCaseSensitive(tool,"inputSchema");
  props=cJSON_GetObjectItemCaseSensitive(input,"properties");
  cJSON_AddItemToObject(props,name,schema);
}

static cJSON *build_tools(void) {
  cJSON *tools,*tool,*s,*e;

  tools=cJSON_CreateArray();

  tool=new_tool("hello","Return a test message from the Debian server.");
  cJSON_AddItemToArray(tools,tool);

  tool=new_tool("write_file","Write a text file inside the MCP work directory.");
  add_property(tool,"path",schema_string());
  add_property(tool,"content",schema_string());
  require_field(tool,"path");
  require_field(tool,"content");
  cJSON_AddItemToArray(tools,tool);

  tool=new_tool("read_file","Read a text file inside the MCP work directory.");
  add_property(tool,"path",schema_string());
  require_field(tool,"path");
  cJSON_AddItemToArray(tools,tool);

  tool=new_tool("list_files","List files and directories inside the MCP work directory.");
  add_property(tool,"path",schema_string());
  add_property(tool,"recursive",schema_boolean(1,1));
  require_field(tool,"path");
  cJSON_AddItemToArray(tools,tool);

  tool=new_tool("read_blob","Read a binary-safe file chunk encoded as base64.");
  add_property(tool,"path",schema_string());
  s=schema_integer(0,1);
  cJSON_AddNumberToObject(s,"minimum",0);
  add_property(tool,"offset",s);
  s=schema_integer(MAX_BLOB_CHUNK,1);
  cJSON_AddNumberToObject(s,"minimum",1);
  cJSON_AddNumberToObject(s,"maximum",MAX_BLOB_CHUNK);
  add_property(tool,"length",s);
  require_field(tool,"path");
  cJSON_AddItemToArray(tools,tool);

  tool=new_tool("write_blob","Write a binary-safe file chunk supplied as base64.");
  add_property(tool,"path",schema_string());
  s=schema_integer(0,1);
  cJSON_AddNumberToObject(s,"minimum",0);
  add_property(tool,"offset",s);
  add_property(tool,"data_base64",schema_string());
  add_property(tool,"truncate",schema_boolean(0,1));
  require_field(tool,"path");
  require_field(tool,"data_base64");
  cJSON_AddItemToArray(tools,tool);

  tool=new_tool("run","Run a shell command as the mcp user and return exit code, stdout and stderr.");
  add_property(tool,"command",schema_string());
  s=schema_string();
  cJSON_AddStringToObject(s,"default",".");
  add_property(tool,"cwd",s);
  require_field(tool,"command");
  cJSON_AddItemToArray(tools,tool);

  tool=new_tool("start","Start a command asynchronously and return its job_id and PID.");
  add_property(tool,"command",schema_string());
  s=schema_string();
  cJSON_AddStringToObject(s,"default",".");
  add_property(tool,"cwd",s);
  require_field(tool,"command");
  cJSON_AddItemToArray(tools,tool);

  tool=new_tool("status","Return state, PID, elapsed time, exit code, CPU and RSS for a job.");
  add_property(tool,"job_id",schema_string());
  require_field(tool,"job_id");
  cJSON_AddItemToArray(tools,tool);

  tool=new_tool("tail","Return the last lines of stdout, stderr or both for a job.");
  add_property(tool,"job_id",schema_string());
  s=schema_integer(50,1);
  cJSON_AddNumberToObject(s,"minimum",1);
  cJSON_AddNumberToObject(s,"maximum",MAX_TAIL_LINES);
  add_property(tool,"lines",s);
  s=schema_string();
  cJSON_AddStringToObject(s,"default","stdout");
  e=cJSON_AddArrayToObject(s,"enum");
  cJSON_AddItemToArray(e,cJSON_CreateString("stdout"));
  cJSON_AddItemToArray(e,cJSON_CreateString("stderr"));
  cJSON_AddItemToArray(e,cJSON_CreateString("both"));
  add_property(tool,"stream",s);
  require_field(tool,"job_id");
  cJSON_AddItemToArray(tools,tool);

  tool=new_tool("stop","Stop a job with SIGTERM, or SIGKILL when force is true.");
  add_property(tool,"job_id",schema_string());
  add_property(tool,"force",schema_boolean(0,1));
  require_field(tool,"job_id");
  cJSON_AddItemToArray(tools,tool);

  tool=new_tool("jobs","List known jobs, newest first, with their current state.");
  s=schema_integer(100,1);
  cJSON_AddNumberToObject(s,"minimum",1);
  cJSON_AddNumberToObject(s,"maximum",1000);
  add_property(tool,"limit",s);
  cJSON_AddItemToArray(tools,tool);

  tool=new_tool("agent_call","Send a typed request to an authorized remote agent and wait for its result.");
  add_property(tool,"agent",schema_string());
  add_property(tool,"module",schema_string());
  add_property(tool,"action",schema_string());
  add_property(tool,"payload",cJSON_CreateObject());
  s=schema_integer(AGENT_CALL_TIMEOUT,1);
  cJSON_AddNumberToObject(s,"minimum",1);
  cJSON_AddNumberToObject(s,"maximum",AGENT_CALL_TIMEOUT_MAX);
  add_property(tool,"timeout",s);
  require_field(tool,"module");
  require_field(tool,"action");
  cJSON_AddItemToArray(tools,tool);

  return tools;
}

static cJSON *handle_discover(void) {
  cJSON *result,*versions,*caps,*tools;

  result=cJSON_CreateObject();
  cJSON_AddStringToObject(result,"cacheScope","private");
  caps=cJSON_AddObjectToObject(result,"capabilities");
  tools=cJSON_AddObjectToObject(caps,"tools");
  cJSON_AddBoolToObject(tools,"listChanged",0);
  cJSON_AddStringToObject(result,"instructions","mymcp provides file, shell and asynchronous job tools inside /home/tools/mcp/work. Every tool call requires a chat name (1-64 characters: A-Z, a-z, 0-9, _, -, .). If the chat name is not already known, ask the user to choose it before the first tool call and reuse the same value for that conversation. For long-running work, use start() to launch the process and use status(), tail() or read_file() for observations. A synchronous run() containing sleep, delayed wait-then-observe commands, or polling loops is technically possible, but this pattern can consume Work/Codex usage heavily. Never use that delayed-wait strategy unless the owner has explicitly authorized it for the current task. Without explicit owner authorization, do not use sleep-based or delayed polling calls.");
  cJSON_AddStringToObject(result,"resultType","complete");
  versions=cJSON_AddArrayToObject(result,"supportedVersions");
  cJSON_AddItemToArray(versions,cJSON_CreateString(PROTOCOL_VERSION));
  cJSON_AddNumberToObject(result,"ttlMs",0);
  add_server_meta(result);
  return result;
}

static cJSON *handle_tools_list(void) {
  cJSON *result;

  result=cJSON_CreateObject();
  cJSON_AddStringToObject(result,"cacheScope","private");
  cJSON_AddStringToObject(result,"resultType","complete");
  cJSON_AddItemToObject(result,"tools",build_tools());
  cJSON_AddNumberToObject(result,"ttlMs",0);
  add_server_meta(result);
  return result;
}

static int get_string_arg(cJSON *args,const char *name,const char **value,int required) {
  cJSON *item;

  item=cJSON_GetObjectItemCaseSensitive(args,name);
  if(item==NULL) return required?-1:0;
  if(!cJSON_IsString(item) || item->valuestring==NULL) return -1;
  *value=item->valuestring;
  return 1;
}

static int get_int_arg(cJSON *args,const char *name,int *value,int default_value) {
  cJSON *item;

  item=cJSON_GetObjectItemCaseSensitive(args,name);
  if(item==NULL) {
    *value=default_value;
    return 0;
  }
  if(!cJSON_IsNumber(item)) return -1;
  *value=item->valueint;
  return 1;
}

static int get_long_arg(cJSON *args,const char *name,long *value,long default_value) {
  cJSON *item;
  double number;

  item=cJSON_GetObjectItemCaseSensitive(args,name);
  if(item==NULL) {
    *value=default_value;
    return 0;
  }
  if(!cJSON_IsNumber(item)) return -1;
  number=item->valuedouble;
  if(number<(double)LONG_MIN || number>(double)LONG_MAX || number!=(double)(long)number) return -1;
  *value=(long)number;
  return 1;
}

static int get_bool_arg(cJSON *args,const char *name,int *value,int default_value) {
  cJSON *item;

  item=cJSON_GetObjectItemCaseSensitive(args,name);
  if(item==NULL) {
    *value=default_value;
    return 0;
  }
  if(!cJSON_IsBool(item)) return -1;
  *value=cJSON_IsTrue(item)?1:0;
  return 1;
}

static int tool_result_is_error(cJSON *result) {
  cJSON *item;

  if(!cJSON_IsObject(result)) return 1;
  item=cJSON_GetObjectItemCaseSensitive(result,"isError");
  if(!cJSON_IsBool(item)) return 1;
  return cJSON_IsTrue(item)?1:0;
}

static int tool_run_exit_code(cJSON *result,int *exit_code) {
  cJSON *structured,*item;
  char *end;
  long value;

  if(!cJSON_IsObject(result)) return 0;
  structured=cJSON_GetObjectItemCaseSensitive(result,"structuredContent");
  if(!cJSON_IsObject(structured)) return 0;
  item=cJSON_GetObjectItemCaseSensitive(structured,"result");
  if(!cJSON_IsString(item) || item->valuestring==NULL) return 0;
  if(strncmp(item->valuestring,"exit_code=",10)!=0) return 0;
  errno=0;
  value=strtol(item->valuestring+10,&end,10);
  if(errno!=0 || end==item->valuestring+10) return 0;
  *exit_code=(int)value;
  return 1;
}

static void build_tool_log_detail(const char *name,cJSON *args,cJSON *result,
  char *out,size_t out_size) {
  cJSON *item,*structured;
  const char *path,*command,*cwd,*job_id,*stream;
  char q1[LOG_VALUE_MAX+8],q2[LOG_VALUE_MAX+8];
  int lines,limit,force,exit_code,is_error,recursive,truncate,length;
  long offset;
  size_t bytes;

  if(out_size==0) return;
  out[0]=0;
  if(name==NULL || !cJSON_IsObject(args)) return;
  is_error=tool_result_is_error(result);
  path=NULL; command=NULL; cwd="."; job_id=NULL; stream="stdout";
  lines=50; limit=100; force=0; recursive=1; truncate=0; length=MAX_BLOB_CHUNK; offset=0; bytes=0;

  if(strcmp(name,"write_file")==0) {
    get_string_arg(args,"path",&path,0);
    item=cJSON_GetObjectItemCaseSensitive(args,"content");
    if(cJSON_IsString(item) && item->valuestring!=NULL) bytes=strlen(item->valuestring);
    log_quote_value(path,q1,sizeof(q1));
    snprintf(out,out_size,"path=%s bytes=%lu tool_error=%s",q1,(unsigned long)bytes,is_error?"true":"false");
  } else if(strcmp(name,"read_file")==0) {
    get_string_arg(args,"path",&path,0);
    log_quote_value(path,q1,sizeof(q1));
    snprintf(out,out_size,"path=%s tool_error=%s",q1,is_error?"true":"false");
  } else if(strcmp(name,"list_files")==0) {
    get_string_arg(args,"path",&path,0);
    get_bool_arg(args,"recursive",&recursive,1);
    log_quote_value(path,q1,sizeof(q1));
    snprintf(out,out_size,"path=%s recursive=%s tool_error=%s",q1,recursive?"true":"false",is_error?"true":"false");
  } else if(strcmp(name,"read_blob")==0) {
    get_string_arg(args,"path",&path,0);
    get_long_arg(args,"offset",&offset,0);
    get_int_arg(args,"length",&length,MAX_BLOB_CHUNK);
    log_quote_value(path,q1,sizeof(q1));
    snprintf(out,out_size,"path=%s offset=%ld length=%d tool_error=%s",q1,offset,length,is_error?"true":"false");
  } else if(strcmp(name,"write_blob")==0) {
    get_string_arg(args,"path",&path,0);
    get_long_arg(args,"offset",&offset,0);
    get_bool_arg(args,"truncate",&truncate,0);
    item=cJSON_GetObjectItemCaseSensitive(args,"data_base64");
    if(cJSON_IsString(item) && item->valuestring!=NULL) bytes=strlen(item->valuestring);
    log_quote_value(path,q1,sizeof(q1));
    snprintf(out,out_size,"path=%s offset=%ld base64_chars=%lu truncate=%s tool_error=%s",q1,offset,(unsigned long)bytes,truncate?"true":"false",is_error?"true":"false");
  } else if(strcmp(name,"run")==0) {
    get_string_arg(args,"command",&command,0);
    get_string_arg(args,"cwd",&cwd,0);
    log_quote_value(cwd,q1,sizeof(q1));
    log_quote_value(command,q2,sizeof(q2));
    if(tool_run_exit_code(result,&exit_code))
      snprintf(out,out_size,"cwd=%s command=%s exit_code=%d tool_error=%s",q1,q2,exit_code,is_error?"true":"false");
    else
      snprintf(out,out_size,"cwd=%s command=%s tool_error=%s",q1,q2,is_error?"true":"false");
  } else if(strcmp(name,"start")==0) {
    get_string_arg(args,"command",&command,0);
    get_string_arg(args,"cwd",&cwd,0);
    log_quote_value(cwd,q1,sizeof(q1));
    log_quote_value(command,q2,sizeof(q2));
    structured=cJSON_GetObjectItemCaseSensitive(result,"structuredContent");
    item=cJSON_IsObject(structured)?cJSON_GetObjectItemCaseSensitive(structured,"job_id"):NULL;
    job_id=cJSON_IsString(item)?item->valuestring:NULL;
    if(job_id!=NULL)
      snprintf(out,out_size,"cwd=%s command=%s job_id=%s tool_error=%s",q1,q2,job_id,is_error?"true":"false");
    else
      snprintf(out,out_size,"cwd=%s command=%s tool_error=%s",q1,q2,is_error?"true":"false");
  } else if(strcmp(name,"status")==0) {
    get_string_arg(args,"job_id",&job_id,0);
    snprintf(out,out_size,"job_id=%s tool_error=%s",job_id!=NULL?job_id:"-",is_error?"true":"false");
  } else if(strcmp(name,"tail")==0) {
    get_string_arg(args,"job_id",&job_id,0);
    get_int_arg(args,"lines",&lines,50);
    get_string_arg(args,"stream",&stream,0);
    snprintf(out,out_size,"job_id=%s stream=%s lines=%d tool_error=%s",
      job_id!=NULL?job_id:"-",stream!=NULL?stream:"stdout",lines,is_error?"true":"false");
  } else if(strcmp(name,"stop")==0) {
    get_string_arg(args,"job_id",&job_id,0);
    get_bool_arg(args,"force",&force,0);
    snprintf(out,out_size,"job_id=%s force=%s tool_error=%s",
      job_id!=NULL?job_id:"-",force?"true":"false",is_error?"true":"false");
  } else if(strcmp(name,"jobs")==0) {
    get_int_arg(args,"limit",&limit,100);
    snprintf(out,out_size,"limit=%d tool_error=%s",limit,is_error?"true":"false");
  } else if(strcmp(name,"hello")==0) {
    snprintf(out,out_size,"tool_error=%s",is_error?"true":"false");
  }
}

static cJSON *tool_hello(cJSON *args) {
  (void)args;
  return tool_result_string("hello from mymcp",0);
}

static cJSON *tool_write_file(cJSON *args) {
  const char *path,*content;
  char full[PATH_MAX],result[PATH_MAX+128];
  FILE *f;
  size_t len,written;
  int n;

  path=NULL;
  content=NULL;
  if(get_string_arg(args,"path",&path,1)<0 || get_string_arg(args,"content",&content,1)<0)
    return tool_result_string("invalid arguments: path and content are required strings",1);
  if(ensure_write_path(path,full,sizeof(full))!=0) return tool_result_string("path outside work directory or invalid path",1);
  f=fopen(full,"wb");
  if(f==NULL) return tool_result_string(strerror(errno),1);
  len=strlen(content);
  written=fwrite(content,1,len,f);
  if(fclose(f)!=0 || written!=len) return tool_result_string("failed to write complete file",1);
  n=snprintf(result,sizeof(result),"written %lu bytes to %s",(unsigned long)len,full);
  if(n<0 || (size_t)n>=sizeof(result)) return tool_result_string("result formatting failed",1);
  return tool_result_string(result,0);
}

static cJSON *tool_read_file(cJSON *args) {
  const char *path;
  char full[PATH_MAX],*text;

  path=NULL;
  if(get_string_arg(args,"path",&path,1)<0) return tool_result_string("invalid argument: path is required",1);
  if(safe_existing_path(path,full,sizeof(full),0)!=0) return tool_result_string("file not found or path outside work directory",1);
  text=read_file_alloc(full,MAX_BODY,NULL);
  if(text==NULL) return tool_result_string(strerror(errno),1);
  {
    cJSON *result;
    result=tool_result_string(text,0);
    free(text);
    return result;
  }
}

static cJSON *tool_list_files(cJSON *args) {
  const char *path;
  char full[PATH_MAX],rel_root[PATH_MAX];
  cJSON *data;
  size_t len;
  int recursive,count,rc;

  path=NULL;
  recursive=1;
  if(get_string_arg(args,"path",&path,1)<0) return tool_result_string("invalid argument: path is required",1);
  if(get_bool_arg(args,"recursive",&recursive,1)<0) return tool_result_string("invalid argument: recursive must be boolean",1);
  if(safe_existing_path(path,full,sizeof(full),1)!=0) return tool_result_string("directory not found or path outside work directory",1);
  if(strlen(path)>=sizeof(rel_root)) return tool_result_string("path too long",1);
  strcpy(rel_root,path);
  len=strlen(rel_root);
  for(;len>1 && rel_root[len-1]=='/';len--) rel_root[len-1]=0;
  if(strcmp(rel_root,".")==0) rel_root[0]=0;
  data=cJSON_CreateArray();
  if(data==NULL) return tool_result_string("out of memory",1);
  count=0;
  rc=list_entries(data,full,rel_root,recursive,&count);
  if(rc!=0) {
    cJSON_Delete(data);
    if(rc==-2) return tool_result_string("too many directory entries",1);
    return tool_result_string("cannot list directory",1);
  }
  return tool_result_json(data,0);
}

static cJSON *tool_read_blob(cJSON *args) {
  const char *path;
  char full[PATH_MAX],*encoded;
  unsigned char *buf;
  FILE *f;
  struct stat st;
  cJSON *data;
  long offset;
  size_t want,got;
  int length;

  path=NULL;
  offset=0;
  length=MAX_BLOB_CHUNK;
  if(get_string_arg(args,"path",&path,1)<0) return tool_result_string("invalid argument: path is required",1);
  if(get_long_arg(args,"offset",&offset,0)<0 || offset<0) return tool_result_string("invalid argument: offset must be a non-negative integer",1);
  if(get_int_arg(args,"length",&length,MAX_BLOB_CHUNK)<0 || length<1 || length>MAX_BLOB_CHUNK) return tool_result_string("invalid argument: length out of range",1);
  if(safe_existing_path(path,full,sizeof(full),0)!=0) return tool_result_string("file not found or path outside work directory",1);
  if(stat(full,&st)!=0) return tool_result_string(strerror(errno),1);
  if(st.st_size>(off_t)LONG_MAX || offset>(long)st.st_size) return tool_result_string("offset beyond end of file",1);
  want=(size_t)((long)st.st_size-offset);
  if(want>(size_t)length) want=(size_t)length;
  buf=(unsigned char *)malloc(want>0?want:1);
  if(buf==NULL) return tool_result_string("out of memory",1);
  f=fopen(full,"rb");
  if(f==NULL) {
    free(buf);
    return tool_result_string(strerror(errno),1);
  }
  if(fseek(f,offset,SEEK_SET)!=0) {
    fclose(f);
    free(buf);
    return tool_result_string("cannot seek file",1);
  }
  got=fread(buf,1,want,f);
  if(ferror(f)) {
    fclose(f);
    free(buf);
    return tool_result_string("cannot read file",1);
  }
  fclose(f);
  encoded=base64_encode(buf,got);
  free(buf);
  if(encoded==NULL) return tool_result_string("out of memory",1);
  data=cJSON_CreateObject();
  if(data==NULL) {
    free(encoded);
    return tool_result_string("out of memory",1);
  }
  cJSON_AddStringToObject(data,"path",path);
  cJSON_AddNumberToObject(data,"size",(double)st.st_size);
  cJSON_AddNumberToObject(data,"offset",(double)offset);
  cJSON_AddNumberToObject(data,"length",(double)got);
  cJSON_AddStringToObject(data,"data_base64",encoded);
  cJSON_AddBoolToObject(data,"eof",offset+(long)got>=(long)st.st_size);
  free(encoded);
  return tool_result_json(data,0);
}

static cJSON *tool_write_blob(cJSON *args) {
  const char *path,*text;
  char full[PATH_MAX];
  unsigned char *buf;
  FILE *f;
  struct stat st;
  cJSON *data;
  long offset;
  size_t len,written;
  int truncate,exists;

  path=NULL;
  text=NULL;
  offset=0;
  truncate=0;
  if(get_string_arg(args,"path",&path,1)<0 || get_string_arg(args,"data_base64",&text,1)<0) return tool_result_string("invalid arguments: path and data_base64 are required strings",1);
  if(get_long_arg(args,"offset",&offset,0)<0 || offset<0) return tool_result_string("invalid argument: offset must be a non-negative integer",1);
  if(get_bool_arg(args,"truncate",&truncate,0)<0) return tool_result_string("invalid argument: truncate must be boolean",1);
  if(truncate && offset!=0) return tool_result_string("truncate requires offset 0",1);
  buf=base64_decode(text,&len);
  if(buf==NULL) return tool_result_string("invalid base64 data",1);
  if(len>MAX_BLOB_CHUNK) {
    free(buf);
    return tool_result_string("blob chunk too large",1);
  }
  if(ensure_write_path(path,full,sizeof(full))!=0) {
    free(buf);
    return tool_result_string("path outside work directory or invalid path",1);
  }
  exists=lstat(full,&st)==0;
  if(exists && !S_ISREG(st.st_mode)) {
    free(buf);
    return tool_result_string("target is not a regular file",1);
  }
  if(!truncate) {
    if(exists && offset>(long)st.st_size) {
      free(buf);
      return tool_result_string("offset beyond end of file",1);
    }
    if(!exists && offset!=0) {
      free(buf);
      return tool_result_string("offset beyond end of file",1);
    }
  }
  if(truncate) f=fopen(full,"wb");
  else if(exists) f=fopen(full,"r+b");
  else f=fopen(full,"wb");
  if(f==NULL) {
    free(buf);
    return tool_result_string(strerror(errno),1);
  }
  if(fseek(f,offset,SEEK_SET)!=0) {
    fclose(f);
    free(buf);
    return tool_result_string("cannot seek file",1);
  }
  written=fwrite(buf,1,len,f);
  free(buf);
  if(fclose(f)!=0 || written!=len) return tool_result_string("failed to write complete blob",1);
  if(stat(full,&st)!=0) return tool_result_string(strerror(errno),1);
  data=cJSON_CreateObject();
  if(data==NULL) return tool_result_string("out of memory",1);
  cJSON_AddStringToObject(data,"path",path);
  cJSON_AddNumberToObject(data,"offset",(double)offset);
  cJSON_AddNumberToObject(data,"length",(double)len);
  cJSON_AddNumberToObject(data,"size",(double)st.st_size);
  return tool_result_json(data,0);
}

static int append_output(char *dst,size_t *used,const char *src,size_t n,int *truncated) {
  size_t room,take;

  if(*used>=MAX_OUTPUT) {
    *truncated=1;
    return 0;
  }
  room=MAX_OUTPUT-*used;
  take=n<room?n:room;
  memcpy(dst+*used,src,take);
  *used+=take;
  if(take<n) *truncated=1;
  return 0;
}

static cJSON *tool_run(cJSON *args) {
  const char *command,*cwd_arg;
  char cwd[PATH_MAX],out[MAX_OUTPUT+32],err[MAX_OUTPUT+32],tmp[8192],result[MAX_OUTPUT*2+256];
  int out_pipe[2],err_pipe[2],flags,status,done,timed_out,out_open,err_open;
  int out_truncated,err_truncated,maxfd,sel,wait_rc;
  pid_t pid,w;
  ssize_t n;
  size_t out_used,err_used;
  double start,elapsed,remain;
  fd_set rfds;
  struct timeval tv;

  command=NULL;
  cwd_arg=".";
  if(get_string_arg(args,"command",&command,1)<0) return tool_result_string("invalid argument: command is required",1);
  if(get_string_arg(args,"cwd",&cwd_arg,0)<0) return tool_result_string("invalid argument: cwd must be a string",1);
  if(command[0]==0) return tool_result_string("command is empty",1);
  if(safe_existing_path(cwd_arg,cwd,sizeof(cwd),1)!=0) return tool_result_string("cwd is not a directory inside work",1);
  if(pipe(out_pipe)!=0 || pipe(err_pipe)!=0) return tool_result_string(strerror(errno),1);
  pid=fork();
  if(pid<0) {
    close(out_pipe[0]); close(out_pipe[1]); close(err_pipe[0]); close(err_pipe[1]);
    return tool_result_string(strerror(errno),1);
  }
  if(pid==0) {
    setpgid(0,0);
    chdir(cwd);
    close(out_pipe[0]); close(err_pipe[0]);
    dup2(out_pipe[1],STDOUT_FILENO);
    dup2(err_pipe[1],STDERR_FILENO);
    close(out_pipe[1]); close(err_pipe[1]);
    execl("/bin/sh","sh","-c",command,(char *)NULL);
    _exit(127);
  }
  setpgid(pid,pid);
  close(out_pipe[1]); close(err_pipe[1]);
  flags=fcntl(out_pipe[0],F_GETFL,0); fcntl(out_pipe[0],F_SETFL,flags|O_NONBLOCK);
  flags=fcntl(err_pipe[0],F_GETFL,0); fcntl(err_pipe[0],F_SETFL,flags|O_NONBLOCK);
  out_used=0; err_used=0; out_truncated=0; err_truncated=0;
  done=0; timed_out=0; out_open=1; err_open=1; status=0;
  start=now_seconds();
  for(;!done || out_open || err_open;) {
    elapsed=now_seconds()-start;
    if(!done && elapsed>=RUN_TIMEOUT) {
      kill(-pid,SIGKILL);
      timed_out=1;
    }
    FD_ZERO(&rfds);
    maxfd=-1;
    if(out_open) { FD_SET(out_pipe[0],&rfds); if(out_pipe[0]>maxfd) maxfd=out_pipe[0]; }
    if(err_open) { FD_SET(err_pipe[0],&rfds); if(err_pipe[0]>maxfd) maxfd=err_pipe[0]; }
    remain=0.05;
    tv.tv_sec=(long)remain;
    tv.tv_usec=(long)((remain-(double)tv.tv_sec)*1000000.0);
    sel=maxfd>=0?select(maxfd+1,&rfds,NULL,NULL,&tv):0;
    if(sel>0 && out_open && FD_ISSET(out_pipe[0],&rfds)) {
      for(;;) {
        n=read(out_pipe[0],tmp,sizeof(tmp));
        if(n>0) append_output(out,&out_used,tmp,(size_t)n,&out_truncated);
        else { if(n==0) { close(out_pipe[0]); out_open=0; } break; }
      }
    }
    if(sel>0 && err_open && FD_ISSET(err_pipe[0],&rfds)) {
      for(;;) {
        n=read(err_pipe[0],tmp,sizeof(tmp));
        if(n>0) append_output(err,&err_used,tmp,(size_t)n,&err_truncated);
        else { if(n==0) { close(err_pipe[0]); err_open=0; } break; }
      }
    }
    if(!done) {
      w=waitpid(pid,&status,WNOHANG);
      if(w==pid) done=1;
    }
    if(timed_out && !done) {
      wait_rc=waitpid(pid,&status,WNOHANG);
      if(wait_rc==pid) done=1;
    }
  }
  if(!done) waitpid(pid,&status,0);
  out[out_used]=0; err[err_used]=0;
  if(out_truncated && out_used+20<sizeof(out)) strcat(out,"\n[stdout truncated]");
  if(err_truncated && err_used+20<sizeof(err)) strcat(err,"\n[stderr truncated]");
  if(timed_out) snprintf(result,sizeof(result),"timeout after %d seconds\nstdout=%s\nstderr=%s",RUN_TIMEOUT,out,err);
  else if(WIFEXITED(status)) snprintf(result,sizeof(result),"exit_code=%d\n--- stdout ---\n%s\n--- stderr ---\n%s",WEXITSTATUS(status),out,err);
  else if(WIFSIGNALED(status)) snprintf(result,sizeof(result),"exit_code=%d\n--- stdout ---\n%s\n--- stderr ---\n%s",-WTERMSIG(status),out,err);
  else snprintf(result,sizeof(result),"exit_code=-1\n--- stdout ---\n%s\n--- stderr ---\n%s",out,err);
  return tool_result_string(result,0);
}

static int make_job_id(char *out,size_t out_size) {
  unsigned char raw[6];
  int fd,n,i;

  if(out_size<JOB_ID_LEN+1) return -1;
  fd=open("/dev/urandom",O_RDONLY);
  if(fd<0) return -1;
  n=(int)read(fd,raw,sizeof(raw));
  close(fd);
  if(n!=(int)sizeof(raw)) return -1;
  strcpy(out,"job_");
  for(i=0;i<6;i++) sprintf(out+4+i*2,"%02x",raw[i]);
  out[JOB_ID_LEN]=0;
  return 0;
}

static int valid_job_id(const char *job_id) {
  int i;

  if(job_id==NULL || strlen(job_id)!=JOB_ID_LEN) return 0;
  if(strncmp(job_id,"job_",4)!=0) return 0;
  for(i=4;i<JOB_ID_LEN;i++) if(!isxdigit((unsigned char)job_id[i])) return 0;
  return 1;
}

static int job_dir_path(const char *job_id,char *out,size_t out_size) {
  int n;

  if(!valid_job_id(job_id)) return -1;
  n=snprintf(out,out_size,"%s/%s",JOBS_DIR,job_id);
  if(n<0 || (size_t)n>=out_size) return -1;
  return 0;
}

static int read_proc_info(pid_t pid,ProcInfo *info) {
  char path[64],buf[4096],*rparen,*save,*tok;
  FILE *f;
  int field;

  snprintf(path,sizeof(path),"/proc/%ld/stat",(long)pid);
  f=fopen(path,"r");
  if(f==NULL) return -1;
  if(fgets(buf,sizeof(buf),f)==NULL) { fclose(f); return -1; }
  fclose(f);
  rparen=strrchr(buf,')');
  if(rparen==NULL || rparen[1]!=' ') return -1;
  info->pid=pid; info->pgrp=0; info->utime=0; info->stime=0; info->starttime=0; info->rss_pages=0;
  save=NULL;
  tok=strtok_r(rparen+2," ",&save);
  field=3;
  for(;tok!=NULL;tok=strtok_r(NULL," ",&save),field++) {
    if(field==5) info->pgrp=(pid_t)strtol(tok,NULL,10);
    else if(field==14) info->utime=strtoull(tok,NULL,10);
    else if(field==15) info->stime=strtoull(tok,NULL,10);
    else if(field==22) info->starttime=strtoull(tok,NULL,10);
    else if(field==24) { info->rss_pages=strtol(tok,NULL,10); break; }
  }
  if(info->starttime==0) return -1;
  return 0;
}

static unsigned long long proc_start_time(pid_t pid) {
  ProcInfo info;

  if(read_proc_info(pid,&info)!=0) return 0;
  return info.starttime;
}

static double system_uptime(void) {
  FILE *f;
  double uptime;

  f=fopen("/proc/uptime","r");
  if(f==NULL) return 0.0;
  uptime=0.0;
  fscanf(f,"%lf",&uptime);
  fclose(f);
  return uptime;
}

static void free_group_stats(GroupStats *stats) {
  if(stats->pids!=NULL) free(stats->pids);
  memset(stats,0,sizeof(*stats));
}

static int add_group_pid(GroupStats *stats,pid_t pid) {
  pid_t *p;
  int new_capacity;

  if(stats->count>=stats->capacity) {
    new_capacity=stats->capacity==0?8:stats->capacity*2;
    p=(pid_t *)realloc(stats->pids,(size_t)new_capacity*sizeof(pid_t));
    if(p==NULL) return -1;
    stats->pids=p;
    stats->capacity=new_capacity;
  }
  stats->pids[stats->count++]=pid;
  return 0;
}

static int group_stats(pid_t pgid,GroupStats *stats) {
  DIR *d;
  struct dirent *de;
  ProcInfo info;
  long hz,page_size;
  double uptime,elapsed,cpu;
  pid_t pid;

  memset(stats,0,sizeof(*stats));
  hz=sysconf(_SC_CLK_TCK);
  page_size=sysconf(_SC_PAGESIZE);
  uptime=system_uptime();
  d=opendir("/proc");
  if(d==NULL) return -1;
  for(; (de=readdir(d))!=NULL;) {
    if(!isdigit((unsigned char)de->d_name[0])) continue;
    pid=(pid_t)strtol(de->d_name,NULL,10);
    if(read_proc_info(pid,&info)!=0 || info.pgrp!=pgid) continue;
    if(add_group_pid(stats,pid)!=0) { closedir(d); free_group_stats(stats); return -1; }
    if(info.rss_pages>0) stats->rss_bytes+=(unsigned long long)info.rss_pages*(unsigned long long)page_size;
    elapsed=uptime-(double)info.starttime/(double)hz;
    if(elapsed>0.0) {
      cpu=((double)(info.utime+info.stime)/(double)hz)/elapsed*100.0;
      stats->cpu_percent+=cpu;
    }
  }
  closedir(d);
  return 0;
}

static int save_job_meta(JobMeta *meta) {
  char dir[PATH_MAX],path[PATH_MAX],tmp[PATH_MAX],*text;
  cJSON *root;
  int n,rc;

  if(job_dir_path(meta->job_id,dir,sizeof(dir))!=0) return -1;
  n=snprintf(path,sizeof(path),"%s/meta.json",dir);
  if(n<0 || (size_t)n>=sizeof(path)) return -1;
  n=snprintf(tmp,sizeof(tmp),"%s/meta.json.new",dir);
  if(n<0 || (size_t)n>=sizeof(tmp)) return -1;
  root=cJSON_CreateObject();
  cJSON_AddStringToObject(root,"job_id",meta->job_id);
  cJSON_AddStringToObject(root,"chat",meta->chat);
  cJSON_AddNumberToObject(root,"pid",(double)meta->pid);
  cJSON_AddNumberToObject(root,"pgid",(double)meta->pgid);
  cJSON_AddNumberToObject(root,"pid_start_time",(double)meta->pid_start_time);
  cJSON_AddStringToObject(root,"command",meta->command!=NULL?meta->command:"");
  cJSON_AddStringToObject(root,"cwd",meta->cwd);
  cJSON_AddStringToObject(root,"started_at",meta->started_at);
  cJSON_AddNumberToObject(root,"started_epoch",meta->started_epoch);
  cJSON_AddStringToObject(root,"stdout",meta->stdout_path);
  cJSON_AddStringToObject(root,"stderr",meta->stderr_path);
  if(meta->stop_signal>0) cJSON_AddNumberToObject(root,"stop_signal",meta->stop_signal);
  text=cJSON_PrintUnformatted(root);
  cJSON_Delete(root);
  if(text==NULL) return -1;
  rc=write_text_file(tmp,text);
  free(text);
  if(rc!=0) return -1;
  if(rename(tmp,path)!=0) return -1;
  return 0;
}

static void free_job_meta(JobMeta *meta) {
  if(meta->command!=NULL) free(meta->command);
  meta->command=NULL;
}

static int copy_json_string(cJSON *root,const char *name,char *out,size_t out_size) {
  cJSON *item;

  item=cJSON_GetObjectItemCaseSensitive(root,name);
  if(!cJSON_IsString(item) || item->valuestring==NULL || strlen(item->valuestring)+1>out_size) return -1;
  strcpy(out,item->valuestring);
  return 0;
}

static int load_job_meta(const char *job_id,JobMeta *meta) {
  char dir[PATH_MAX],path[PATH_MAX],*text;
  cJSON *root,*item;
  int n;
  size_t command_len;

  memset(meta,0,sizeof(*meta));
  if(job_dir_path(job_id,dir,sizeof(dir))!=0) return -1;
  n=snprintf(path,sizeof(path),"%s/meta.json",dir);
  if(n<0 || (size_t)n>=sizeof(path)) return -1;
  text=read_file_alloc(path,MAX_BODY,NULL);
  if(text==NULL) return -1;
  root=cJSON_Parse(text);
  free(text);
  if(root==NULL) return -1;
  if(copy_json_string(root,"job_id",meta->job_id,sizeof(meta->job_id))!=0 || strcmp(meta->job_id,job_id)!=0) { cJSON_Delete(root); return -1; }
  if(copy_json_string(root,"chat",meta->chat,sizeof(meta->chat))!=0) strcpy(meta->chat,"legacy");
  if(copy_json_string(root,"cwd",meta->cwd,sizeof(meta->cwd))!=0 || copy_json_string(root,"started_at",meta->started_at,sizeof(meta->started_at))!=0 || copy_json_string(root,"stdout",meta->stdout_path,sizeof(meta->stdout_path))!=0 || copy_json_string(root,"stderr",meta->stderr_path,sizeof(meta->stderr_path))!=0) { cJSON_Delete(root); return -1; }
  item=cJSON_GetObjectItemCaseSensitive(root,"command");
  if(!cJSON_IsString(item) || item->valuestring==NULL) { cJSON_Delete(root); return -1; }
  command_len=strlen(item->valuestring);
  meta->command=(char *)malloc(command_len+1);
  if(meta->command==NULL) { cJSON_Delete(root); return -1; }
  strcpy(meta->command,item->valuestring);
  item=cJSON_GetObjectItemCaseSensitive(root,"pid"); if(!cJSON_IsNumber(item)) { cJSON_Delete(root); free_job_meta(meta); return -1; } meta->pid=(pid_t)item->valuedouble;
  item=cJSON_GetObjectItemCaseSensitive(root,"pgid"); if(!cJSON_IsNumber(item)) { cJSON_Delete(root); free_job_meta(meta); return -1; } meta->pgid=(pid_t)item->valuedouble;
  item=cJSON_GetObjectItemCaseSensitive(root,"pid_start_time"); if(!cJSON_IsNumber(item)) { cJSON_Delete(root); free_job_meta(meta); return -1; } meta->pid_start_time=(unsigned long long)item->valuedouble;
  item=cJSON_GetObjectItemCaseSensitive(root,"started_epoch"); if(!cJSON_IsNumber(item)) { cJSON_Delete(root); free_job_meta(meta); return -1; } meta->started_epoch=item->valuedouble;
  item=cJSON_GetObjectItemCaseSensitive(root,"stop_signal"); if(cJSON_IsNumber(item)) meta->stop_signal=item->valueint;
  cJSON_Delete(root);
  return 0;
}

static int read_exit_code(const char *job_id,int *code) {
  char dir[PATH_MAX],path[PATH_MAX];
  FILE *f;
  int n,value;

  if(job_dir_path(job_id,dir,sizeof(dir))!=0) return -1;
  n=snprintf(path,sizeof(path),"%s/exit_code",dir);
  if(n<0 || (size_t)n>=sizeof(path)) return -1;
  f=fopen(path,"r");
  if(f==NULL) return -1;
  if(fscanf(f,"%d",&value)!=1) { fclose(f); return -1; }
  fclose(f);
  *code=value;
  return 0;
}

static cJSON *status_data(const char *job_id,const char *chat) {
  JobMeta meta;
  GroupStats stats;
  cJSON *data,*pids;
  int i,code,has_code,running;
  double elapsed;

  if(load_job_meta(job_id,&meta)!=0) return NULL;
  if(chat!=NULL && strcmp(meta.chat,chat)!=0) { free_job_meta(&meta); return NULL; }
  if(group_stats(meta.pgid,&stats)!=0) { free_job_meta(&meta); return NULL; }
  running=stats.count>0;
  has_code=read_exit_code(job_id,&code)==0;
  if(!running && !has_code && meta.stop_signal>0) { code=-meta.stop_signal; has_code=1; }
  elapsed=now_seconds()-meta.started_epoch;
  if(elapsed<0.0) elapsed=0.0;
  data=cJSON_CreateObject();
  cJSON_AddStringToObject(data,"job_id",meta.job_id);
  cJSON_AddStringToObject(data,"chat",meta.chat);
  cJSON_AddStringToObject(data,"state",running?"running":"exited");
  cJSON_AddNumberToObject(data,"pid",(double)meta.pid);
  cJSON_AddNumberToObject(data,"pgid",(double)meta.pgid);
  pids=cJSON_AddArrayToObject(data,"pids");
  for(i=0;i<stats.count;i++) cJSON_AddItemToArray(pids,cJSON_CreateNumber((double)stats.pids[i]));
  cJSON_AddNumberToObject(data,"elapsed_seconds",elapsed);
  if(running || !has_code) cJSON_AddNullToObject(data,"exit_code");
  else cJSON_AddNumberToObject(data,"exit_code",code);
  cJSON_AddNumberToObject(data,"cpu_percent",stats.cpu_percent);
  cJSON_AddNumberToObject(data,"rss_bytes",(double)stats.rss_bytes);
  cJSON_AddStringToObject(data,"command",meta.command);
  cJSON_AddStringToObject(data,"cwd",meta.cwd);
  cJSON_AddStringToObject(data,"started_at",meta.started_at);
  cJSON_AddStringToObject(data,"stdout",meta.stdout_path);
  cJSON_AddStringToObject(data,"stderr",meta.stderr_path);
  free_group_stats(&stats);
  free_job_meta(&meta);
  return data;
}

static int write_exit_code(const char *path,int code) {
  char buf[64];

  snprintf(buf,sizeof(buf),"%d\n",code);
  return write_text_file(path,buf);
}

static void job_supervisor(const char *command,const char *cwd,const char *stdout_path,const char *stderr_path,const char *exit_path) {
  int out_fd,err_fd,null_fd,status,code;
  pid_t child,w;

  setsid();
  out_fd=open(stdout_path,O_WRONLY|O_CREAT|O_TRUNC,0644);
  err_fd=open(stderr_path,O_WRONLY|O_CREAT|O_TRUNC,0644);
  null_fd=open("/dev/null",O_RDONLY);
  if(out_fd<0 || err_fd<0 || null_fd<0) _exit(126);
  child=fork();
  if(child<0) { write_exit_code(exit_path,126); _exit(126); }
  if(child==0) {
    chdir(cwd);
    dup2(null_fd,STDIN_FILENO);
    dup2(out_fd,STDOUT_FILENO);
    dup2(err_fd,STDERR_FILENO);
    close(null_fd); close(out_fd); close(err_fd);
    execl("/bin/sh","sh","-c",command,(char *)NULL);
    _exit(127);
  }
  close(null_fd); close(out_fd); close(err_fd);
  status=0;
  for(;;) {
    w=waitpid(child,&status,0);
    if(w==child) break;
    if(w<0 && errno!=EINTR) break;
  }
  if(WIFEXITED(status)) code=WEXITSTATUS(status);
  else if(WIFSIGNALED(status)) code=-WTERMSIG(status);
  else code=-1;
  write_exit_code(exit_path,code);
  _exit(code>=0 && code<=255?code:1);
}

static cJSON *tool_start(cJSON *args) {
  const char *chat,*command,*cwd_arg;
  char cwd[PATH_MAX],job_id[JOB_ID_LEN+1],dir[PATH_MAX],stdout_path[PATH_MAX];
  char stderr_path[PATH_MAX],exit_path[PATH_MAX];
  JobMeta meta;
  cJSON *data;
  pid_t pid;
  unsigned long long start_time;
  struct tm tmv;
  time_t now;
  int attempts,n;

  chat=NULL; command=NULL; cwd_arg=".";
  if(get_string_arg(args,"chat",&chat,1)<0 || !valid_chat(chat)) return tool_result_string("invalid chat",1);
  if(get_string_arg(args,"command",&command,1)<0) return tool_result_string("invalid argument: command is required",1);
  if(get_string_arg(args,"cwd",&cwd_arg,0)<0) return tool_result_string("invalid argument: cwd must be a string",1);
  if(command[0]==0) return tool_result_string("command is empty",1);
  if(safe_existing_path(cwd_arg,cwd,sizeof(cwd),1)!=0) return tool_result_string("cwd is not a directory inside work",1);
  if(mkdir(JOBS_DIR,0755)!=0 && errno!=EEXIST) return tool_result_string(strerror(errno),1);
  for(attempts=0;attempts<20;attempts++) {
    if(make_job_id(job_id,sizeof(job_id))!=0) return tool_result_string("cannot generate job id",1);
    if(job_dir_path(job_id,dir,sizeof(dir))!=0) return tool_result_string("cannot build job path",1);
    if(mkdir(dir,0755)==0) break;
    if(errno!=EEXIST) return tool_result_string(strerror(errno),1);
  }
  if(attempts==20) return tool_result_string("cannot allocate unique job id",1);
  n=snprintf(stdout_path,sizeof(stdout_path),"%s/stdout.log",dir); if(n<0 || (size_t)n>=sizeof(stdout_path)) return tool_result_string("path too long",1);
  n=snprintf(stderr_path,sizeof(stderr_path),"%s/stderr.log",dir); if(n<0 || (size_t)n>=sizeof(stderr_path)) return tool_result_string("path too long",1);
  n=snprintf(exit_path,sizeof(exit_path),"%s/exit_code",dir); if(n<0 || (size_t)n>=sizeof(exit_path)) return tool_result_string("path too long",1);
  pid=fork();
  if(pid<0) return tool_result_string(strerror(errno),1);
  if(pid==0) job_supervisor(command,cwd,stdout_path,stderr_path,exit_path);
  start_time=0;
  for(attempts=0;attempts<100 && start_time==0;attempts++) {
    start_time=proc_start_time(pid);
    if(start_time==0) usleep(1000);
  }
  if(start_time==0) { kill(pid,SIGKILL); return tool_result_string("cannot read started process metadata",1); }
  memset(&meta,0,sizeof(meta));
  strcpy(meta.job_id,job_id);
  strcpy(meta.chat,chat);
  meta.pid=pid; meta.pgid=pid; meta.pid_start_time=start_time; meta.started_epoch=now_seconds();
  strcpy(meta.cwd,cwd); strcpy(meta.stdout_path,stdout_path); strcpy(meta.stderr_path,stderr_path);
  meta.command=(char *)command;
  now=time(NULL);
  gmtime_r(&now,&tmv);
  strftime(meta.started_at,sizeof(meta.started_at),"%Y-%m-%dT%H:%M:%SZ",&tmv);
  if(save_job_meta(&meta)!=0) { kill(-pid,SIGKILL); return tool_result_string("cannot save job metadata",1); }
  data=cJSON_CreateObject();
  cJSON_AddStringToObject(data,"job_id",job_id);
  cJSON_AddStringToObject(data,"chat",chat);
  cJSON_AddNumberToObject(data,"pid",(double)pid);
  cJSON_AddStringToObject(data,"state","running");
  cJSON_AddStringToObject(data,"stdout",stdout_path);
  cJSON_AddStringToObject(data,"stderr",stderr_path);
  return tool_result_json(data,0);
}

static cJSON *tool_status(cJSON *args) {
  const char *chat,*job_id;
  cJSON *data;

  chat=NULL; job_id=NULL;
  if(get_string_arg(args,"chat",&chat,1)<0 || !valid_chat(chat)) return tool_result_string("invalid chat",1);
  if(get_string_arg(args,"job_id",&job_id,1)<0 || !valid_job_id(job_id)) return tool_result_string("invalid job_id",1);
  data=status_data(job_id,chat);
  if(data==NULL) return tool_result_string("unknown job_id or job belongs to another chat",1);
  return tool_result_json(data,0);
}

static char *tail_file(const char *path,int lines) {
  FILE *f;
  char block[4096],*out;
  long size,pos,start,read_pos;
  size_t chunk,i,out_len,got;
  int target,count,last_newline,found;

  f=fopen(path,"rb");
  if(f==NULL) return NULL;
  if(fseek(f,0,SEEK_END)!=0) { fclose(f); return NULL; }
  size=ftell(f);
  if(size<=0) { fclose(f); out=(char *)malloc(1); if(out!=NULL) out[0]=0; return out; }
  if(fseek(f,size-1,SEEK_SET)!=0) { fclose(f); return NULL; }
  last_newline=fgetc(f)=='\n';
  target=lines+(last_newline?1:0);
  count=0; pos=size; start=0; found=0;
  for(;pos>0 && !found;) {
    chunk=pos>(long)sizeof(block)?sizeof(block):(size_t)pos;
    read_pos=pos-(long)chunk;
    if(fseek(f,read_pos,SEEK_SET)!=0) break;
    got=fread(block,1,chunk,f);
    for(i=got;i>0;i--) {
      if(block[i-1]=='\n') {
        count++;
        if(count==target) { start=read_pos+(long)i; found=1; break; }
      }
    }
    pos=read_pos;
  }
  if(size-start>MAX_OUTPUT) start=size-MAX_OUTPUT;
  out_len=(size_t)(size-start);
  out=(char *)malloc(out_len+1);
  if(out==NULL) { fclose(f); return NULL; }
  if(fseek(f,start,SEEK_SET)!=0) { free(out); fclose(f); return NULL; }
  got=fread(out,1,out_len,f);
  fclose(f);
  out[got]=0;
  return out;
}

static cJSON *tool_tail(cJSON *args) {
  const char *chat,*job_id,*stream;
  JobMeta meta;
  cJSON *data;
  char *out,*err;
  int lines;

  chat=NULL; job_id=NULL; stream="stdout"; lines=50; out=NULL; err=NULL;
  if(get_string_arg(args,"chat",&chat,1)<0 || !valid_chat(chat)) return tool_result_string("invalid chat",1);
  if(get_string_arg(args,"job_id",&job_id,1)<0 || !valid_job_id(job_id)) return tool_result_string("invalid job_id",1);
  if(get_int_arg(args,"lines",&lines,50)<0 || lines<1) return tool_result_string("lines must be >= 1",1);
  if(lines>MAX_TAIL_LINES) lines=MAX_TAIL_LINES;
  if(get_string_arg(args,"stream",&stream,0)<0 || (strcmp(stream,"stdout")!=0 && strcmp(stream,"stderr")!=0 && strcmp(stream,"both")!=0)) return tool_result_string("stream must be stdout, stderr or both",1);
  if(load_job_meta(job_id,&meta)!=0) return tool_result_string("unknown job_id",1);
  if(strcmp(meta.chat,chat)!=0) { free_job_meta(&meta); return tool_result_string("job belongs to another chat",1); }
  data=cJSON_CreateObject();
  cJSON_AddStringToObject(data,"job_id",job_id);
  cJSON_AddStringToObject(data,"chat",chat);
  cJSON_AddNumberToObject(data,"lines",lines);
  cJSON_AddStringToObject(data,"stream",stream);
  if(strcmp(stream,"stdout")==0 || strcmp(stream,"both")==0) {
    out=tail_file(meta.stdout_path,lines);
    cJSON_AddStringToObject(data,"stdout",out!=NULL?out:"");
  }
  if(strcmp(stream,"stderr")==0 || strcmp(stream,"both")==0) {
    err=tail_file(meta.stderr_path,lines);
    cJSON_AddStringToObject(data,"stderr",err!=NULL?err:"");
  }
  if(out!=NULL) free(out);
  if(err!=NULL) free(err);
  free_job_meta(&meta);
  return tool_result_json(data,0);
}

static cJSON *tool_stop(cJSON *args) {
  const char *chat,*job_id;
  JobMeta meta;
  GroupStats stats;
  cJSON *data;
  unsigned long long current_start;
  int force,sig,code,has_code;

  chat=NULL; job_id=NULL; force=0;
  if(get_string_arg(args,"chat",&chat,1)<0 || !valid_chat(chat)) return tool_result_string("invalid chat",1);
  if(get_string_arg(args,"job_id",&job_id,1)<0 || !valid_job_id(job_id)) return tool_result_string("invalid job_id",1);
  if(get_bool_arg(args,"force",&force,0)<0) return tool_result_string("force must be boolean",1);
  if(load_job_meta(job_id,&meta)!=0) return tool_result_string("unknown job_id",1);
  if(strcmp(meta.chat,chat)!=0) { free_job_meta(&meta); return tool_result_string("job belongs to another chat",1); }
  if(group_stats(meta.pgid,&stats)!=0) { free_job_meta(&meta); return tool_result_string("cannot inspect job",1); }
  if(stats.count==0) {
    has_code=read_exit_code(job_id,&code)==0;
    data=cJSON_CreateObject();
    cJSON_AddStringToObject(data,"job_id",job_id);
    cJSON_AddStringToObject(data,"chat",chat);
    cJSON_AddStringToObject(data,"state","exited");
    if(has_code) cJSON_AddNumberToObject(data,"exit_code",code); else cJSON_AddNullToObject(data,"exit_code");
    cJSON_AddStringToObject(data,"message","job already exited");
    free_group_stats(&stats); free_job_meta(&meta);
    return tool_result_json(data,0);
  }
  current_start=proc_start_time(meta.pid);
  if(current_start==0 || current_start!=meta.pid_start_time) {
    free_group_stats(&stats); free_job_meta(&meta);
    return tool_result_string("job leader PID no longer matches; refusing to signal",1);
  }
  sig=force?SIGKILL:SIGTERM;
  if(kill(-meta.pgid,sig)!=0 && errno!=ESRCH) {
    free_group_stats(&stats); free_job_meta(&meta);
    return tool_result_string(strerror(errno),1);
  }
  meta.stop_signal=sig;
  save_job_meta(&meta);
  data=cJSON_CreateObject();
  cJSON_AddStringToObject(data,"job_id",job_id);
  cJSON_AddStringToObject(data,"chat",chat);
  cJSON_AddStringToObject(data,"state","stopping");
  cJSON_AddStringToObject(data,"signal",force?"SIGKILL":"SIGTERM");
  cJSON_AddNumberToObject(data,"pid",(double)meta.pid);
  cJSON_AddNumberToObject(data,"pgid",(double)meta.pgid);
  free_group_stats(&stats); free_job_meta(&meta);
  return tool_result_json(data,0);
}

static int compare_job_index(const void *a,const void *b) {
  const JobIndex *ja,*jb;

  ja=(const JobIndex *)a; jb=(const JobIndex *)b;
  if(ja->started_epoch<jb->started_epoch) return 1;
  if(ja->started_epoch>jb->started_epoch) return -1;
  return 0;
}

static cJSON *tool_jobs(cJSON *args) {
  const char *chat;
  DIR *d;
  struct dirent *de;
  JobIndex *index,*new_index;
  JobMeta meta;
  cJSON *array,*status,*summary,*item;
  int limit,count,capacity,i;

  chat=NULL; limit=100;
  if(get_string_arg(args,"chat",&chat,1)<0 || !valid_chat(chat)) return tool_result_string("invalid chat",1);
  if(get_int_arg(args,"limit",&limit,100)<0 || limit<1) return tool_result_string("limit must be >= 1",1);
  if(limit>1000) limit=1000;
  d=opendir(JOBS_DIR);
  array=cJSON_CreateArray();
  if(d==NULL) return tool_result_json(array,0);
  index=NULL; count=0; capacity=0;
  for(; (de=readdir(d))!=NULL;) {
    if(!valid_job_id(de->d_name)) continue;
    if(load_job_meta(de->d_name,&meta)!=0) continue;
    if(strcmp(meta.chat,chat)!=0) { free_job_meta(&meta); continue; }
    if(count>=capacity) {
      capacity=capacity==0?16:capacity*2;
      new_index=(JobIndex *)realloc(index,(size_t)capacity*sizeof(JobIndex));
      if(new_index==NULL) { free_job_meta(&meta); break; }
      index=new_index;
    }
    strcpy(index[count].job_id,de->d_name);
    index[count].started_epoch=meta.started_epoch;
    count++;
    free_job_meta(&meta);
  }
  closedir(d);
  qsort(index,(size_t)count,sizeof(JobIndex),compare_job_index);
  if(count>limit) count=limit;
  for(i=0;i<count;i++) {
    status=status_data(index[i].job_id,chat);
    if(status==NULL) continue;
    summary=cJSON_CreateObject();
    item=cJSON_GetObjectItemCaseSensitive(status,"job_id"); cJSON_AddItemToObject(summary,"job_id",cJSON_Duplicate(item,1));
    item=cJSON_GetObjectItemCaseSensitive(status,"chat"); cJSON_AddItemToObject(summary,"chat",cJSON_Duplicate(item,1));
    item=cJSON_GetObjectItemCaseSensitive(status,"state"); cJSON_AddItemToObject(summary,"state",cJSON_Duplicate(item,1));
    item=cJSON_GetObjectItemCaseSensitive(status,"pid"); cJSON_AddItemToObject(summary,"pid",cJSON_Duplicate(item,1));
    item=cJSON_GetObjectItemCaseSensitive(status,"elapsed_seconds"); cJSON_AddItemToObject(summary,"elapsed_seconds",cJSON_Duplicate(item,1));
    item=cJSON_GetObjectItemCaseSensitive(status,"exit_code"); cJSON_AddItemToObject(summary,"exit_code",cJSON_Duplicate(item,1));
    item=cJSON_GetObjectItemCaseSensitive(status,"cpu_percent"); cJSON_AddItemToObject(summary,"cpu_percent",cJSON_Duplicate(item,1));
    item=cJSON_GetObjectItemCaseSensitive(status,"rss_bytes"); cJSON_AddItemToObject(summary,"rss_bytes",cJSON_Duplicate(item,1));
    item=cJSON_GetObjectItemCaseSensitive(status,"command"); cJSON_AddItemToObject(summary,"command",cJSON_Duplicate(item,1));
    item=cJSON_GetObjectItemCaseSensitive(status,"cwd"); cJSON_AddItemToObject(summary,"cwd",cJSON_Duplicate(item,1));
    item=cJSON_GetObjectItemCaseSensitive(status,"started_at"); cJSON_AddItemToObject(summary,"started_at",cJSON_Duplicate(item,1));
    cJSON_AddItemToArray(array,summary);
    cJSON_Delete(status);
  }
  if(index!=NULL) free(index);
  return tool_result_json(array,0);
}

static int valid_edistribuzione_pod(const char *pod) {
  const unsigned char *p;

  if(pod==NULL || pod[0]==0 || strlen(pod)>64) return 0;
  for(p=(const unsigned char *)pod;*p!=0;p++) {
    if(!isalnum(*p) && *p!='_' && *p!='-') return 0;
  }
  return 1;
}

static int save_edistribuzione_csv(cJSON *done) {
  cJSON *result,*code,*pod,*year,*month,*days,*day,*date_key,*samples,*sample,*key,*value;
  char rel[PATH_MAX],path[PATH_MAX],tmp[PATH_MAX],date[11],time_value[6],line[256];
  const char *raw_date;
  FILE *f;
  int n,quarter,hour,minute,rc;

  result=cJSON_GetObjectItemCaseSensitive(done,"result");
  if(!cJSON_IsObject(result)) return 0;
  code=cJSON_GetObjectItemCaseSensitive(result,"code");
  if(!cJSON_IsString(code) || strcmp(code->valuestring,"OK")!=0) return 0;
  pod=cJSON_GetObjectItemCaseSensitive(result,"pod");
  year=cJSON_GetObjectItemCaseSensitive(result,"year");
  month=cJSON_GetObjectItemCaseSensitive(result,"month");
  days=cJSON_GetObjectItemCaseSensitive(result,"days");
  if(!cJSON_IsString(pod) || !valid_edistribuzione_pod(pod->valuestring) || !cJSON_IsNumber(year) ||
    !cJSON_IsNumber(month) || !cJSON_IsArray(days)) return -1;
  n=snprintf(rel,sizeof(rel),"mymcp/tmpdata/%s_%04d_%02d.csv",pod->valuestring,year->valueint,month->valueint);
  if(n<0 || (size_t)n>=sizeof(rel) || ensure_write_path(rel,path,sizeof(path))!=0) return -1;
  n=snprintf(tmp,sizeof(tmp),"%s.new",path);
  if(n<0 || (size_t)n>=sizeof(tmp)) return -1;
  f=fopen(tmp,"w");
  if(f==NULL) return -1;
  rc=0;
  if(fputs("date,time,value\n",f)==EOF) rc=-1;
  cJSON_ArrayForEach(day,days) {
    if(rc!=0) break;
    date_key=cJSON_GetObjectItemCaseSensitive(day,"date_key");
    samples=cJSON_GetObjectItemCaseSensitive(day,"samples");
    if(!cJSON_IsString(date_key) || !cJSON_IsArray(samples)) { rc=-1; break; }
    raw_date=date_key->valuestring;
    if(strlen(raw_date)==9 && raw_date[6]=='-')
      n=snprintf(date,sizeof(date),"%.4s-%.2s-%.2s",raw_date,raw_date+4,raw_date+7);
    else if(strlen(raw_date)==8)
      n=snprintf(date,sizeof(date),"%.4s-%.2s-%.2s",raw_date,raw_date+4,raw_date+6);
    else { rc=-1; break; }
    if(n!=10) { rc=-1; break; }
    cJSON_ArrayForEach(sample,samples) {
      key=cJSON_GetObjectItemCaseSensitive(sample,"key");
      value=cJSON_GetObjectItemCaseSensitive(sample,"value");
      if(!cJSON_IsString(key) || !cJSON_IsNumber(value)) { rc=-1; break; }
      quarter=atoi(key->valuestring);
      if(quarter>=1 && quarter<=96) minute=(quarter-1)*15;
      else if(quarter>=97 && quarter<=100) minute=120+(quarter-97)*15;
      else { rc=-1; break; }
      hour=minute/60;
      minute%=60;
      n=snprintf(time_value,sizeof(time_value),"%02d:%02d",hour,minute);
      if(n!=5) { rc=-1; break; }
      n=snprintf(line,sizeof(line),"%s,%s,%.15g\n",date,time_value,value->valuedouble);
      if(n<0 || (size_t)n>=sizeof(line) || fputs(line,f)==EOF) { rc=-1; break; }
    }
  }
  if(fclose(f)!=0) rc=-1;
  if(rc==0 && rename(tmp,path)!=0) rc=-1;
  if(rc!=0) unlink(tmp);
  if(rc==0) cJSON_AddStringToObject(result,"csv_file",rel);
  return rc;
}

static void maybe_save_agent_result(const char *module,const char *action,cJSON *done) {
  cJSON *status;

  if(strcmp(module,"edistribuzione")!=0 || strcmp(action,"load_profile.month")!=0) return;
  status=cJSON_GetObjectItemCaseSensitive(done,"status");
  if(!cJSON_IsString(status) || strcmp(status->valuestring,"ok")!=0) return;
  save_edistribuzione_csv(done);
}

static cJSON *tool_agent_call(cJSON *args) {
  const char *chat,*target_agent,*module,*action;
  cJSON *payload,*done,*status_item,*data;
  char request_id[AGENT_REQUEST_ID_LEN+1];
  double started;
  int timeout,state,is_error;

  chat=NULL;
  target_agent=NULL;
  module=NULL;
  action=NULL;
  timeout=AGENT_CALL_TIMEOUT;
  if(get_string_arg(args,"chat",&chat,1)<0 || !valid_chat(chat)) return tool_result_string("invalid chat",1);
  if(get_string_arg(args,"agent",&target_agent,0)<0 ||
    (target_agent!=NULL && !valid_agent_id(target_agent))) return tool_result_string("invalid agent",1);
  if(get_string_arg(args,"module",&module,1)<0 || !valid_agent_name(module)) return tool_result_string("invalid module",1);
  if(get_string_arg(args,"action",&action,1)<0 || !valid_agent_name(action)) return tool_result_string("invalid action",1);
  if(get_int_arg(args,"timeout",&timeout,AGENT_CALL_TIMEOUT)<0 || timeout<1 || timeout>AGENT_CALL_TIMEOUT_MAX)
    return tool_result_string("timeout out of range",1);
  payload=cJSON_GetObjectItemCaseSensitive(args,"payload");
  if(!agent_module_configured(module,target_agent)) return tool_result_string("no configured agent provides this module",1);
  if(agent_enqueue(chat,target_agent,module,action,payload,request_id,sizeof(request_id))!=0)
    return tool_result_string("cannot queue agent request",1);
  started=now_seconds();
  for(;;) {
    done=agent_done_read(request_id);
    if(cJSON_IsObject(done)) {
      cJSON_AddStringToObject(done,"state","done");
      status_item=cJSON_GetObjectItemCaseSensitive(done,"status");
      is_error=!cJSON_IsString(status_item) || strcmp(status_item->valuestring,"ok")!=0;
      maybe_save_agent_result(module,action,done);
      return tool_result_json(done,is_error);
    }
    cJSON_Delete(done);
    if(now_seconds()-started>=(double)timeout) break;
    usleep(50000);
  }
  done=agent_done_read(request_id);
  if(cJSON_IsObject(done)) {
    cJSON_AddStringToObject(done,"state","done");
    status_item=cJSON_GetObjectItemCaseSensitive(done,"status");
    is_error=!cJSON_IsString(status_item) || strcmp(status_item->valuestring,"ok")!=0;
    maybe_save_agent_result(module,action,done);
    return tool_result_json(done,is_error);
  }
  cJSON_Delete(done);
  state=agent_request_state(request_id);
  data=cJSON_CreateObject();
  if(data==NULL) return tool_result_string("out of memory",1);
  cJSON_AddStringToObject(data,"request_id",request_id);
  cJSON_AddStringToObject(data,"status","timeout");
  if(state==1) {
    agent_cancel_queued(request_id);
    cJSON_AddStringToObject(data,"state","cancelled");
    cJSON_AddBoolToObject(data,"delivered",0);
  } else if(state==2) {
    cJSON_AddStringToObject(data,"state","running");
    cJSON_AddBoolToObject(data,"delivered",1);
  } else {
    cJSON_AddStringToObject(data,"state","unknown");
    cJSON_AddBoolToObject(data,"delivered",0);
  }
  return tool_result_json(data,1);
}

static cJSON *dispatch_tool(const char *name,cJSON *args) {
  const char *chat;

  chat=NULL;
  if(get_string_arg(args,"chat",&chat,1)<0 || !valid_chat(chat))
    return tool_result_string("chat is required and must contain only A-Z, a-z, 0-9, _, - or . (maximum 64 characters)",1);
  if(strcmp(name,"hello")==0) return tool_hello(args);
  if(strcmp(name,"write_file")==0) return tool_write_file(args);
  if(strcmp(name,"read_file")==0) return tool_read_file(args);
  if(strcmp(name,"list_files")==0) return tool_list_files(args);
  if(strcmp(name,"read_blob")==0) return tool_read_blob(args);
  if(strcmp(name,"write_blob")==0) return tool_write_blob(args);
  if(strcmp(name,"run")==0) return tool_run(args);
  if(strcmp(name,"start")==0) return tool_start(args);
  if(strcmp(name,"status")==0) return tool_status(args);
  if(strcmp(name,"tail")==0) return tool_tail(args);
  if(strcmp(name,"stop")==0) return tool_stop(args);
  if(strcmp(name,"jobs")==0) return tool_jobs(args);
  if(strcmp(name,"agent_call")==0) return tool_agent_call(args);
  return NULL;
}

static void request_client(cJSON *params,char *out,size_t out_size) {
  cJSON *meta,*info,*name,*version;

  if(out_size==0) return;
  strcpy(out,"unknown");
  if(!cJSON_IsObject(params)) return;
  meta=cJSON_GetObjectItemCaseSensitive(params,"_meta");
  if(!cJSON_IsObject(meta)) return;
  info=cJSON_GetObjectItemCaseSensitive(meta,"io.modelcontextprotocol/clientInfo");
  if(!cJSON_IsObject(info)) return;
  name=cJSON_GetObjectItemCaseSensitive(info,"name");
  version=cJSON_GetObjectItemCaseSensitive(info,"version");
  if(!cJSON_IsString(name) || name->valuestring==NULL) return;
  if(cJSON_IsString(version) && version->valuestring!=NULL)
    snprintf(out,out_size,"%s/%s",name->valuestring,version->valuestring);
  else snprintf(out,out_size,"%s",name->valuestring);
}

static int validate_meta(HttpRequest *http,cJSON *root,cJSON **id,cJSON **params,cJSON **method,int *status,cJSON **error) {
  cJSON *jsonrpc,*meta,*version,*caps,*name;

  *id=cJSON_GetObjectItemCaseSensitive(root,"id");
  jsonrpc=cJSON_GetObjectItemCaseSensitive(root,"jsonrpc");
  *method=cJSON_GetObjectItemCaseSensitive(root,"method");
  *params=cJSON_GetObjectItemCaseSensitive(root,"params");
  if(!cJSON_IsString(jsonrpc) || strcmp(jsonrpc->valuestring,"2.0")!=0 || !cJSON_IsString(*method) || !cJSON_IsObject(*params)) {
    *status=400; *error=jsonrpc_error(*id,-32600,"Invalid Request"); return -1;
  }
  meta=cJSON_GetObjectItemCaseSensitive(*params,"_meta");
  if(!cJSON_IsObject(meta)) { *status=400; *error=jsonrpc_error(*id,-32602,"params._meta is required"); return -1; }
  version=cJSON_GetObjectItemCaseSensitive(meta,"io.modelcontextprotocol/protocolVersion");
  caps=cJSON_GetObjectItemCaseSensitive(meta,"io.modelcontextprotocol/clientCapabilities");
  if(!cJSON_IsString(version) || !cJSON_IsObject(caps)) { *status=400; *error=jsonrpc_error(*id,-32602,"required MCP request metadata is missing"); return -1; }
  if(strcmp(version->valuestring,PROTOCOL_VERSION)!=0) { *status=400; *error=jsonrpc_unsupported(*id,version->valuestring); return -1; }
  if(http->mcp_version[0]==0 || strcmp(http->mcp_version,version->valuestring)!=0) { *status=400; *error=jsonrpc_error(*id,-32020,"MCP-Protocol-Version header mismatch"); return -1; }
  if(http->mcp_method[0]==0 || strcmp(http->mcp_method,(*method)->valuestring)!=0) { *status=400; *error=jsonrpc_error(*id,-32020,"Mcp-Method header mismatch"); return -1; }
  if(strcmp((*method)->valuestring,"tools/call")==0) {
    name=cJSON_GetObjectItemCaseSensitive(*params,"name");
    if(!cJSON_IsString(name) || http->mcp_name[0]==0 || strcmp(http->mcp_name,name->valuestring)!=0) { *status=400; *error=jsonrpc_error(*id,-32020,"Mcp-Name header mismatch"); return -1; }
  }
  return 0;
}

static void handle_connection(int fd) {
  HttpRequest http;
  cJSON *root,*id,*params,*method,*response,*result,*name,*args,*error,*chat_item;
  char client[CLIENT_MAX],chat[CHAT_MAX+1],method_name[128],tool_name[256],detail[LOG_DETAIL_MAX+1];
  int rc,status,log_status;
  double started,elapsed_ms;

  root=NULL; response=NULL; error=NULL; status=200; log_status=400;
  strcpy(client,"unknown"); strcpy(chat,"-"); strcpy(method_name,"-"); strcpy(tool_name,""); detail[0]=0;
  started=now_seconds();
  rc=read_http_request(fd,&http);
  if(rc!=0) {
    response=jsonrpc_error(NULL,-32700,rc==-2?"Request too large":"Invalid HTTP request");
    send_json(fd,400,response);
    cJSON_Delete(response);
    elapsed_ms=(now_seconds()-started)*1000.0;
    log_request(chat,client,method_name,tool_name,400,elapsed_ms);
    return;
  }
  if(strcmp(http.method,"POST")!=0) {
    response=jsonrpc_error(NULL,-32600,"Only POST is supported");
    send_json(fd,405,response); cJSON_Delete(response); free_http_request(&http);
    elapsed_ms=(now_seconds()-started)*1000.0;
    log_request(chat,client,method_name,tool_name,405,elapsed_ms);
    return;
  }
  if(strcmp(http.path,"/mcp")!=0) {
    response=jsonrpc_error(NULL,-32600,"MCP endpoint not found");
    send_json(fd,404,response); cJSON_Delete(response); free_http_request(&http);
    elapsed_ms=(now_seconds()-started)*1000.0;
    log_request(chat,client,method_name,tool_name,404,elapsed_ms);
    return;
  }
  if(http.agent_id[0]!=0 || http.agent_action[0]!=0 || http.agent_token[0]!=0) {
    snprintf(client,sizeof(client),"agent/%s",http.agent_id[0]!=0?http.agent_id:"unknown");
    snprintf(method_name,sizeof(method_name),"agent/%s",http.agent_action[0]!=0?http.agent_action:"unknown");
    status=handle_agent_connection(fd,&http);
    free_http_request(&http);
    elapsed_ms=(now_seconds()-started)*1000.0;
    log_request(chat,client,method_name,tool_name,status,elapsed_ms);
    return;
  }
  root=cJSON_ParseWithLength(http.body,http.body_len);
  if(root==NULL) {
    response=jsonrpc_error(NULL,-32700,"Parse error");
    send_json(fd,400,response); cJSON_Delete(response); free_http_request(&http);
    elapsed_ms=(now_seconds()-started)*1000.0;
    log_request(chat,client,method_name,tool_name,400,elapsed_ms);
    return;
  }
  params=cJSON_GetObjectItemCaseSensitive(root,"params");
  request_client(params,client,sizeof(client));
  method=cJSON_GetObjectItemCaseSensitive(root,"method");
  if(cJSON_IsString(method) && method->valuestring!=NULL) snprintf(method_name,sizeof(method_name),"%s",method->valuestring);
  if(validate_meta(&http,root,&id,&params,&method,&status,&error)!=0) {
    send_json(fd,status,error); cJSON_Delete(error); cJSON_Delete(root); free_http_request(&http);
    elapsed_ms=(now_seconds()-started)*1000.0;
    log_request(chat,client,method_name,tool_name,status,elapsed_ms);
    return;
  }
  if(strcmp(method->valuestring,"server/discover")==0) result=handle_discover();
  else if(strcmp(method->valuestring,"tools/list")==0) result=handle_tools_list();
  else if(strcmp(method->valuestring,"ping")==0) { result=cJSON_CreateObject(); cJSON_AddStringToObject(result,"resultType","complete"); add_server_meta(result); }
  else if(strcmp(method->valuestring,"tools/call")==0) {
    name=cJSON_GetObjectItemCaseSensitive(params,"name");
    args=cJSON_GetObjectItemCaseSensitive(params,"arguments");
    if(!cJSON_IsString(name)) {
      response=jsonrpc_error(id,-32602,"tool name is required");
      send_json(fd,200,response); cJSON_Delete(response); cJSON_Delete(root); free_http_request(&http);
      elapsed_ms=(now_seconds()-started)*1000.0;
      log_request(chat,client,method_name,tool_name,200,elapsed_ms);
      return;
    }
    snprintf(tool_name,sizeof(tool_name),"%s",name->valuestring);
    if(args==NULL) args=cJSON_CreateObject();
    else if(!cJSON_IsObject(args)) {
      response=jsonrpc_error(id,-32602,"tool arguments must be an object");
      send_json(fd,200,response); cJSON_Delete(response); cJSON_Delete(root); free_http_request(&http);
      elapsed_ms=(now_seconds()-started)*1000.0;
      log_request(chat,client,method_name,tool_name,200,elapsed_ms);
      return;
    }
    chat_item=cJSON_GetObjectItemCaseSensitive(args,"chat");
    if(cJSON_IsString(chat_item) && chat_item->valuestring!=NULL && valid_chat(chat_item->valuestring))
      snprintf(chat,sizeof(chat),"%s",chat_item->valuestring);
    else {
      result=tool_result_string("chat is required and must contain only a-z A-Z 0-9 _ - .",1);
      if(cJSON_GetObjectItemCaseSensitive(params,"arguments")==NULL) cJSON_Delete(args);
      response=jsonrpc_result(id,result);
      send_json(fd,200,response); cJSON_Delete(response); cJSON_Delete(root); free_http_request(&http);
      elapsed_ms=(now_seconds()-started)*1000.0;
      log_request(chat,client,method_name,tool_name,200,elapsed_ms);
      return;
    }
    result=dispatch_tool(name->valuestring,args);
    if(result!=NULL) build_tool_log_detail(name->valuestring,args,result,detail,sizeof(detail));
    if(cJSON_GetObjectItemCaseSensitive(params,"arguments")==NULL) cJSON_Delete(args);
    if(result==NULL) {
      response=jsonrpc_error(id,-32602,"unknown tool");
      send_json(fd,200,response); cJSON_Delete(response); cJSON_Delete(root); free_http_request(&http);
      elapsed_ms=(now_seconds()-started)*1000.0;
      log_request(chat,client,method_name,tool_name,200,elapsed_ms);
      return;
    }
  } else {
    response=jsonrpc_error(id,-32601,"Method not found");
    send_json(fd,200,response); cJSON_Delete(response); cJSON_Delete(root); free_http_request(&http);
    elapsed_ms=(now_seconds()-started)*1000.0;
    log_request(chat,client,method_name,tool_name,200,elapsed_ms);
    return;
  }
  response=jsonrpc_result(id,result);
  send_json(fd,200,response);
  log_status=200;
  cJSON_Delete(response);
  cJSON_Delete(root);
  free_http_request(&http);
  elapsed_ms=(now_seconds()-started)*1000.0;
  if(detail[0]!=0) log_request_detail(chat,client,method_name,tool_name,detail,log_status,elapsed_ms);
  else log_request(chat,client,method_name,tool_name,log_status,elapsed_ms);
}

static int parse_port(const char *s) {
  long p;
  char *end;

  errno=0;
  p=strtol(s,&end,10);
  if(errno!=0 || *s==0 || *end!=0 || p<1 || p>65535) return -1;
  return (int)p;
}

static int server_loop(const char *address,int port) {
  int server_fd,client_fd,one;
  struct sockaddr_in sa;
  socklen_t sa_len;
  pid_t pid;

  if(check_log_file()!=0) return -1;
  server_fd=socket(AF_INET,SOCK_STREAM,0);
  if(server_fd<0) return -1;
  one=1;
  setsockopt(server_fd,SOL_SOCKET,SO_REUSEADDR,&one,sizeof(one));
  memset(&sa,0,sizeof(sa));
  sa.sin_family=AF_INET;
  sa.sin_port=htons((unsigned short)port);
  if(inet_pton(AF_INET,address,&sa.sin_addr)!=1) { close(server_fd); errno=EINVAL; return -1; }
  if(bind(server_fd,(struct sockaddr *)&sa,sizeof(sa))!=0) { close(server_fd); return -1; }
  if(listen(server_fd,BACKLOG)!=0) { close(server_fd); return -1; }
  signal(SIGCHLD,SIG_IGN);
  signal(SIGPIPE,SIG_IGN);
  fprintf(stderr,"%s %s listening on %s:%d\n",SERVER_NAME,SERVER_VERSION,address,port);
  for(;;) {
    sa_len=sizeof(sa);
    client_fd=accept(server_fd,(struct sockaddr *)&sa,&sa_len);
    if(client_fd<0) { if(errno==EINTR) continue; close(server_fd); return -1; }
    pid=fork();
    if(pid<0) { close(client_fd); continue; }
    if(pid==0) {
      signal(SIGCHLD,SIG_DFL);
      close(server_fd);
      handle_connection(client_fd);
      close(client_fd);
      _exit(0);
    }
    close(client_fd);
  }
}

int main(int argc,char **argv) {
  const char *address;
  int port,i,p;

  address=DEFAULT_ADDR;
  port=DEFAULT_PORT;
  for(i=1;i<argc;i++) {
    if(strcmp(argv[i],"-p")==0 && i+1<argc) {
      p=parse_port(argv[++i]);
      if(p<0) { fprintf(stderr,"invalid port\n"); return 2; }
      port=p;
    } else if(strcmp(argv[i],"-a")==0 && i+1<argc) address=argv[++i];
    else if(strcmp(argv[i],"-h")==0 || strcmp(argv[i],"--help")==0) {
      printf("usage: %s [-a address] [-p port]\n",argv[0]);
      return 0;
    } else {
      fprintf(stderr,"unknown argument: %s\n",argv[i]);
      return 2;
    }
  }
  if(server_loop(address,port)!=0) {
    fprintf(stderr,"server error: %s\n",strerror(errno));
    return 1;
  }
  return 0;
}
