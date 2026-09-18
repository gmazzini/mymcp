// Gianluca Mazzini @2026- Version 1.37
#include <cjson/cJSON.h>
#include <curl/curl.h>
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define MCP_AGENT_VERSION "1.37"
#define DEFAULT_URL "https://www.mazzini.org/mcp"
#define DEFAULT_AGENT_ID "mac1"
#define CHROME_HOST "http://127.0.0.1:9222"
#define HTTP_WAIT_TIMEOUT 35L
#define HTTP_RESULT_TIMEOUT 30L
#define CHROME_HTTP_TIMEOUT_MS 3000L
#define WS_TIMEOUT_MS 5000
#define BROWSER_TEXT_MAX 65536
#define QRZ_CALL_MAX 19
#define QRZ_EXPRESSION_MAX 16384
#define TOKEN_MAX 511
#define AGENT_TOKEN_MAX 255
#define AGENT_ID_MAX 64
#define MCP_PROTOCOL "2026-07-28"
#define MCP_CLIENT_NAME "mcp-agent"
#define WATCH_INTERVAL 60
#define CHAT_MAX 64
#define CONVERSATION_MAX 128
#define JOB_ID_MAX 63
#define WATCH_TITLE_MAX 256
#define JOB_RUNNING 1
#define JOB_EXITED 2

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

struct Buffer {
  char *data;
  size_t len;
};

struct TabInfo {
  char chat[CHAT_MAX+1];
  char conversation_id[CONVERSATION_MAX+1];
  char title[WATCH_TITLE_MAX];
  int number;
  int ambiguous;
};

struct JobState {
  char job_id[JOB_ID_MAX+1];
  int state;
  int notified;
  int seen;
};

struct ChatState {
  char chat[CHAT_MAX+1];
  char conversation_id[CONVERSATION_MAX+1];
  char title[WATCH_TITLE_MAX];
  struct JobState *jobs;
  size_t job_count;
  size_t job_cap;
  int active;
  int initialized;
};

static char g_mcp_token[TOKEN_MAX+1];
static char g_agent_token[AGENT_TOKEN_MAX+1];

static void log_line(FILE *stream,const char *component,const char *fmt,...) {
  char stamp[32];
  time_t now;
  struct tm tmv;
  va_list ap;

  now=time(NULL);
  localtime_r(&now,&tmv);
  strftime(stamp,sizeof(stamp),"%Y-%m-%d %H:%M:%S",&tmv);
  fprintf(stream,"[%s] %-5s ",stamp,component);
  va_start(ap,fmt);
  vfprintf(stream,fmt,ap);
  va_end(ap);
  fputc('\n',stream);
  fflush(stream);
}

static size_t http_write(char *ptr,size_t size,size_t nmemb,void *userdata) {
  struct Buffer *b;
  char *p;
  size_t n;

  b=(struct Buffer *)userdata;
  n=size*nmemb;
  p=(char *)realloc(b->data,b->len+n+1);
  if(p==NULL) return 0;
  b->data=p;
  memcpy(b->data+b->len,ptr,n);
  b->len+=n;
  b->data[b->len]=0;
  return n;
}

static int valid_name(const char *s,size_t max_len) {
  size_t i,n;
  unsigned char c;

  if(s==NULL) return 0;
  n=strlen(s);
  if(n<1 || n>max_len) return 0;
  for(i=0;i<n;i++) {
    c=(unsigned char)s[i];
    if(!isalnum(c) && c!='_' && c!='-' && c!='.') return 0;
  }
  return 1;
}

static const char *agent_url(void) {
  const char *url;

  url=getenv("MCP_AGENT_URL");
  if(url!=NULL && url[0]!=0) return url;
  return DEFAULT_URL;
}

static const char *agent_id(void) {
  const char *id;

  id=getenv("MCP_AGENT_ID");
  if(id!=NULL && id[0]!=0) return id;
  return DEFAULT_AGENT_ID;
}

static const char *chrome_host(void) {
  const char *url;

  url=getenv("MCP_CHROME_URL");
  if(url!=NULL && url[0]!=0) return url;
  return CHROME_HOST;
}

static int read_text_token(const char *env_name,const char *filename,char *out,size_t out_size) {
  const char *env,*home;
  char path[PATH_MAX];
  FILE *f;
  size_t n;

  env=getenv(env_name);
  if(env!=NULL && env[0]!=0) {
    if(strlen(env)+1>out_size) return -1;
    strcpy(out,env);
    return 0;
  }
  home=getenv("HOME");
  if(home==NULL || home[0]==0) return -1;
  if(snprintf(path,sizeof(path),"%s/mcp/%s",home,filename)>=(int)sizeof(path)) return -1;
  f=fopen(path,"r");
  if(f==NULL) {
    fprintf(stderr,"cannot open %s: %s\n",path,strerror(errno));
    return -1;
  }
  if(fgets(out,(int)out_size,f)==NULL) {
    fclose(f);
    return -1;
  }
  fclose(f);
  n=strlen(out);
  for(;n>0 && isspace((unsigned char)out[n-1]);n--) out[n-1]=0;
  return out[0]!=0?0:-1;
}

static int http_agent_request(CURL *curl,const char *action,const char *body_text,long timeout,char **reply_out,long *http_code_out) {
  char auth[640],id_header[128],action_header[64],token_header[384];
  struct curl_slist *headers;
  struct Buffer reply;
  CURLcode rc;
  long http_code;

  *reply_out=NULL;
  *http_code_out=0;
  if(curl==NULL) return -1;
  if(!valid_name(agent_id(),AGENT_ID_MAX)) {
    fprintf(stderr,"invalid agent id\n");
    return -1;
  }
  if(snprintf(auth,sizeof(auth),"Authorization: Bearer %s",g_mcp_token)>=(int)sizeof(auth) ||
    snprintf(id_header,sizeof(id_header),"X-MCP-Agent: %s",agent_id())>=(int)sizeof(id_header) ||
    snprintf(action_header,sizeof(action_header),"X-MCP-Agent-Action: %s",action)>=(int)sizeof(action_header) ||
    snprintf(token_header,sizeof(token_header),"X-MCP-Agent-Token: %s",g_agent_token)>=(int)sizeof(token_header)) return -1;

  headers=NULL;
  headers=curl_slist_append(headers,"Content-Type: application/json");
  headers=curl_slist_append(headers,auth);
  headers=curl_slist_append(headers,id_header);
  headers=curl_slist_append(headers,action_header);
  headers=curl_slist_append(headers,token_header);
  reply.data=NULL;
  reply.len=0;
  curl_easy_reset(curl);
  curl_easy_setopt(curl,CURLOPT_URL,agent_url());
  curl_easy_setopt(curl,CURLOPT_POST,1L);
  curl_easy_setopt(curl,CURLOPT_HTTPHEADER,headers);
  curl_easy_setopt(curl,CURLOPT_POSTFIELDS,body_text!=NULL?body_text:"{}");
  curl_easy_setopt(curl,CURLOPT_POSTFIELDSIZE,(long)strlen(body_text!=NULL?body_text:"{}"));
  curl_easy_setopt(curl,CURLOPT_WRITEFUNCTION,http_write);
  curl_easy_setopt(curl,CURLOPT_WRITEDATA,&reply);
  curl_easy_setopt(curl,CURLOPT_CONNECTTIMEOUT,10L);
  curl_easy_setopt(curl,CURLOPT_TIMEOUT,timeout);
  curl_easy_setopt(curl,CURLOPT_NOSIGNAL,1L);
  curl_easy_setopt(curl,CURLOPT_TCP_KEEPALIVE,1L);
  rc=curl_easy_perform(curl);
  http_code=0;
  curl_easy_getinfo(curl,CURLINFO_RESPONSE_CODE,&http_code);
  curl_slist_free_all(headers);
  if(rc!=CURLE_OK) {
    log_line(stderr,"ERROR","HTTP %s",curl_easy_strerror(rc));
    free(reply.data);
    return -1;
  }
  *reply_out=reply.data;
  *http_code_out=http_code;
  return 0;
}

static int chrome_get(const char *url,struct Buffer *out) {
  CURL *curl;
  CURLcode rc;

  out->data=NULL;
  out->len=0;
  curl=curl_easy_init();
  if(curl==NULL) return -1;
  curl_easy_setopt(curl,CURLOPT_URL,url);
  curl_easy_setopt(curl,CURLOPT_WRITEFUNCTION,http_write);
  curl_easy_setopt(curl,CURLOPT_WRITEDATA,out);
  curl_easy_setopt(curl,CURLOPT_CONNECTTIMEOUT_MS,CHROME_HTTP_TIMEOUT_MS);
  curl_easy_setopt(curl,CURLOPT_TIMEOUT_MS,CHROME_HTTP_TIMEOUT_MS);
  curl_easy_setopt(curl,CURLOPT_NOSIGNAL,1L);
  rc=curl_easy_perform(curl);
  curl_easy_cleanup(curl);
  if(rc!=CURLE_OK) {
    free(out->data);
    out->data=NULL;
    return -1;
  }
  return 0;
}

static int find_tab(const char *wanted_title,char **ws_url,char **tab_url) {
  struct Buffer body;
  cJSON *root,*item,*type,*title,*url,*ws;
  char endpoint[256];
  int i,n,count;

  *ws_url=NULL;
  *tab_url=NULL;
  if(snprintf(endpoint,sizeof(endpoint),"%s/json/list",chrome_host())>=(int)sizeof(endpoint)) return -1;
  if(chrome_get(endpoint,&body)!=0) return -2;
  root=cJSON_Parse(body.data!=NULL?body.data:"");
  free(body.data);
  if(!cJSON_IsArray(root)) {
    cJSON_Delete(root);
    return -2;
  }
  count=0;
  n=cJSON_GetArraySize(root);
  for(i=0;i<n;i++) {
    item=cJSON_GetArrayItem(root,i);
    type=cJSON_GetObjectItemCaseSensitive(item,"type");
    title=cJSON_GetObjectItemCaseSensitive(item,"title");
    url=cJSON_GetObjectItemCaseSensitive(item,"url");
    ws=cJSON_GetObjectItemCaseSensitive(item,"webSocketDebuggerUrl");
    if(!cJSON_IsString(type) || strcmp(type->valuestring,"page")!=0) continue;
    if(!cJSON_IsString(title) || strcmp(title->valuestring,wanted_title)!=0) continue;
    if(!cJSON_IsString(url) || !cJSON_IsString(ws)) continue;
    count++;
    if(count==1) {
      *ws_url=strdup(ws->valuestring);
      *tab_url=strdup(url->valuestring);
      if(*ws_url==NULL || *tab_url==NULL) {
        free(*ws_url);
        free(*tab_url);
        *ws_url=NULL;
        *tab_url=NULL;
        cJSON_Delete(root);
        return -3;
      }
    }
  }
  cJSON_Delete(root);
  if(count==1) return 0;
  free(*ws_url);
  free(*tab_url);
  *ws_url=NULL;
  *tab_url=NULL;
  return count==0?-4:-5;
}

static int find_qrz_tab(char **ws_url) {
  struct Buffer body;
  cJSON *root,*item,*type,*url,*ws;
  char endpoint[256];
  const char *page_url;
  int i,n;

  *ws_url=NULL;
  if(snprintf(endpoint,sizeof(endpoint),"%s/json/list",chrome_host())>=(int)sizeof(endpoint)) return -1;
  if(chrome_get(endpoint,&body)!=0) return -2;
  root=cJSON_Parse(body.data!=NULL?body.data:"");
  free(body.data);
  if(!cJSON_IsArray(root)) {
    cJSON_Delete(root);
    return -2;
  }
  n=cJSON_GetArraySize(root);
  for(i=0;i<n;i++) {
    item=cJSON_GetArrayItem(root,i);
    type=cJSON_GetObjectItemCaseSensitive(item,"type");
    url=cJSON_GetObjectItemCaseSensitive(item,"url");
    ws=cJSON_GetObjectItemCaseSensitive(item,"webSocketDebuggerUrl");
    if(!cJSON_IsString(type) || strcmp(type->valuestring,"page")!=0) continue;
    if(!cJSON_IsString(url) || !cJSON_IsString(ws)) continue;
    page_url=url->valuestring;
    if(strncmp(page_url,"https://www.qrz.com/",20)!=0 &&
      strncmp(page_url,"http://www.qrz.com/",19)!=0 &&
      strncmp(page_url,"https://qrz.com/",16)!=0 &&
      strncmp(page_url,"http://qrz.com/",15)!=0) continue;
    *ws_url=strdup(ws->valuestring);
    cJSON_Delete(root);
    return *ws_url!=NULL?0:-3;
  }
  cJSON_Delete(root);
  return -4;
}

static int wait_socket(CURL *ws,short events,int timeout_ms) {
  curl_socket_t sock;
  struct pollfd pfd;
  CURLcode rc;
  int n;

  rc=curl_easy_getinfo(ws,CURLINFO_ACTIVESOCKET,&sock);
  if(rc!=CURLE_OK || sock==CURL_SOCKET_BAD) return -1;
  memset(&pfd,0,sizeof(pfd));
  pfd.fd=(int)sock;
  pfd.events=events;
  n=poll(&pfd,1,timeout_ms);
  return n>0?0:-1;
}

static int ws_send_text(CURL *ws,const char *text) {
  CURLcode rc;
  size_t off,sent,len;

  off=0;
  len=strlen(text);
  for(;off<len;) {
    sent=0;
    rc=curl_ws_send(ws,text+off,len-off,&sent,0,CURLWS_TEXT);
    off+=sent;
    if(off==len) return 0;
    if(rc==CURLE_AGAIN) {
      if(wait_socket(ws,POLLOUT,WS_TIMEOUT_MS)!=0) return -1;
      continue;
    }
    if(rc!=CURLE_OK) return -1;
  }
  return 0;
}

static int ws_recv_text(CURL *ws,char **out) {
  CURLcode rc;
  const struct curl_ws_frame *meta;
  struct Buffer b;
  char chunk[4096];
  char *p;
  size_t got;

  b.data=NULL;
  b.len=0;
  for(;;) {
    got=0;
    meta=NULL;
    rc=curl_ws_recv(ws,chunk,sizeof(chunk),&got,&meta);
    if(rc==CURLE_AGAIN) {
      if(wait_socket(ws,POLLIN,WS_TIMEOUT_MS)!=0) {
        free(b.data);
        return -1;
      }
      continue;
    }
    if(rc!=CURLE_OK) {
      free(b.data);
      return -1;
    }
    if(got>0) {
      p=(char *)realloc(b.data,b.len+got+1);
      if(p==NULL) {
        free(b.data);
        return -1;
      }
      b.data=p;
      memcpy(b.data+b.len,chunk,got);
      b.len+=got;
      b.data[b.len]=0;
    }
    if(meta!=NULL && meta->bytesleft==0) {
      if(meta->flags&CURLWS_TEXT) {
        if(b.data==NULL) b.data=strdup("");
        if(b.data==NULL) return -1;
        *out=b.data;
        return 0;
      }
      free(b.data);
      b.data=NULL;
      b.len=0;
    }
  }
}

static int cdp_call(CURL *ws,int id,const char *method,cJSON *params,cJSON **reply) {
  cJSON *req,*root,*rid,*error;
  char *json,*message;
  int rc;

  req=cJSON_CreateObject();
  if(req==NULL) return -1;
  cJSON_AddNumberToObject(req,"id",id);
  cJSON_AddStringToObject(req,"method",method);
  if(params!=NULL) cJSON_AddItemToObject(req,"params",params);
  json=cJSON_PrintUnformatted(req);
  cJSON_Delete(req);
  if(json==NULL) return -1;
  rc=ws_send_text(ws,json);
  free(json);
  if(rc!=0) return -1;
  for(;;) {
    message=NULL;
    if(ws_recv_text(ws,&message)!=0) return -1;
    root=cJSON_Parse(message!=NULL?message:"");
    free(message);
    if(root==NULL) continue;
    rid=cJSON_GetObjectItemCaseSensitive(root,"id");
    if(!cJSON_IsNumber(rid) || rid->valueint!=id) {
      cJSON_Delete(root);
      continue;
    }
    error=cJSON_GetObjectItemCaseSensitive(root,"error");
    if(error!=NULL) {
      cJSON_Delete(root);
      return -1;
    }
    if(reply!=NULL) *reply=root;
    else cJSON_Delete(root);
    return 0;
  }
}

static cJSON *cdp_eval_value(cJSON *reply) {
  cJSON *result,*remote,*value;

  result=cJSON_GetObjectItemCaseSensitive(reply,"result");
  if(!cJSON_IsObject(result)) return NULL;
  remote=cJSON_GetObjectItemCaseSensitive(result,"result");
  if(!cJSON_IsObject(remote)) return NULL;
  value=cJSON_GetObjectItemCaseSensitive(remote,"value");
  if(value==NULL) return NULL;
  return cJSON_Duplicate(value,1);
}

static cJSON *browser_read_tab(const char *title,char **error_out) {
  const char *expression;
  char *ws_url,*tab_url;
  cJSON *params,*reply,*value,*url_item;
  CURL *ws;
  CURLcode curl_rc;
  int rc;

  ws_url=NULL;
  tab_url=NULL;
  ws=NULL;
  reply=NULL;
  value=NULL;
  rc=find_tab(title,&ws_url,&tab_url);
  if(rc!=0) {
    if(rc==-2) *error_out=strdup("cannot connect to Chrome CDP on 127.0.0.1:9222");
    else if(rc==-4) *error_out=strdup("tab title not found");
    else if(rc==-5) *error_out=strdup("multiple tabs have the same title");
    else *error_out=strdup("cannot inspect Chrome tabs");
    return NULL;
  }
  ws=curl_easy_init();
  if(ws==NULL) goto failed;
  curl_easy_setopt(ws,CURLOPT_URL,ws_url);
  curl_easy_setopt(ws,CURLOPT_CONNECT_ONLY,2L);
  curl_easy_setopt(ws,CURLOPT_CONNECTTIMEOUT_MS,CHROME_HTTP_TIMEOUT_MS);
  curl_easy_setopt(ws,CURLOPT_NOSIGNAL,1L);
  curl_rc=curl_easy_perform(ws);
  if(curl_rc!=CURLE_OK) goto failed;
  expression="(() => {"
    "const t=document.body?document.body.innerText:'';"
    "return {title:document.title,url:location.href,text:t.slice(0,65536),text_length:t.length,truncated:t.length>65536};"
    "})()";
  params=cJSON_CreateObject();
  if(params==NULL) goto failed;
  cJSON_AddStringToObject(params,"expression",expression);
  cJSON_AddBoolToObject(params,"returnByValue",1);
  if(cdp_call(ws,1,"Runtime.evaluate",params,&reply)!=0) goto failed;
  value=cdp_eval_value(reply);
  cJSON_Delete(reply);
  reply=NULL;
  if(!cJSON_IsObject(value)) goto failed;
  url_item=cJSON_GetObjectItemCaseSensitive(value,"url");
  if(!cJSON_IsString(url_item)) {
    cJSON_Delete(value);
    value=NULL;
    goto failed;
  }
  curl_easy_cleanup(ws);
  free(ws_url);
  free(tab_url);
  return value;

failed:
  cJSON_Delete(reply);
  cJSON_Delete(value);
  if(ws!=NULL) curl_easy_cleanup(ws);
  free(ws_url);
  free(tab_url);
  *error_out=strdup("Chrome CDP read failed");
  return NULL;
}

// E-Distribuzione

struct EdistribuzioneRequest {
  char *request_id;
  int body_id;
  int finished;
  int body_done;
};

static cJSON *edistribuzione_result(const char *code,const char *detail) {
  cJSON *result;

  result=cJSON_CreateObject();
  if(result==NULL) return NULL;
  cJSON_AddStringToObject(result,"code",code);
  if(detail!=NULL && detail[0]!=0) cJSON_AddStringToObject(result,"detail",detail);
  return result;
}

static int edistribuzione_find_tab(char **ws_url) {
  struct Buffer body;
  cJSON *root,*item,*type,*url,*ws;
  char endpoint[256];
  const char *page_url,*prefix;
  int i,n,count;
  size_t prefix_len;

  *ws_url=NULL;
  prefix="https://private.e-distribuzione.it/PortaleClienti";
  prefix_len=strlen(prefix);
  if(snprintf(endpoint,sizeof(endpoint),"%s/json/list",chrome_host())>=(int)sizeof(endpoint)) return -1;
  if(chrome_get(endpoint,&body)!=0) return -2;
  root=cJSON_Parse(body.data!=NULL?body.data:"");
  free(body.data);
  if(!cJSON_IsArray(root)) {
    cJSON_Delete(root);
    return -2;
  }
  count=0;
  n=cJSON_GetArraySize(root);
  for(i=0;i<n;i++) {
    item=cJSON_GetArrayItem(root,i);
    type=cJSON_GetObjectItemCaseSensitive(item,"type");
    url=cJSON_GetObjectItemCaseSensitive(item,"url");
    ws=cJSON_GetObjectItemCaseSensitive(item,"webSocketDebuggerUrl");
    if(!cJSON_IsString(type) || strcmp(type->valuestring,"page")!=0) continue;
    if(!cJSON_IsString(url) || !cJSON_IsString(ws)) continue;
    page_url=url->valuestring;
    if(strncmp(page_url,prefix,prefix_len)!=0) continue;
    if(page_url[prefix_len]!=0 && page_url[prefix_len]!='/') continue;
    count++;
    if(count==1) {
      *ws_url=strdup(ws->valuestring);
      if(*ws_url==NULL) {
        cJSON_Delete(root);
        return -3;
      }
    }
  }
  cJSON_Delete(root);
  if(count==1) return 0;
  free(*ws_url);
  *ws_url=NULL;
  return count==0?-4:-5;
}

static int edistribuzione_recv_text(CURL *ws,char **out,int timeout_ms) {
  CURLcode rc;
  const struct curl_ws_frame *meta;
  struct Buffer b;
  char chunk[4096];
  char *p;
  size_t got;

  b.data=NULL;
  b.len=0;
  for(;;) {
    got=0;
    meta=NULL;
    rc=curl_ws_recv(ws,chunk,sizeof(chunk),&got,&meta);
    if(rc==CURLE_AGAIN) {
      if(wait_socket(ws,POLLIN,timeout_ms)!=0) {
        free(b.data);
        return -2;
      }
      continue;
    }
    if(rc!=CURLE_OK) {
      free(b.data);
      return -1;
    }
    if(got>0) {
      p=(char *)realloc(b.data,b.len+got+1);
      if(p==NULL) {
        free(b.data);
        return -1;
      }
      b.data=p;
      memcpy(b.data+b.len,chunk,got);
      b.len+=got;
      b.data[b.len]=0;
    }
    if(meta!=NULL && meta->bytesleft==0) {
      if(meta->flags&CURLWS_TEXT) {
        if(b.data==NULL) b.data=strdup("");
        if(b.data==NULL) return -1;
        *out=b.data;
        return 0;
      }
      free(b.data);
      b.data=NULL;
      b.len=0;
    }
  }
}

static int edistribuzione_cdp_send(CURL *ws,int id,const char *method,cJSON *params) {
  cJSON *req;
  char *json;
  int rc;

  req=cJSON_CreateObject();
  if(req==NULL) {
    cJSON_Delete(params);
    return -1;
  }
  cJSON_AddNumberToObject(req,"id",id);
  cJSON_AddStringToObject(req,"method",method);
  if(params!=NULL) cJSON_AddItemToObject(req,"params",params);
  json=cJSON_PrintUnformatted(req);
  cJSON_Delete(req);
  if(json==NULL) return -1;
  rc=ws_send_text(ws,json);
  free(json);
  return rc;
}

static int edistribuzione_cdp_call(CURL *ws,int id,const char *method,cJSON *params,cJSON **reply) {
  cJSON *root,*rid,*error;
  char *message;
  int rc;

  if(edistribuzione_cdp_send(ws,id,method,params)!=0) return -1;
  for(;;) {
    message=NULL;
    rc=edistribuzione_recv_text(ws,&message,WS_TIMEOUT_MS);
    if(rc!=0) return -1;
    root=cJSON_Parse(message!=NULL?message:"");
    free(message);
    if(root==NULL) continue;
    rid=cJSON_GetObjectItemCaseSensitive(root,"id");
    if(!cJSON_IsNumber(rid) || rid->valueint!=id) {
      cJSON_Delete(root);
      continue;
    }
    error=cJSON_GetObjectItemCaseSensitive(root,"error");
    if(error!=NULL) {
      cJSON_Delete(root);
      return -1;
    }
    *reply=root;
    return 0;
  }
}

static int valid_edistribuzione_magnitude(const char *magnitude) {
  if(magnitude==NULL) return 0;
  return strcmp(magnitude,"A+")==0 || strcmp(magnitude,"A-")==0 ||
    strcmp(magnitude,"RI+")==0 || strcmp(magnitude,"RC+")==0 ||
    strcmp(magnitude,"RI-")==0 || strcmp(magnitude,"RC-")==0;
}

static cJSON *edistribuzione_find_key(cJSON *node,const char *key) {
  cJSON *item,*child,*found;

  if(cJSON_IsObject(node)) {
    item=cJSON_GetObjectItemCaseSensitive(node,key);
    if(item!=NULL) return item;
    for(child=node->child;child!=NULL;child=child->next) {
      found=edistribuzione_find_key(child,key);
      if(found!=NULL) return found;
    }
  } else if(cJSON_IsArray(node)) {
    for(child=node->child;child!=NULL;child=child->next) {
      found=edistribuzione_find_key(child,key);
      if(found!=NULL) return found;
    }
  }
  return NULL;
}

static cJSON *edistribuzione_daily_result(cJSON *daily,int year,int month,const char *magnitude) {
  cJSON *out,*days,*day_out,*samples,*sample_out,*day,*sample,*value;
  int sample_count;

  out=edistribuzione_result("OK","");
  days=cJSON_CreateArray();
  if(out==NULL || days==NULL) {
    cJSON_Delete(out);
    cJSON_Delete(days);
    return NULL;
  }
  sample_count=0;
  for(day=daily->child;day!=NULL;day=day->next) {
    if(!cJSON_IsObject(day)) continue;
    day_out=cJSON_CreateObject();
    samples=cJSON_CreateArray();
    if(day_out==NULL || samples==NULL) {
      cJSON_Delete(day_out);
      cJSON_Delete(samples);
      cJSON_Delete(days);
      cJSON_Delete(out);
      return NULL;
    }
    cJSON_AddStringToObject(day_out,"date_key",day->string!=NULL?day->string:"");
    for(sample=day->child;sample!=NULL;sample=sample->next) {
      sample_out=cJSON_CreateObject();
      if(sample_out==NULL) {
        cJSON_Delete(samples);
        cJSON_Delete(day_out);
        cJSON_Delete(days);
        cJSON_Delete(out);
        return NULL;
      }
      cJSON_AddStringToObject(sample_out,"key",sample->string!=NULL?sample->string:"");
      value=cJSON_Duplicate(sample,1);
      if(value==NULL) {
        cJSON_Delete(sample_out);
        cJSON_Delete(samples);
        cJSON_Delete(day_out);
        cJSON_Delete(days);
        cJSON_Delete(out);
        return NULL;
      }
      cJSON_AddItemToObject(sample_out,"value",value);
      cJSON_AddItemToArray(samples,sample_out);
      sample_count++;
    }
    cJSON_AddItemToObject(day_out,"samples",samples);
    cJSON_AddItemToArray(days,day_out);
  }
  cJSON_AddNumberToObject(out,"year",year);
  cJSON_AddNumberToObject(out,"month",month);
  cJSON_AddStringToObject(out,"magnitude",magnitude);
  cJSON_AddNumberToObject(out,"sample_minutes",15);
  cJSON_AddNumberToObject(out,"sample_count",sample_count);
  cJSON_AddItemToObject(out,"days",days);
  return out;
}

static cJSON *edistribuzione_parse_aura_body(const char *body,int year,int month,const char *magnitude) {
  cJSON *root,*daily,*status,*message,*out;
  const char *status_value;

  root=cJSON_Parse(body!=NULL?body:"");
  if(root==NULL) return NULL;
  daily=edistribuzione_find_key(root,"MappaDailyLoadProfile");
  if(cJSON_IsObject(daily) && daily->child!=NULL) {
    out=edistribuzione_daily_result(daily,year,month,magnitude);
    cJSON_Delete(root);
    return out;
  }
  status=edistribuzione_find_key(root,"status");
  status_value=NULL;
  if(cJSON_IsString(status)) status_value=status->valuestring;
  else if(cJSON_IsNumber(status)) {
    if(status->valueint==2000) status_value="2000";
    else if(status->valueint==2004) status_value="2004";
  }
  if(status_value!=NULL && (strcmp(status_value,"2000")==0 || strcmp(status_value,"2004")==0)) {
    out=edistribuzione_result("NO_DATA","no quarter-hour load-profile data for the requested month");
    if(out!=NULL) {
      cJSON_AddNumberToObject(out,"year",year);
      cJSON_AddNumberToObject(out,"month",month);
      cJSON_AddStringToObject(out,"magnitude",magnitude);
      cJSON_AddStringToObject(out,"status",status_value);
      cJSON_AddNumberToObject(out,"sample_count",0);
      cJSON_AddItemToObject(out,"days",cJSON_CreateArray());
    }
    cJSON_Delete(root);
    return out;
  }
  if(strstr(body,"\"state\":\"ERROR\"")!=NULL || strstr(body,"Attempt to de-reference a null object")!=NULL) {
    message=edistribuzione_find_key(root,"message");
    out=edistribuzione_result("ACTION_FAILED",cJSON_IsString(message)?message->valuestring:"E-Distribuzione QueryLoadProfile failed");
    cJSON_Delete(root);
    return out;
  }
  cJSON_Delete(root);
  return NULL;
}

static int edistribuzione_request_index(struct EdistribuzioneRequest *requests,int count,const char *request_id) {
  int i;

  for(i=0;i<count;i++) {
    if(requests[i].request_id!=NULL && strcmp(requests[i].request_id,request_id)==0) return i;
  }
  return -1;
}

static void edistribuzione_free_requests(struct EdistribuzioneRequest *requests,int count) {
  int i;

  for(i=0;i<count;i++) free(requests[i].request_id);
}

static cJSON *edistribuzione_ensure_curve_page(CURL *ws) {
  const char *target,*check_expression;
  cJSON *params,*reply,*value,*code;
  int i,id;

  target="https://private.e-distribuzione.it/PortaleClienti/s/curvedicarico";
  check_expression=
    "(()=>{"
      "const target='/PortaleClienti/s/curvedicarico';"
      "if(location.pathname!==target)return {code:'WRONG_PAGE',path:location.pathname,host:location.hostname};"
      "const n=document.querySelectorAll('select.curve-date').length;"
      "if(n===4)return {code:'OK'};"
      "return {code:'LOADING',count:n};"
    "})()";

  params=cJSON_CreateObject();
  if(params==NULL) return edistribuzione_result("TEMPORARY_ERROR","out of memory");
  cJSON_AddStringToObject(params,"expression",check_expression);
  cJSON_AddBoolToObject(params,"returnByValue",1);
  reply=NULL;
  i=0;
  if(edistribuzione_cdp_call(ws,10,"Runtime.evaluate",params,&reply)==0) {
    value=cdp_eval_value(reply);
    cJSON_Delete(reply);
    if(cJSON_IsObject(value)) {
      code=cJSON_GetObjectItemCaseSensitive(value,"code");
      if(cJSON_IsString(code) && strcmp(code->valuestring,"OK")==0) {
        cJSON_Delete(value);
        return NULL;
      }
      if(cJSON_IsString(code) && strcmp(code->valuestring,"LOADING")==0) i=1;
    }
    cJSON_Delete(value);
  }

  if(!i) {
    params=cJSON_CreateObject();
    if(params==NULL) return edistribuzione_result("TEMPORARY_ERROR","out of memory");
    cJSON_AddStringToObject(params,"url",target);
    reply=NULL;
    if(edistribuzione_cdp_call(ws,11,"Page.navigate",params,&reply)!=0) return edistribuzione_result("TEMPORARY_ERROR","Chrome could not open E-Distribuzione Curve di carico");
    cJSON_Delete(reply);
  }

  id=12;
  for(i=0;i<40;i++) {
    usleep(250000);
    params=cJSON_CreateObject();
    if(params==NULL) return edistribuzione_result("TEMPORARY_ERROR","out of memory");
    cJSON_AddStringToObject(params,"expression",check_expression);
    cJSON_AddBoolToObject(params,"returnByValue",1);
    reply=NULL;
    if(edistribuzione_cdp_call(ws,id++,"Runtime.evaluate",params,&reply)!=0) continue;
    value=cdp_eval_value(reply);
    cJSON_Delete(reply);
    if(!cJSON_IsObject(value)) {
      cJSON_Delete(value);
      continue;
    }
    code=cJSON_GetObjectItemCaseSensitive(value,"code");
    if(cJSON_IsString(code) && strcmp(code->valuestring,"OK")==0) {
      cJSON_Delete(value);
      return NULL;
    }
    if(cJSON_IsString(code) && strcmp(code->valuestring,"WRONG_PAGE")==0) {
      cJSON *host;

      host=cJSON_GetObjectItemCaseSensitive(value,"host");
      if(cJSON_IsString(host) && strcmp(host->valuestring,"private.e-distribuzione.it")!=0) {
        cJSON_Delete(value);
        return edistribuzione_result("NOT_AUTHENTICATED","E-Distribuzione redirected away from the authenticated portal");
      }
    }
    cJSON_Delete(value);
  }
  return edistribuzione_result("CURVE_PAGE_UNAVAILABLE","Curve di carico did not become ready");
}

static cJSON *edistribuzione_load_profile_month(int year,int month,const char *magnitude) {
  struct EdistribuzioneRequest requests[8];
  const char *setup_template,*click_expression,*post_data,*request_id,*method_name;
  char *ws_url,*magnitude_json,*message;
  char setup_expression[8192],pod[64];
  cJSON *magnitude_item,*params,*reply,*value,*root,*rid,*error,*method,*event_params,*pod_item;
  cJSON *request,*url,*post,*finished_id,*body_result,*body,*base64,*out;
  CURL *ws;
  CURLcode curl_rc;
  int rc,n,i,index,request_count,next_body_id,body_done_count,idle_count,timeout_count,click_done;

  for(i=0;i<8;i++) {
    requests[i].request_id=NULL;
    requests[i].body_id=0;
    requests[i].finished=0;
    requests[i].body_done=0;
  }
  ws_url=NULL;
  magnitude_json=NULL;
  magnitude_item=NULL;
  params=NULL;
  reply=NULL;
  value=NULL;
  ws=NULL;
  request_count=0;
  next_body_id=100;
  body_done_count=0;
  idle_count=0;
  timeout_count=0;
  click_done=0;
  pod[0]=0;

  rc=edistribuzione_find_tab(&ws_url);
  if(rc==-2) return edistribuzione_result("NO_BROWSER","Chrome CDP is unavailable on 127.0.0.1:9222");
  if(rc==-4) return edistribuzione_result("NO_EDISTRIBUZIONE_CONTEXT","no E-Distribuzione PortaleClienti page is open");
  if(rc==-5) return edistribuzione_result("MULTIPLE_EDISTRIBUZIONE_CONTEXTS","multiple E-Distribuzione PortaleClienti pages are open");
  if(rc!=0) return edistribuzione_result("TEMPORARY_ERROR","cannot inspect the E-Distribuzione browser context");

  magnitude_item=cJSON_CreateString(magnitude);
  if(magnitude_item==NULL) {
    free(ws_url);
    return edistribuzione_result("TEMPORARY_ERROR","out of memory");
  }
  magnitude_json=cJSON_PrintUnformatted(magnitude_item);
  cJSON_Delete(magnitude_item);
  if(magnitude_json==NULL) {
    free(ws_url);
    return edistribuzione_result("TEMPORARY_ERROR","out of memory");
  }

  setup_template=
    "(()=>{"
      "const year=%d,month=%d,magnitude=%s;"
      "const dates=Array.from(document.querySelectorAll('select.curve-date'));"
      "if(dates.length!==4)return {code:'DATE_CONTROLS_UNAVAILABLE',count:dates.length};"
      "const wanted=[month,year,month,year],monthNames=['','gennaio','febbraio','marzo','aprile','maggio','giugno','luglio','agosto','settembre','ottobre','novembre','dicembre'];"
      "for(let i=0;i<4;i++){"
        "const options=Array.from(dates[i].options);"
        "const opt=options.find(o=>{const v=String(o.value||'').trim(),t=String(o.textContent||'').trim().toLowerCase();if(i===0||i===2)return parseInt(v,10)===month||t===monthNames[month];return parseInt(v,10)===year;});"
        "if(!opt)return {code:'DATE_UNAVAILABLE',index:i,value:String(wanted[i])};"
        "const actual=String(opt.value);"
        "const dc=$A.getComponent(dates[i].id)||$A.getComponent(dates[i].getAttribute('data-aura-rendered-by'));"
        "if(!dc||!dc.set||!dc.get)return {code:'DATE_AURA_COMPONENT_UNAVAILABLE',index:i,id:dates[i].id};"
        "dc.set('v.value',actual);dates[i].value=actual;"
        "if(String(dc.get('v.value'))!==actual)return {code:'DATE_AURA_SYNC_FAILED',index:i,value:String(dc.get('v.value')),expected:actual};"
      "}"
      "const node=document.querySelector('.cPED_Curva_di_carico_main[data-aura-rendered-by]');"
      "if(!node||!window.$A||!$A.getComponent)return {code:'CURVE_COMPONENT_UNAVAILABLE'};"
      "let component=$A.getComponent(node.getAttribute('data-aura-rendered-by'));"
      "for(let i=0;component&&i<12;i++){"
        "let name='';try{name=component.getDef().getDescriptor().getQualifiedName();}catch(e){}"
        "if(name==='markup://c:PED_Curva_di_carico_main')break;"
        "try{component=component.getOwner?component.getOwner():null;}catch(e){component=null;}"
      "}"
      "if(!component)return {code:'CURVE_COMPONENT_UNAVAILABLE'};"
      "const selected=String(component.get('v.eTypeVal')||'');"
      "if(!selected.includes('– '+magnitude))return {code:'MAGNITUDE_NOT_SELECTED',selected:selected,magnitude:magnitude};"
      "const pod=String(component.get('v.podVal')||'').trim();"
      "if(!pod)return {code:'NO_POD_CONTEXT'};"
      "return {code:'OK',pod:pod};"
    "})()";
  n=snprintf(setup_expression,sizeof(setup_expression),setup_template,year,month,magnitude_json);
  free(magnitude_json);
  if(n<0 || (size_t)n>=sizeof(setup_expression)) {
    free(ws_url);
    return edistribuzione_result("TEMPORARY_ERROR","E-Distribuzione browser expression is too long");
  }

  ws=curl_easy_init();
  if(ws==NULL) {
    free(ws_url);
    return edistribuzione_result("TEMPORARY_ERROR","cannot initialize Chrome CDP connection");
  }
  curl_easy_setopt(ws,CURLOPT_URL,ws_url);
  curl_easy_setopt(ws,CURLOPT_CONNECT_ONLY,2L);
  curl_easy_setopt(ws,CURLOPT_CONNECTTIMEOUT_MS,CHROME_HTTP_TIMEOUT_MS);
  curl_easy_setopt(ws,CURLOPT_NOSIGNAL,1L);
  curl_rc=curl_easy_perform(ws);
  free(ws_url);
  if(curl_rc!=CURLE_OK) {
    curl_easy_cleanup(ws);
    return edistribuzione_result("TEMPORARY_ERROR","cannot connect to the E-Distribuzione Chrome page through CDP");
  }

  out=edistribuzione_ensure_curve_page(ws);
  if(out!=NULL) {
    curl_easy_cleanup(ws);
    return out;
  }

  params=cJSON_CreateObject();
  if(params==NULL) {
    curl_easy_cleanup(ws);
    return edistribuzione_result("TEMPORARY_ERROR","out of memory");
  }
  if(edistribuzione_cdp_call(ws,1,"Network.enable",params,&reply)!=0) {
    curl_easy_cleanup(ws);
    return edistribuzione_result("TEMPORARY_ERROR","Chrome could not enable E-Distribuzione network observation");
  }
  cJSON_Delete(reply);
  reply=NULL;

  params=cJSON_CreateObject();
  if(params==NULL) {
    curl_easy_cleanup(ws);
    return edistribuzione_result("TEMPORARY_ERROR","out of memory");
  }
  cJSON_AddStringToObject(params,"expression",setup_expression);
  cJSON_AddBoolToObject(params,"returnByValue",1);
  if(edistribuzione_cdp_call(ws,2,"Runtime.evaluate",params,&reply)!=0) {
    curl_easy_cleanup(ws);
    return edistribuzione_result("TEMPORARY_ERROR","Chrome could not set the E-Distribuzione month controls");
  }
  value=cdp_eval_value(reply);
  cJSON_Delete(reply);
  reply=NULL;
  if(!cJSON_IsObject(value)) {
    cJSON_Delete(value);
    curl_easy_cleanup(ws);
    return edistribuzione_result("TEMPORARY_ERROR","Chrome returned an invalid month-control result");
  }
  error=cJSON_GetObjectItemCaseSensitive(value,"code");
  if(!cJSON_IsString(error) || strcmp(error->valuestring,"OK")!=0) {
    out=cJSON_Duplicate(value,1);
    cJSON_Delete(value);
    curl_easy_cleanup(ws);
    return out!=NULL?out:edistribuzione_result("TEMPORARY_ERROR","out of memory");
  }
  pod_item=cJSON_GetObjectItemCaseSensitive(value,"pod");
  if(!cJSON_IsString(pod_item) || pod_item->valuestring==NULL || pod_item->valuestring[0]==0 ||
    strlen(pod_item->valuestring)>=sizeof(pod)) {
    cJSON_Delete(value);
    curl_easy_cleanup(ws);
    return edistribuzione_result("NO_POD_CONTEXT","load-profile POD is unavailable");
  }
  strcpy(pod,pod_item->valuestring);
  cJSON_Delete(value);

  click_expression=
    "(()=>{const b=Array.from(document.querySelectorAll('button')).find(x=>x.textContent.replace(/\\s+/g,' ').trim().includes('Modifica periodo'));"
    "if(!b)return {code:'MODIFY_PERIOD_UNAVAILABLE'};b.click();return {code:'OK'};})()";
  params=cJSON_CreateObject();
  if(params==NULL) {
    curl_easy_cleanup(ws);
    return edistribuzione_result("TEMPORARY_ERROR","out of memory");
  }
  cJSON_AddStringToObject(params,"expression",click_expression);
  cJSON_AddBoolToObject(params,"returnByValue",1);
  if(edistribuzione_cdp_send(ws,3,"Runtime.evaluate",params)!=0) {
    curl_easy_cleanup(ws);
    return edistribuzione_result("TEMPORARY_ERROR","Chrome could not activate Modifica periodo");
  }

  for(;;) {
    message=NULL;
    rc=edistribuzione_recv_text(ws,&message,3000);
    if(rc==-2) {
      timeout_count++;
      if(request_count==0) {
        if(timeout_count>=3) break;
      } else {
        index=0;
        for(i=0;i<request_count;i++) if(!requests[i].body_done) index++;
        if(index>0) {
          if(timeout_count>=20) break;
        } else {
          idle_count++;
          if(idle_count>=2) break;
        }
      }
      continue;
    }
    if(rc!=0) break;
    timeout_count=0;
    root=cJSON_Parse(message!=NULL?message:"");
    free(message);
    if(root==NULL) continue;

    rid=cJSON_GetObjectItemCaseSensitive(root,"id");
    if(cJSON_IsNumber(rid) && rid->valueint==3) {
      error=cJSON_GetObjectItemCaseSensitive(root,"error");
      if(error!=NULL) {
        cJSON_Delete(root);
        edistribuzione_free_requests(requests,request_count);
        curl_easy_cleanup(ws);
        return edistribuzione_result("TEMPORARY_ERROR","Chrome could not activate Modifica periodo");
      }
      value=cdp_eval_value(root);
      if(cJSON_IsObject(value)) {
        error=cJSON_GetObjectItemCaseSensitive(value,"code");
        if(!cJSON_IsString(error) || strcmp(error->valuestring,"OK")!=0) {
          out=cJSON_Duplicate(value,1);
          cJSON_Delete(value);
          cJSON_Delete(root);
          edistribuzione_free_requests(requests,request_count);
          curl_easy_cleanup(ws);
          return out!=NULL?out:edistribuzione_result("TEMPORARY_ERROR","out of memory");
        }
      }
      cJSON_Delete(value);
      click_done=1;
      cJSON_Delete(root);
      continue;
    }

    if(cJSON_IsNumber(rid) && rid->valueint>=100 && rid->valueint<108) {
      index=-1;
      for(i=0;i<request_count;i++) if(requests[i].body_id==rid->valueint) index=i;
      if(index>=0) {
        requests[index].body_done=1;
        body_done_count++;
        error=cJSON_GetObjectItemCaseSensitive(root,"error");
        body_result=cJSON_GetObjectItemCaseSensitive(root,"result");
        body=cJSON_IsObject(body_result)?cJSON_GetObjectItemCaseSensitive(body_result,"body"):NULL;
        base64=cJSON_IsObject(body_result)?cJSON_GetObjectItemCaseSensitive(body_result,"base64Encoded"):NULL;
        if(error==NULL && cJSON_IsString(body) && !cJSON_IsTrue(base64)) {
          out=edistribuzione_parse_aura_body(body->valuestring,year,month,magnitude);
          if(out!=NULL) {
            cJSON_AddStringToObject(out,"pod",pod);
            cJSON_Delete(root);
            edistribuzione_free_requests(requests,request_count);
            curl_easy_cleanup(ws);
            return out;
          }
        }
      }
      cJSON_Delete(root);
      continue;
    }

    method=cJSON_GetObjectItemCaseSensitive(root,"method");
    method_name=cJSON_IsString(method)?method->valuestring:"";
    event_params=cJSON_GetObjectItemCaseSensitive(root,"params");
    if(strcmp(method_name,"Network.requestWillBeSent")==0 && cJSON_IsObject(event_params)) {
      request=cJSON_GetObjectItemCaseSensitive(event_params,"request");
      finished_id=cJSON_GetObjectItemCaseSensitive(event_params,"requestId");
      url=cJSON_IsObject(request)?cJSON_GetObjectItemCaseSensitive(request,"url"):NULL;
      post=cJSON_IsObject(request)?cJSON_GetObjectItemCaseSensitive(request,"postData"):NULL;
      post_data=cJSON_IsString(post)?post->valuestring:"";
      if(cJSON_IsString(finished_id) && cJSON_IsString(url) && strstr(url->valuestring,"/s/sfsites/aura")!=NULL &&
        (strstr(post_data,"QueryLoadProfile")!=NULL || strstr(post_data,"processQueryLoadProfile")!=NULL)) {
        request_id=finished_id->valuestring;
        if(edistribuzione_request_index(requests,request_count,request_id)<0 && request_count<8) {
          requests[request_count].request_id=strdup(request_id);
          if(requests[request_count].request_id==NULL) {
            cJSON_Delete(root);
            edistribuzione_free_requests(requests,request_count);
            curl_easy_cleanup(ws);
            return edistribuzione_result("TEMPORARY_ERROR","out of memory");
          }
          request_count++;
        }
        idle_count=0;
      }
    } else if(strcmp(method_name,"Network.loadingFinished")==0 && cJSON_IsObject(event_params)) {
      finished_id=cJSON_GetObjectItemCaseSensitive(event_params,"requestId");
      if(cJSON_IsString(finished_id)) {
        index=edistribuzione_request_index(requests,request_count,finished_id->valuestring);
        if(index>=0 && requests[index].body_id==0) {
          requests[index].finished=1;
          requests[index].body_id=next_body_id++;
          params=cJSON_CreateObject();
          if(params==NULL) {
            cJSON_Delete(root);
            edistribuzione_free_requests(requests,request_count);
            curl_easy_cleanup(ws);
            return edistribuzione_result("TEMPORARY_ERROR","out of memory");
          }
          cJSON_AddStringToObject(params,"requestId",finished_id->valuestring);
          if(edistribuzione_cdp_send(ws,requests[index].body_id,"Network.getResponseBody",params)!=0) {
            cJSON_Delete(root);
            edistribuzione_free_requests(requests,request_count);
            curl_easy_cleanup(ws);
            return edistribuzione_result("TEMPORARY_ERROR","Chrome could not read the E-Distribuzione response");
          }
          idle_count=0;
        }
      }
    }
    cJSON_Delete(root);
    if(click_done && body_done_count>0) {
      index=0;
      for(i=0;i<request_count;i++) if(!requests[i].body_done) index++;
      if(index==0) idle_count=0;
    }
  }

  edistribuzione_free_requests(requests,request_count);
  curl_easy_cleanup(ws);
  if(request_count==0) return edistribuzione_result("ACTION_NOT_OBSERVED","Modifica periodo did not generate QueryLoadProfile");
  if(body_done_count==0) return edistribuzione_result("TEMPORARY_ERROR","QueryLoadProfile did not complete");
  return edistribuzione_result("INVALID_RESPONSE","QueryLoadProfile completed without quarter-hour data");
}

static cJSON *handle_edistribuzione(const char *action,cJSON *payload,char **error_out) {
  cJSON *item;
  const char *magnitude;
  int year,month;

  if(strcmp(action,"load_profile.month")!=0) {
    *error_out=strdup("unsupported edistribuzione action");
    return NULL;
  }
  if(!cJSON_IsObject(payload)) return edistribuzione_result("INVALID_REQUEST","edistribuzione payload must be an object");
  item=cJSON_GetObjectItemCaseSensitive(payload,"year");
  if(!cJSON_IsNumber(item) || item->valuedouble!=(double)item->valueint || item->valueint<2000 || item->valueint>2100)
    return edistribuzione_result("INVALID_REQUEST","year must be an integer between 2000 and 2100");
  year=item->valueint;
  item=cJSON_GetObjectItemCaseSensitive(payload,"month");
  if(!cJSON_IsNumber(item) || item->valuedouble!=(double)item->valueint || item->valueint<1 || item->valueint>12)
    return edistribuzione_result("INVALID_REQUEST","month must be an integer between 1 and 12");
  month=item->valueint;
  item=cJSON_GetObjectItemCaseSensitive(payload,"magnitude");
  magnitude=cJSON_IsString(item)?item->valuestring:"A+";
  if(!valid_edistribuzione_magnitude(magnitude))
    return edistribuzione_result("INVALID_REQUEST","magnitude must be A+, A-, RI+, RC+, RI- or RC-");
  return edistribuzione_load_profile_month(year,month,magnitude);
}

static cJSON *qrz_result(const char *code,const char *detail) {
  cJSON *result;

  result=cJSON_CreateObject();
  if(result==NULL) return NULL;
  cJSON_AddStringToObject(result,"code",code);
  if(detail!=NULL && detail[0]!=0) cJSON_AddStringToObject(result,"detail",detail);
  return result;
}

static int valid_qrz_call(const char *call) {
  size_t i,n;

  if(call==NULL) return 0;
  n=strlen(call);
  if(n<1 || n>QRZ_CALL_MAX) return 0;
  for(i=0;i<n;i++) {
    if(iscntrl((unsigned char)call[i]) || isspace((unsigned char)call[i])) return 0;
  }
  return 1;
}

static cJSON *qrz_webcontact_add(const char *callsign,const char *mycall) {
  const char *template;
  char *ws_url,*call_json,*mycall_json;
  char expression[QRZ_EXPRESSION_MAX];
  cJSON *call_item,*mycall_item,*params,*reply,*value;
  CURL *ws;
  CURLcode curl_rc;
  int rc,n;

  if(!valid_qrz_call(callsign) || !valid_qrz_call(mycall))
    return qrz_result("INVALID_REQUEST","callsign and mycall must be non-empty callsigns of at most 19 bytes");
  rc=find_qrz_tab(&ws_url);
  if(rc==-2) return qrz_result("NO_BROWSER","Chrome CDP is unavailable on 127.0.0.1:9222");
  if(rc==-4) return qrz_result("NO_QRZ_CONTEXT","no QRZ page is open in the dedicated Chrome");
  if(rc!=0) return qrz_result("TEMPORARY_ERROR","cannot inspect the QRZ browser context");
  call_item=cJSON_CreateString(callsign);
  mycall_item=cJSON_CreateString(mycall);
  if(call_item==NULL || mycall_item==NULL) {
    cJSON_Delete(call_item);
    cJSON_Delete(mycall_item);
    free(ws_url);
    return qrz_result("TEMPORARY_ERROR","out of memory");
  }
  call_json=cJSON_PrintUnformatted(call_item);
  mycall_json=cJSON_PrintUnformatted(mycall_item);
  cJSON_Delete(call_item);
  cJSON_Delete(mycall_item);
  if(call_json==NULL || mycall_json==NULL) {
    free(call_json);
    free(mycall_json);
    free(ws_url);
    return qrz_result("TEMPORARY_ERROR","out of memory");
  }
  template=
    "(async()=>{"
    "const callsign=%s,mycall=%s;"
    "const out=(code,detail)=>({code:code,detail:detail||''});"
    "const doc=h=>new DOMParser().parseFromString(h,'text/html');"
    "const login=(r,h)=>{const u=(r.url||'').toLowerCase(),d=doc(h);"
      "if(u.includes('/login'))return true;"
      "const p=d.querySelector('input[type=\\\"password\\\"]');"
      "return !!p&&/(login|sign[ -]?in)/i.test((d.body&&d.body.innerText)||'');};"
    "const get=async u=>{const r=await fetch(u,{credentials:'include',cache:'no-store',redirect:'follow'});"
      "const h=await r.text();return {r:r,h:h};};"
    "const has=(h,base)=>{const d=doc(h),want=mycall.toUpperCase();"
      "return [...d.querySelectorAll('a[href]')].some(a=>{try{const u=new URL(a.getAttribute('href'),base);"
      "if(u.hostname!=='www.qrz.com'&&u.hostname!=='qrz.com')return false;"
      "if(!u.pathname.toLowerCase().startsWith('/db/'))return false;"
      "let c=decodeURIComponent(u.pathname.slice(4));if(c.endsWith('/'))c=c.slice(0,-1);"
      "return c.toUpperCase()===want;}catch(e){return false;}});};"
    "try{"
      "const p=await get('https://www.qrz.com/db/'+encodeURIComponent(callsign));"
      "if(login(p.r,p.h))return out('NOT_AUTHENTICATED','QRZ redirected the profile request to login');"
      "if(!p.r.ok)return out('TEMPORARY_ERROR','QRZ profile request failed with HTTP '+p.r.status);"
      "const pd=doc(p.h),title=(pd.title||'').trim();"
      "if(title==='QRZ Callsign Database Search by QRZ Ham Radio')return out('PROFILE_UNAVAILABLE','QRZ callsign profile is unavailable');"
      "const m=p.h.match(/var\\s+wc_summary\\s*=\\s*[\\\"']([^\\\"']+)[\\\"']/);"
      "if(!m)return out('WEB_CONTACTS_UNAVAILABLE','QRZ Web Contacts summary is not available for the resolved profile');"
      "const su=new URL(m[1],p.r.url).href;"
      "const s=await get(su);"
      "if(login(s.r,s.h))return out('NOT_AUTHENTICATED','QRZ redirected the Web Contacts request to login');"
      "if(!s.r.ok)return out('TEMPORARY_ERROR','QRZ Web Contacts request failed with HTTP '+s.r.status);"
      "if(has(s.h,s.r.url))return out('ALREADY_PRESENT','mycall is already present in the target Web Contacts');"
      "const sd=doc(s.h),uid=sd.querySelector('input[name=\\\"wc_userid\\\"]');"
      "if(!uid||!uid.value)return out('WEB_CONTACTS_UNAVAILABLE','QRZ authenticated Web Contact action is not available for the resolved profile');"
      "const body=new URLSearchParams();body.set('webcon','1');body.set('wc_userid',uid.value);"
      "let a;try{a=await fetch(p.r.url,{method:'POST',credentials:'include',cache:'no-store',redirect:'follow',"
        "headers:{'Content-Type':'application/x-www-form-urlencoded'},body:body.toString()});}"
      "catch(e){return out('TEMPORARY_ERROR','QRZ add action ended with an ambiguous browser or network error');}"
      "const ah=await a.text();"
      "if(login(a,ah))return out('NOT_AUTHENTICATED','QRZ redirected the add action to login');"
      "if(!a.ok)return out('ACTION_FAILED','QRZ add action failed with HTTP '+a.status);"
      "let v;try{v=await get(su);}catch(e){return out('TEMPORARY_ERROR','QRZ final verification could not be read');}"
      "if(login(v.r,v.h))return out('NOT_AUTHENTICATED','QRZ redirected final verification to login');"
      "if(!v.r.ok)return out('TEMPORARY_ERROR','QRZ final verification failed with HTTP '+v.r.status);"
      "if(has(v.h,v.r.url))return out('ADDED','mycall was added and positively verified in fresh Web Contacts state');"
      "return out('NOT_CONFIRMED','QRZ add action completed but final Web Contacts state does not contain mycall');"
    "}catch(e){return out('TEMPORARY_ERROR','unexpected QRZ browser execution error');}"
    "})()";
  n=snprintf(expression,sizeof(expression),template,call_json,mycall_json);
  free(call_json);
  free(mycall_json);
  if(n<0 || (size_t)n>=sizeof(expression)) {
    free(ws_url);
    return qrz_result("INVALID_REQUEST","QRZ browser expression is too long");
  }
  ws=curl_easy_init();
  if(ws==NULL) {
    free(ws_url);
    return qrz_result("TEMPORARY_ERROR","cannot initialize Chrome CDP connection");
  }
  curl_easy_setopt(ws,CURLOPT_URL,ws_url);
  curl_easy_setopt(ws,CURLOPT_CONNECT_ONLY,2L);
  curl_easy_setopt(ws,CURLOPT_CONNECTTIMEOUT_MS,CHROME_HTTP_TIMEOUT_MS);
  curl_easy_setopt(ws,CURLOPT_NOSIGNAL,1L);
  curl_rc=curl_easy_perform(ws);
  free(ws_url);
  if(curl_rc!=CURLE_OK) {
    curl_easy_cleanup(ws);
    return qrz_result("TEMPORARY_ERROR","cannot connect to the QRZ Chrome tab through CDP");
  }
  params=cJSON_CreateObject();
  if(params==NULL) {
    curl_easy_cleanup(ws);
    return qrz_result("TEMPORARY_ERROR","out of memory");
  }
  cJSON_AddStringToObject(params,"expression",expression);
  cJSON_AddBoolToObject(params,"returnByValue",1);
  cJSON_AddBoolToObject(params,"awaitPromise",1);
  reply=NULL;
  if(cdp_call(ws,1,"Runtime.evaluate",params,&reply)!=0) {
    curl_easy_cleanup(ws);
    return qrz_result("TEMPORARY_ERROR","Chrome could not execute the QRZ operation");
  }
  value=cdp_eval_value(reply);
  cJSON_Delete(reply);
  curl_easy_cleanup(ws);
  if(!cJSON_IsObject(value)) {
    cJSON_Delete(value);
    return qrz_result("TEMPORARY_ERROR","Chrome returned an invalid QRZ operation result");
  }
  return value;
}

static cJSON *handle_qrz(const char *action,cJSON *payload,char **error_out) {
  cJSON *item;
  const char *callsign,*mycall;

  (void)error_out;
  if(strcmp(action,"webcontact.add")!=0) {
    *error_out=strdup("unsupported qrz action");
    return NULL;
  }
  if(!cJSON_IsObject(payload)) return qrz_result("INVALID_REQUEST","qrz payload must be an object");
  item=cJSON_GetObjectItemCaseSensitive(payload,"callsign");
  callsign=cJSON_IsString(item)?item->valuestring:NULL;
  item=cJSON_GetObjectItemCaseSensitive(payload,"mycall");
  mycall=cJSON_IsString(item)?item->valuestring:NULL;
  return qrz_webcontact_add(callsign,mycall);
}

static cJSON *handle_test(const char *action,cJSON *payload,char **error_out) {
  cJSON *result;

  if(strcmp(action,"echo")!=0) {
    *error_out=strdup("unsupported test action");
    return NULL;
  }
  result=payload!=NULL?cJSON_Duplicate(payload,1):cJSON_CreateNull();
  if(result==NULL) *error_out=strdup("out of memory");
  return result;
}

static cJSON *handle_browser(const char *action,cJSON *payload,char **error_out) {
  cJSON *item;
  const char *title;

  if(strcmp(action,"read_tab")!=0) {
    *error_out=strdup("unsupported browser action");
    return NULL;
  }
  if(!cJSON_IsObject(payload)) {
    *error_out=strdup("browser payload must be an object");
    return NULL;
  }
  item=cJSON_GetObjectItemCaseSensitive(payload,"title");
  if(!cJSON_IsString(item) || item->valuestring==NULL || item->valuestring[0]==0) {
    *error_out=strdup("browser read_tab requires title");
    return NULL;
  }
  title=item->valuestring;
  return browser_read_tab(title,error_out);
}

static cJSON *dispatch_request(const char *module,const char *action,cJSON *payload,char **error_out) {
  if(strcmp(module,"test")==0) return handle_test(action,payload,error_out);
  if(strcmp(module,"browser")==0) return handle_browser(action,payload,error_out);
  if(strcmp(module,"edistribuzione")==0) return handle_edistribuzione(action,payload,error_out);
  if(strcmp(module,"qrz")==0) return handle_qrz(action,payload,error_out);
  *error_out=strdup("unsupported module");
  return NULL;
}

static int send_result(CURL *curl,const char *request_id,cJSON *result,const char *error_text) {
  cJSON *root;
  char *body,*reply;
  long http_code;
  unsigned int retry_delay;
  int rc;

  root=cJSON_CreateObject();
  if(root==NULL) return -1;
  cJSON_AddStringToObject(root,"request_id",request_id);
  if(error_text==NULL) {
    cJSON_AddStringToObject(root,"status","ok");
    cJSON_AddItemToObject(root,"result",result);
  } else {
    cJSON_AddStringToObject(root,"status","error");
    cJSON_AddStringToObject(root,"error",error_text);
    cJSON_Delete(result);
  }
  body=cJSON_PrintUnformatted(root);
  cJSON_Delete(root);
  if(body==NULL) return -1;
  retry_delay=2;
  for(;;) {
    reply=NULL;
    rc=http_agent_request(curl,"result",body,HTTP_RESULT_TIMEOUT,&reply,&http_code);
    if(rc==0 && http_code==200) {
      log_line(stdout,"AGENT","result delivered request=%s",request_id);
      free(reply);
      free(body);
      return 0;
    }
    if(rc==0 && http_code<500) {
      log_line(stderr,"ERROR","agent result rejected request=%s HTTP=%ld detail=%s",request_id,http_code,reply!=NULL?reply:"");
      free(reply);
      free(body);
      return -1;
    }
    if(rc==0)
      log_line(stderr,"ERROR","agent result temporary HTTP=%ld request=%s retry=%us",http_code,request_id,retry_delay);
    else
      log_line(stderr,"ERROR","agent result network failure request=%s retry=%us",request_id,retry_delay);
    free(reply);
    sleep(retry_delay);
    if(retry_delay<10) {
      retry_delay*=2;
      if(retry_delay>10) retry_delay=10;
    }
  }
}

static int valid_conversation_id(const char *s) {
  size_t i,n;

  if(s==NULL) return 0;
  n=strlen(s);
  if(n<8 || n>CONVERSATION_MAX) return 0;
  for(i=0;i<n;i++) {
    if(!isalnum((unsigned char)s[i]) && s[i]!='-' && s[i]!='_') return 0;
  }
  return 1;
}

static cJSON *make_jobs_request(const char *chat) {
  cJSON *root,*params,*meta,*info,*args;

  root=cJSON_CreateObject();
  if(root==NULL) return NULL;
  cJSON_AddStringToObject(root,"jsonrpc","2.0");
  cJSON_AddNumberToObject(root,"id",1);
  cJSON_AddStringToObject(root,"method","tools/call");
  params=cJSON_AddObjectToObject(root,"params");
  meta=cJSON_AddObjectToObject(params,"_meta");
  cJSON_AddStringToObject(meta,"io.modelcontextprotocol/protocolVersion",MCP_PROTOCOL);
  cJSON_AddObjectToObject(meta,"io.modelcontextprotocol/clientCapabilities");
  info=cJSON_AddObjectToObject(meta,"io.modelcontextprotocol/clientInfo");
  cJSON_AddStringToObject(info,"name",MCP_CLIENT_NAME);
  cJSON_AddStringToObject(info,"version",MCP_AGENT_VERSION);
  cJSON_AddStringToObject(params,"name","jobs");
  args=cJSON_AddObjectToObject(params,"arguments");
  cJSON_AddStringToObject(args,"chat",chat);
  cJSON_AddNumberToObject(args,"limit",1000);
  return root;
}

static cJSON *query_jobs(CURL *curl,const char *chat) {
  char auth[640];
  struct Buffer body;
  struct curl_slist *headers;
  CURLcode curl_rc;
  cJSON *req,*reply,*result,*content;
  char *json;
  long http_code;

  req=make_jobs_request(chat);
  if(req==NULL) return NULL;
  json=cJSON_PrintUnformatted(req);
  cJSON_Delete(req);
  if(json==NULL) return NULL;
  if(snprintf(auth,sizeof(auth),"Authorization: Bearer %s",g_mcp_token)>=(int)sizeof(auth)) {
    free(json);
    return NULL;
  }
  body.data=NULL;
  body.len=0;
  headers=NULL;
  headers=curl_slist_append(headers,"Content-Type: application/json");
  headers=curl_slist_append(headers,"MCP-Protocol-Version: " MCP_PROTOCOL);
  headers=curl_slist_append(headers,"Mcp-Method: tools/call");
  headers=curl_slist_append(headers,"Mcp-Name: jobs");
  headers=curl_slist_append(headers,auth);
  curl_easy_reset(curl);
  curl_easy_setopt(curl,CURLOPT_URL,agent_url());
  curl_easy_setopt(curl,CURLOPT_POST,1L);
  curl_easy_setopt(curl,CURLOPT_HTTPHEADER,headers);
  curl_easy_setopt(curl,CURLOPT_POSTFIELDS,json);
  curl_easy_setopt(curl,CURLOPT_POSTFIELDSIZE,(long)strlen(json));
  curl_easy_setopt(curl,CURLOPT_WRITEFUNCTION,http_write);
  curl_easy_setopt(curl,CURLOPT_WRITEDATA,&body);
  curl_easy_setopt(curl,CURLOPT_CONNECTTIMEOUT_MS,10000L);
  curl_easy_setopt(curl,CURLOPT_TIMEOUT_MS,10000L);
  curl_easy_setopt(curl,CURLOPT_NOSIGNAL,1L);
  curl_easy_setopt(curl,CURLOPT_TCP_KEEPALIVE,1L);
  curl_rc=curl_easy_perform(curl);
  http_code=0;
  curl_easy_getinfo(curl,CURLINFO_RESPONSE_CODE,&http_code);
  curl_slist_free_all(headers);
  free(json);
  if(curl_rc!=CURLE_OK || http_code!=200) {
    if(curl_rc!=CURLE_OK)
      log_line(stderr,"ERROR","watch jobs HTTP chat=%s error=%s",chat,curl_easy_strerror(curl_rc));
    else
      log_line(stderr,"ERROR","watch jobs HTTP chat=%s status=%ld",chat,http_code);
    free(body.data);
    return NULL;
  }
  reply=cJSON_Parse(body.data!=NULL?body.data:"");
  free(body.data);
  if(!cJSON_IsObject(reply)) {
    cJSON_Delete(reply);
    return NULL;
  }
  result=cJSON_GetObjectItemCaseSensitive(reply,"result");
  content=cJSON_IsObject(result)?cJSON_GetObjectItemCaseSensitive(result,"structuredContent"):NULL;
  if(!cJSON_IsArray(content)) {
    cJSON_Delete(reply);
    return NULL;
  }
  cJSON_DetachItemViaPointer(result,content);
  cJSON_Delete(reply);
  return content;
}

static int extract_chat(const char *title,char *out,size_t out_size,int *number) {
  const char *p,*end;
  char *number_end;
  long value;
  size_t n;

  if(title==NULL || number==NULL || out_size<2) return -1;
  p=title;
  for(;*p!=0 && isspace((unsigned char)*p);p++);
  n=0;
  for(;p[n]!=0 && !isspace((unsigned char)p[n]);n++);
  if(n==0 || n>=out_size) return -1;
  memcpy(out,p,n);
  out[n]=0;
  if(!valid_name(out,CHAT_MAX)) return -1;
  p+=n;
  for(;*p!=0 && isspace((unsigned char)*p);p++);
  if(*p==0) {
    *number=0;
    return 0;
  }
  value=strtol(p,&number_end,10);
  if(number_end==p || value<0 || value>INT_MAX) return -1;
  end=number_end;
  for(;*end!=0 && isspace((unsigned char)*end);end++);
  if(*end!=0) return -1;
  *number=(int)value;
  return 0;
}

static int extract_conversation_id(const char *url,char *out,size_t out_size) {
  const char *p;
  size_t n;

  if(url==NULL || out==NULL || out_size<2) return -1;
  if(strstr(url,"chatgpt.com/")==NULL && strstr(url,"chat.openai.com/")==NULL) return -1;
  p=strstr(url,"/c/");
  if(p==NULL) return -1;
  p+=3;
  n=0;
  for(;p[n]!=0 && p[n]!='?' && p[n]!='#' && p[n]!='/';n++);
  if(n==0 || n>=out_size) return -1;
  memcpy(out,p,n);
  out[n]=0;
  return valid_conversation_id(out)?0:-1;
}

static int add_tab(struct TabInfo **tabs,size_t *count,size_t *cap,const char *chat,
    const char *conversation_id,const char *title,int number) {
  struct TabInfo *p;
  size_t i,new_cap;

  if(strlen(title)>=WATCH_TITLE_MAX) return -1;
  for(i=0;i<*count;i++) {
    if(strcmp((*tabs)[i].chat,chat)!=0) continue;
    if(number>(*tabs)[i].number) {
      snprintf((*tabs)[i].conversation_id,sizeof((*tabs)[i].conversation_id),"%s",conversation_id);
      snprintf((*tabs)[i].title,sizeof((*tabs)[i].title),"%s",title);
      (*tabs)[i].number=number;
      (*tabs)[i].ambiguous=0;
    } else if(number==(*tabs)[i].number && strcmp((*tabs)[i].conversation_id,conversation_id)!=0)
      (*tabs)[i].ambiguous=1;
    return 0;
  }
  if(*count==*cap) {
    new_cap=*cap!=0?*cap*2:8;
    p=(struct TabInfo *)realloc(*tabs,new_cap*sizeof(**tabs));
    if(p==NULL) return -1;
    *tabs=p;
    *cap=new_cap;
  }
  snprintf((*tabs)[*count].chat,sizeof((*tabs)[*count].chat),"%s",chat);
  snprintf((*tabs)[*count].conversation_id,sizeof((*tabs)[*count].conversation_id),"%s",conversation_id);
  snprintf((*tabs)[*count].title,sizeof((*tabs)[*count].title),"%s",title);
  (*tabs)[*count].number=number;
  (*tabs)[*count].ambiguous=0;
  (*count)++;
  return 0;
}

static int discover_tabs(struct TabInfo **tabs,size_t *count) {
  struct Buffer body;
  cJSON *root,*item,*type,*title,*url,*ws;
  char endpoint[256],chat[CHAT_MAX+1],conversation_id[CONVERSATION_MAX+1];
  size_t cap;
  int i,n,number;

  *tabs=NULL;
  *count=0;
  cap=0;
  if(snprintf(endpoint,sizeof(endpoint),"%s/json/list",chrome_host())>=(int)sizeof(endpoint)) return -1;
  if(chrome_get(endpoint,&body)!=0) return -1;
  root=cJSON_Parse(body.data!=NULL?body.data:"");
  free(body.data);
  if(!cJSON_IsArray(root)) {
    cJSON_Delete(root);
    return -1;
  }
  n=cJSON_GetArraySize(root);
  for(i=0;i<n;i++) {
    item=cJSON_GetArrayItem(root,i);
    type=cJSON_GetObjectItemCaseSensitive(item,"type");
    title=cJSON_GetObjectItemCaseSensitive(item,"title");
    url=cJSON_GetObjectItemCaseSensitive(item,"url");
    ws=cJSON_GetObjectItemCaseSensitive(item,"webSocketDebuggerUrl");
    if(!cJSON_IsString(type) || strcmp(type->valuestring,"page")!=0) continue;
    if(!cJSON_IsString(title) || !cJSON_IsString(url) || !cJSON_IsString(ws)) continue;
    if(extract_chat(title->valuestring,chat,sizeof(chat),&number)!=0) continue;
    if(extract_conversation_id(url->valuestring,conversation_id,sizeof(conversation_id))!=0) continue;
    if(add_tab(tabs,count,&cap,chat,conversation_id,title->valuestring,number)!=0) {
      free(*tabs);
      *tabs=NULL;
      *count=0;
      cJSON_Delete(root);
      return -1;
    }
  }
  cJSON_Delete(root);
  return 0;
}

static int watch_candidate(cJSON *item,const char *conversation_id) {
  cJSON *type,*url,*ws;
  char pattern[160];

  type=cJSON_GetObjectItemCaseSensitive(item,"type");
  url=cJSON_GetObjectItemCaseSensitive(item,"url");
  ws=cJSON_GetObjectItemCaseSensitive(item,"webSocketDebuggerUrl");
  if(!cJSON_IsString(type) || strcmp(type->valuestring,"page")!=0) return 0;
  if(!cJSON_IsString(url) || !cJSON_IsString(ws)) return 0;
  if(snprintf(pattern,sizeof(pattern),"/c/%s",conversation_id)>=(int)sizeof(pattern)) return 0;
  return strstr(url->valuestring,pattern)!=NULL;
}

static int watch_find_target(const char *conversation_id,char **ws_url,char **title) {
  struct Buffer body;
  cJSON *root,*item,*cjs_title,*cjs_ws;
  char endpoint[256];
  int i,n,count;

  *ws_url=NULL;
  *title=NULL;
  if(snprintf(endpoint,sizeof(endpoint),"%s/json/list",chrome_host())>=(int)sizeof(endpoint)) return -1;
  if(chrome_get(endpoint,&body)!=0) return -1;
  root=cJSON_Parse(body.data!=NULL?body.data:"");
  free(body.data);
  if(!cJSON_IsArray(root)) {
    cJSON_Delete(root);
    return -1;
  }
  n=cJSON_GetArraySize(root);
  count=0;
  for(i=0;i<n;i++) {
    item=cJSON_GetArrayItem(root,i);
    if(watch_candidate(item,conversation_id)) count++;
  }
  if(count!=1) {
    cJSON_Delete(root);
    return -1;
  }
  for(i=0;i<n;i++) {
    item=cJSON_GetArrayItem(root,i);
    if(!watch_candidate(item,conversation_id)) continue;
    cjs_title=cJSON_GetObjectItemCaseSensitive(item,"title");
    cjs_ws=cJSON_GetObjectItemCaseSensitive(item,"webSocketDebuggerUrl");
    if(!cJSON_IsString(cjs_title) || !cJSON_IsString(cjs_ws)) break;
    *ws_url=strdup(cjs_ws->valuestring);
    *title=strdup(cjs_title->valuestring);
    cJSON_Delete(root);
    if(*ws_url==NULL || *title==NULL) {
      free(*ws_url);
      free(*title);
      *ws_url=NULL;
      *title=NULL;
      return -1;
    }
    return 0;
  }
  cJSON_Delete(root);
  return -1;
}

static const char *watch_eval_string(cJSON *reply) {
  cJSON *value;

  value=cdp_eval_value(reply);
  return cJSON_IsString(value)?value->valuestring:NULL;
}

static int send_check(const struct ChatState *state) {
  const char *expr,*value;
  cJSON *params,*reply;
  char *ws_url,*title;
  CURL *ws;
  CURLcode curl_rc;
  int id,result;

  ws_url=NULL;
  title=NULL;
  ws=NULL;
  result=-1;
  if(watch_find_target(state->conversation_id,&ws_url,&title)!=0) {
    log_line(stderr,"ERROR","watch target disappeared chat=%s conversation=%s",state->chat,state->conversation_id);
    goto done;
  }
  ws=curl_easy_init();
  if(ws==NULL) goto done;
  curl_easy_setopt(ws,CURLOPT_URL,ws_url);
  curl_easy_setopt(ws,CURLOPT_CONNECT_ONLY,2L);
  curl_easy_setopt(ws,CURLOPT_CONNECTTIMEOUT_MS,3000L);
  curl_easy_setopt(ws,CURLOPT_NOSIGNAL,1L);
  curl_rc=curl_easy_perform(ws);
  if(curl_rc!=CURLE_OK) {
    log_line(stderr,"ERROR","watch WebSocket connect chat=%s error=%s",state->chat,curl_easy_strerror(curl_rc));
    goto done;
  }
  id=1;
  expr="(() => {"
       "const visible=e=>{const r=e.getBoundingClientRect();return r.width>0&&r.height>0;};"
       "const e=document.querySelector('#prompt-textarea')||"
       "[...document.querySelectorAll('textarea,[contenteditable=\"true\"]')].find(visible);"
       "if(!e)return 'NO_COMPOSER';"
       "if(e.disabled||e.getAttribute('aria-disabled')==='true')return 'DISABLED';"
       "const t=('value' in e?e.value:e.innerText).trim();"
       "if(t)return 'BUSY';"
       "e.focus();return document.activeElement===e?'READY':'NO_FOCUS';"
       "})()";
  params=cJSON_CreateObject();
  cJSON_AddStringToObject(params,"expression",expr);
  cJSON_AddBoolToObject(params,"returnByValue",1);
  cJSON_AddBoolToObject(params,"userGesture",1);
  reply=NULL;
  if(cdp_call(ws,id++,"Runtime.evaluate",params,&reply)!=0) {
    log_line(stderr,"ERROR","watch CDP Runtime.evaluate failed chat=%s",state->chat);
    goto done;
  }
  value=watch_eval_string(reply);
  if(value==NULL || strcmp(value,"READY")!=0) {
    log_line(stderr,"ERROR","watch composer not ready chat=%s state=%s",state->chat,value!=NULL?value:"invalid");
    cJSON_Delete(reply);
    goto done;
  }
  cJSON_Delete(reply);
  params=cJSON_CreateObject();
  cJSON_AddStringToObject(params,"text","check");
  if(cdp_call(ws,id++,"Input.insertText",params,NULL)!=0) {
    log_line(stderr,"ERROR","watch CDP Input.insertText failed chat=%s",state->chat);
    goto done;
  }
  usleep(100000);
  params=cJSON_CreateObject();
  cJSON_AddStringToObject(params,"type","rawKeyDown");
  cJSON_AddStringToObject(params,"key","Enter");
  cJSON_AddStringToObject(params,"code","Enter");
  cJSON_AddNumberToObject(params,"windowsVirtualKeyCode",13);
  cJSON_AddNumberToObject(params,"nativeVirtualKeyCode",13);
  if(cdp_call(ws,id++,"Input.dispatchKeyEvent",params,NULL)!=0) {
    log_line(stderr,"ERROR","watch CDP keyDown failed chat=%s",state->chat);
    goto done;
  }
  params=cJSON_CreateObject();
  cJSON_AddStringToObject(params,"type","keyUp");
  cJSON_AddStringToObject(params,"key","Enter");
  cJSON_AddStringToObject(params,"code","Enter");
  cJSON_AddNumberToObject(params,"windowsVirtualKeyCode",13);
  cJSON_AddNumberToObject(params,"nativeVirtualKeyCode",13);
  if(cdp_call(ws,id++,"Input.dispatchKeyEvent",params,NULL)!=0) {
    log_line(stderr,"ERROR","watch CDP keyUp failed chat=%s",state->chat);
    goto done;
  }
  log_line(stdout,"WATCH","check sent chat=%s tab=%s",state->chat,title);
  result=0;
done:
  if(ws!=NULL) curl_easy_cleanup(ws);
  free(ws_url);
  free(title);
  return result;
}

static struct ChatState *find_chat_state(struct ChatState *states,size_t count,const char *chat) {
  size_t i;

  for(i=0;i<count;i++) if(strcmp(states[i].chat,chat)==0) return &states[i];
  return NULL;
}

static struct ChatState *add_chat_state(struct ChatState **states,size_t *count,size_t *cap,
    const struct TabInfo *tab) {
  struct ChatState *p,*state;
  size_t new_cap;

  if(*count==*cap) {
    new_cap=*cap!=0?*cap*2:8;
    p=(struct ChatState *)realloc(*states,new_cap*sizeof(**states));
    if(p==NULL) return NULL;
    *states=p;
    *cap=new_cap;
  }
  state=&(*states)[*count];
  memset(state,0,sizeof(*state));
  snprintf(state->chat,sizeof(state->chat),"%s",tab->chat);
  snprintf(state->conversation_id,sizeof(state->conversation_id),"%s",tab->conversation_id);
  snprintf(state->title,sizeof(state->title),"%s",tab->title);
  state->active=1;
  (*count)++;
  return state;
}

static struct JobState *find_job(struct ChatState *state,const char *job_id) {
  size_t i;

  for(i=0;i<state->job_count;i++)
    if(strcmp(state->jobs[i].job_id,job_id)==0) return &state->jobs[i];
  return NULL;
}

static struct JobState *add_job(struct ChatState *state,const char *job_id) {
  struct JobState *p,*job;
  size_t new_cap;

  if(state->job_count==state->job_cap) {
    new_cap=state->job_cap!=0?state->job_cap*2:16;
    p=(struct JobState *)realloc(state->jobs,new_cap*sizeof(*state->jobs));
    if(p==NULL) return NULL;
    state->jobs=p;
    state->job_cap=new_cap;
  }
  job=&state->jobs[state->job_count++];
  memset(job,0,sizeof(*job));
  snprintf(job->job_id,sizeof(job->job_id),"%s",job_id);
  return job;
}

static void format_elapsed(cJSON *elapsed,char *out,size_t out_size) {
  long total;
  int days,hours,minutes,seconds;

  total=cJSON_IsNumber(elapsed)?(long)elapsed->valuedouble:0;
  if(total<0) total=0;
  days=(int)(total/86400L);
  hours=(int)((total%86400L)/3600L);
  minutes=(int)((total%3600L)/60L);
  seconds=(int)(total%60L);
  if(days>0) snprintf(out,out_size,"%dd %02d:%02d:%02d",days,hours,minutes,seconds);
  else snprintf(out,out_size,"%02d:%02d:%02d",hours,minutes,seconds);
}

static int update_jobs(struct ChatState *state,cJSON *jobs,int initial) {
  cJSON *item,*job_id,*job_state,*elapsed,*command,*exit_code;
  struct JobState *job;
  char elapsed_text[32];
  size_t i,j;
  int n,state_code,pending,running,exited,exit_value,completed_now;

  for(i=0;i<state->job_count;i++) state->jobs[i].seen=0;
  running=0;
  exited=0;
  n=cJSON_GetArraySize(jobs);
  for(j=0;j<(size_t)n;j++) {
    item=cJSON_GetArrayItem(jobs,(int)j);
    job_id=cJSON_GetObjectItemCaseSensitive(item,"job_id");
    job_state=cJSON_GetObjectItemCaseSensitive(item,"state");
    if(!cJSON_IsString(job_id) || !cJSON_IsString(job_state)) continue;
    if(strcmp(job_state->valuestring,"running")==0) state_code=JOB_RUNNING;
    else if(strcmp(job_state->valuestring,"exited")==0) state_code=JOB_EXITED;
    else continue;
    job=find_job(state,job_id->valuestring);
    completed_now=0;
    if(job==NULL) {
      job=add_job(state,job_id->valuestring);
      if(job==NULL) return -1;
      job->state=state_code;
      job->notified=(initial && state_code==JOB_EXITED)?1:0;
      if(!initial && state_code==JOB_EXITED) completed_now=1;
    } else if(job->state==JOB_RUNNING && state_code==JOB_EXITED) {
      job->state=JOB_EXITED;
      job->notified=0;
      completed_now=1;
    } else {
      job->state=state_code;
    }
    job->seen=1;
    if(state_code==JOB_RUNNING) {
      running++;
      elapsed=cJSON_GetObjectItemCaseSensitive(item,"elapsed_seconds");
      command=cJSON_GetObjectItemCaseSensitive(item,"command");
      format_elapsed(elapsed,elapsed_text,sizeof(elapsed_text));
      if(cJSON_IsString(command))
        log_line(stdout,"JOB","chat=%s running id=%s elapsed=%s command=%s",state->chat,job_id->valuestring,elapsed_text,command->valuestring);
      else
        log_line(stdout,"JOB","chat=%s running id=%s elapsed=%s",state->chat,job_id->valuestring,elapsed_text);
    } else {
      exited++;
      if(completed_now) {
        exit_code=cJSON_GetObjectItemCaseSensitive(item,"exit_code");
        exit_value=cJSON_IsNumber(exit_code)?exit_code->valueint:0;
        log_line(stdout,"JOB","chat=%s completed id=%s exit=%d",state->chat,job_id->valuestring,exit_value);
      }
    }
  }
  for(i=0;i<state->job_count;) {
    if(!state->jobs[i].seen && state->jobs[i].state==JOB_EXITED && state->jobs[i].notified) {
      state->jobs[i]=state->jobs[state->job_count-1];
      state->job_count--;
    } else {
      i++;
    }
  }
  pending=0;
  for(i=0;i<state->job_count;i++)
    if(state->jobs[i].state==JOB_EXITED && !state->jobs[i].notified) pending++;
  log_line(stdout,"WATCH","chat=%s jobs=%d running=%d exited=%d pending=%d%s",state->chat,n,running,exited,pending,initial?" baseline":"");
  if(initial || pending==0) return 0;
  if(send_check(state)!=0) {
    log_line(stderr,"ERROR","watch check failed chat=%s",state->chat);
    return 0;
  }
  for(i=0;i<state->job_count;i++)
    if(state->jobs[i].state==JOB_EXITED && !state->jobs[i].notified) state->jobs[i].notified=1;
  return 0;
}

static void remove_inactive_states(struct ChatState *states,size_t *count) {
  size_t i;

  for(i=0;i<*count;) {
    if(states[i].active) {
      i++;
      continue;
    }
    free(states[i].jobs);
    states[i]=states[*count-1];
    (*count)--;
  }
}

static int watch_scan(CURL *curl,struct ChatState **states,size_t *state_count,size_t *state_cap,int notify) {
  struct TabInfo *tabs;
  struct ChatState *state;
  cJSON *jobs;
  size_t tab_count,i;
  int initial;

  if(discover_tabs(&tabs,&tab_count)!=0) {
    log_line(stderr,"ERROR","watch scan: Chrome discovery failed");
    return -1;
  }
  log_line(stdout,"WATCH","scan tabs=%lu",(unsigned long)tab_count);
  for(i=0;i<*state_count;i++) (*states)[i].active=0;
  for(i=0;i<tab_count;i++) {
    state=find_chat_state(*states,*state_count,tabs[i].chat);
    if(state==NULL) {
      state=add_chat_state(states,state_count,state_cap,&tabs[i]);
      if(state==NULL) {
        free(tabs);
        return -1;
      }
    }
    state->active=1;
    log_line(stdout,"WATCH","chat=%s tab=%s",tabs[i].chat,tabs[i].title);
    if(tabs[i].ambiguous) {
      log_line(stderr,"ERROR","watch chat=%s ambiguous highest-numbered tab",tabs[i].chat);
      free(state->jobs);
      state->jobs=NULL;
      state->job_count=0;
      state->job_cap=0;
      state->initialized=0;
      continue;
    }
    if(state->initialized && strcmp(state->conversation_id,tabs[i].conversation_id)!=0) {
      free(state->jobs);
      state->jobs=NULL;
      state->job_count=0;
      state->job_cap=0;
      state->initialized=0;
    }
    snprintf(state->conversation_id,sizeof(state->conversation_id),"%s",tabs[i].conversation_id);
    snprintf(state->title,sizeof(state->title),"%s",tabs[i].title);
    jobs=query_jobs(curl,state->chat);
    if(jobs==NULL) {
      log_line(stderr,"ERROR","watch jobs query failed chat=%s",state->chat);
      free(tabs);
      return -1;
    }
    initial=!state->initialized;
    if(!notify) initial=1;
    if(update_jobs(state,jobs,initial)!=0) {
      cJSON_Delete(jobs);
      free(tabs);
      return -1;
    }
    if(notify && !state->initialized) state->initialized=1;
    cJSON_Delete(jobs);
  }
  if(notify) remove_inactive_states(*states,state_count);
  free(tabs);
  return 0;
}

static int process_wait_reply(const char *text,pid_t *worker_pid) {
  cJSON *root,*item,*payload,*result;
  const char *status,*request_id,*module,*action;
  char *error_text;
  CURL *worker_curl;
  pid_t pid;
  int rc;

  root=cJSON_Parse(text!=NULL?text:"");
  if(!cJSON_IsObject(root)) {
    cJSON_Delete(root);
    fprintf(stderr,"invalid server JSON\n");
    return -1;
  }
  item=cJSON_GetObjectItemCaseSensitive(root,"status");
  status=cJSON_IsString(item)?item->valuestring:NULL;
  if(status!=NULL && strcmp(status,"idle")==0) {
    log_line(stdout,"AGENT","idle");
    cJSON_Delete(root);
    return 0;
  }
  if(status==NULL || strcmp(status,"request")!=0) {
    fprintf(stderr,"unexpected server response: %s\n",text!=NULL?text:"");
    cJSON_Delete(root);
    return -1;
  }
  item=cJSON_GetObjectItemCaseSensitive(root,"request_id");
  request_id=cJSON_IsString(item)?item->valuestring:NULL;
  item=cJSON_GetObjectItemCaseSensitive(root,"module");
  module=cJSON_IsString(item)?item->valuestring:NULL;
  item=cJSON_GetObjectItemCaseSensitive(root,"action");
  action=cJSON_IsString(item)?item->valuestring:NULL;
  payload=cJSON_GetObjectItemCaseSensitive(root,"payload");
  if(request_id==NULL || module==NULL || action==NULL) {
    cJSON_Delete(root);
    fprintf(stderr,"incomplete agent request\n");
    return -1;
  }
  log_line(stdout,"AGENT","received request=%s module=%s action=%s",request_id,module,action);
  pid=fork();
  if(pid<0) {
    cJSON_Delete(root);
    fprintf(stderr,"cannot fork agent worker: %s\n",strerror(errno));
    return -1;
  }
  if(pid==0) {
    worker_curl=curl_easy_init();
    if(worker_curl==NULL) _exit(1);
    error_text=NULL;
    result=dispatch_request(module,action,payload,&error_text);
    rc=send_result(worker_curl,request_id,result,error_text);
    free(error_text);
    curl_easy_cleanup(worker_curl);
    if(rc==0)
      log_line(stdout,"AGENT","completed request=%s",request_id);
    else
      log_line(stderr,"ERROR","agent request failed request=%s",request_id);
    cJSON_Delete(root);
    _exit(rc==0?0:1);
  }
  log_line(stdout,"AGENT","worker started request=%s pid=%ld",request_id,(long)pid);
  cJSON_Delete(root);
  *worker_pid=pid;
  return 1;
}

static int agent_wait(CURL *curl,pid_t *worker_pid) {
  char *reply;
  long http_code;
  int rc;

  reply=NULL;
  rc=http_agent_request(curl,"wait","{}",HTTP_WAIT_TIMEOUT,&reply,&http_code);
  if(rc!=0) {
    log_line(stderr,"ERROR","agent wait network failure");
    return -1;
  }
  if(http_code!=200) {
    log_line(stderr,"ERROR","agent wait rejected HTTP=%ld detail=%s",http_code,reply!=NULL?reply:"");
    free(reply);
    return -1;
  }
  rc=process_wait_reply(reply,worker_pid);
  free(reply);
  return rc;
}

static void usage(const char *prog) {
  printf("mcp_agent %s\n",MCP_AGENT_VERSION);
  printf("usage: %s\n",prog);
  printf("runs the remote agent and the ChatGPT job watcher together\n");
  printf("watch interval: %d seconds\n",WATCH_INTERVAL);
  printf("modules: test/echo, browser/read_tab, edistribuzione/load_profile.month, qrz/webcontact.add\n");
  printf("browser/read_tab reads at most %d characters from document.body.innerText\n",BROWSER_TEXT_MAX);
  printf("default endpoint: %s\n",DEFAULT_URL);
  printf("default agent: %s\n",DEFAULT_AGENT_ID);
  printf("tokens: ~/mcp/token.txt and ~/mcp/agent.token\n");
  printf("environment: MCP_AGENT_URL, MCP_AGENT_ID, MCP_TOKEN, MCP_AGENT_TOKEN, MCP_CHROME_URL\n");
}

int main(int argc,char **argv) {
  struct ChatState *states;
  CURL *agent_curl,*watch_curl;
  size_t state_count,state_cap;
  time_t next_watch,now;
  pid_t worker_pid,waited;
  int rc,status;

  if(argc==2 && (strcmp(argv[1],"-h")==0 || strcmp(argv[1],"--help")==0)) {
    usage(argv[0]);
    return 0;
  }
  if(argc!=1) {
    usage(argv[0]);
    return 2;
  }
  if(read_text_token("MCP_TOKEN","token.txt",g_mcp_token,sizeof(g_mcp_token))!=0) {
    fprintf(stderr,"cannot read MCP token\n");
    return 1;
  }
  if(read_text_token("MCP_AGENT_TOKEN","agent.token",g_agent_token,sizeof(g_agent_token))!=0) {
    fprintf(stderr,"cannot read agent token\n");
    return 1;
  }
  if(curl_global_init(CURL_GLOBAL_DEFAULT)!=CURLE_OK) return 1;
  agent_curl=curl_easy_init();
  watch_curl=curl_easy_init();
  if(agent_curl==NULL || watch_curl==NULL) {
    if(agent_curl!=NULL) curl_easy_cleanup(agent_curl);
    if(watch_curl!=NULL) curl_easy_cleanup(watch_curl);
    curl_global_cleanup();
    return 1;
  }
  states=NULL;
  state_count=0;
  state_cap=0;
  worker_pid=0;
  next_watch=time(NULL);
  log_line(stdout,"START","mcp_agent=%s agent=%s watch=%ds",MCP_AGENT_VERSION,agent_id(),WATCH_INTERVAL);
  log_line(stdout,"START","Chrome=%s MCP=%s",chrome_host(),agent_url());
  for(;;) {
    now=time(NULL);
    if(now>=next_watch) {
      watch_scan(watch_curl,&states,&state_count,&state_cap,1);
      next_watch=now+WATCH_INTERVAL;
    }
    if(worker_pid!=0) {
      waited=waitpid(worker_pid,&status,WNOHANG);
      if(waited==0) {
        sleep(1);
        continue;
      }
      if(waited<0)
        log_line(stderr,"ERROR","agent worker waitpid failed pid=%ld error=%s",(long)worker_pid,strerror(errno));
      else if(WIFEXITED(status))
        log_line(stdout,"AGENT","worker ended pid=%ld exit=%d",(long)worker_pid,WEXITSTATUS(status));
      else if(WIFSIGNALED(status))
        log_line(stderr,"ERROR","agent worker ended pid=%ld signal=%d",(long)worker_pid,WTERMSIG(status));
      worker_pid=0;
      continue;
    }
    rc=agent_wait(agent_curl,&worker_pid);
    if(rc<0) {
      log_line(stderr,"ERROR","agent retry in 2s");
      sleep(2);
      continue;
    }
  }
}
