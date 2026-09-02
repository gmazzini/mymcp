// Gianluca Mazzini @2026- Version 1.05
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <poll.h>
#include <time.h>
#include <limits.h>
#include <curl/curl.h>
#include <cjson/cJSON.h>

#define MCP_URL "https://www.mazzini.org/mcp"
#define MCP_PROTOCOL "2026-07-28"
#define MCP_CLIENT_NAME "mcp-watch"
#define MCP_CLIENT_VERSION "1.05"
#define CHROME_HOST "http://127.0.0.1:9222"
#define CHECK_TEXT "check"
#define WATCH_INTERVAL 60
#define HTTP_TIMEOUT_MS 10000L
#define WS_TIMEOUT_MS 5000
#define CHAT_MAX 64
#define CONVERSATION_MAX 128
#define JOB_ID_MAX 63

#define JOB_RUNNING 1
#define JOB_EXITED 2

struct Buffer {
  char *data;
  size_t len;
};

struct TabInfo {
  char chat[CHAT_MAX+1];
  char conversation_id[CONVERSATION_MAX+1];
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
  struct JobState *jobs;
  size_t job_count;
  size_t job_cap;
  int active;
  int initialized;
};

static size_t write_cb(char *ptr,size_t size,size_t nmemb,void *userdata) {
  struct Buffer *b;
  char *p;
  size_t n;

  b=(struct Buffer *)userdata;
  n=size*nmemb;
  p=(char *)realloc(b->data,b->len+n+1);
  if(!p) return 0;
  b->data=p;
  memcpy(b->data+b->len,ptr,n);
  b->len+=n;
  b->data[b->len]=0;
  return n;
}

static int valid_chat(const char *s) {
  size_t i,n;

  if(!s) return 0;
  n=strlen(s);
  if(n<1 || n>CHAT_MAX) return 0;
  for(i=0;i<n;i++) {
    if(!isalnum((unsigned char)s[i]) && s[i]!='_' && s[i]!='-' && s[i]!='.') return 0;
  }
  return 1;
}

static int valid_conversation_id(const char *s) {
  size_t i,n;

  if(!s) return 0;
  n=strlen(s);
  if(n<8 || n>CONVERSATION_MAX) return 0;
  for(i=0;i<n;i++) {
    if(!isalnum((unsigned char)s[i]) && s[i]!='-' && s[i]!='_') return 0;
  }
  return 1;
}

static void trim_line(char *s) {
  size_t n;

  if(!s) return;
  n=strlen(s);
  while(n>0 && isspace((unsigned char)s[n-1])) {
    s[n-1]=0;
    n--;
  }
}

static int read_token(char *out,size_t out_size) {
  const char *home;
  char path[PATH_MAX];
  FILE *f;
  int n;

  home=getenv("HOME");
  if(!home || !*home) return -1;
  n=snprintf(path,sizeof(path),"%s/mcp/token.txt",home);
  if(n<0 || (size_t)n>=sizeof(path)) return -1;
  f=fopen(path,"r");
  if(!f) {
    fprintf(stderr,"cannot open %s\n",path);
    return -1;
  }
  if(!fgets(out,(int)out_size,f)) {
    fclose(f);
    fprintf(stderr,"cannot read %s\n",path);
    return -1;
  }
  fclose(f);
  trim_line(out);
  if(out[0]==0) {
    fprintf(stderr,"empty token in %s\n",path);
    return -1;
  }
  return 0;
}

static cJSON *make_jobs_request(const char *chat) {
  cJSON *root,*params,*meta,*info,*args;

  root=cJSON_CreateObject();
  if(!root) return NULL;
  cJSON_AddStringToObject(root,"jsonrpc","2.0");
  cJSON_AddNumberToObject(root,"id",1);
  cJSON_AddStringToObject(root,"method","tools/call");
  params=cJSON_AddObjectToObject(root,"params");
  meta=cJSON_AddObjectToObject(params,"_meta");
  cJSON_AddStringToObject(meta,"io.modelcontextprotocol/protocolVersion",MCP_PROTOCOL);
  cJSON_AddObjectToObject(meta,"io.modelcontextprotocol/clientCapabilities");
  info=cJSON_AddObjectToObject(meta,"io.modelcontextprotocol/clientInfo");
  cJSON_AddStringToObject(info,"name",MCP_CLIENT_NAME);
  cJSON_AddStringToObject(info,"version",MCP_CLIENT_VERSION);
  cJSON_AddStringToObject(params,"name","jobs");
  args=cJSON_AddObjectToObject(params,"arguments");
  cJSON_AddStringToObject(args,"chat",chat);
  cJSON_AddNumberToObject(args,"limit",1000);
  return root;
}

static cJSON *query_jobs(const char *chat,const char *token) {
  char auth[640];
  struct Buffer body;
  struct curl_slist *headers;
  CURL *curl;
  CURLcode rc;
  cJSON *req,*reply,*result,*content;
  char *json;
  long http_code;

  req=make_jobs_request(chat);
  if(!req) return NULL;
  json=cJSON_PrintUnformatted(req);
  cJSON_Delete(req);
  if(!json) return NULL;
  if(snprintf(auth,sizeof(auth),"Authorization: Bearer %s",token)>=(int)sizeof(auth)) {
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
  curl=curl_easy_init();
  if(!curl) {
    curl_slist_free_all(headers);
    free(json);
    return NULL;
  }
  curl_easy_setopt(curl,CURLOPT_URL,MCP_URL);
  curl_easy_setopt(curl,CURLOPT_POST,1L);
  curl_easy_setopt(curl,CURLOPT_HTTPHEADER,headers);
  curl_easy_setopt(curl,CURLOPT_POSTFIELDS,json);
  curl_easy_setopt(curl,CURLOPT_POSTFIELDSIZE,(long)strlen(json));
  curl_easy_setopt(curl,CURLOPT_WRITEFUNCTION,write_cb);
  curl_easy_setopt(curl,CURLOPT_WRITEDATA,&body);
  curl_easy_setopt(curl,CURLOPT_CONNECTTIMEOUT_MS,HTTP_TIMEOUT_MS);
  curl_easy_setopt(curl,CURLOPT_TIMEOUT_MS,HTTP_TIMEOUT_MS);
  curl_easy_setopt(curl,CURLOPT_NOSIGNAL,1L);
  rc=curl_easy_perform(curl);
  http_code=0;
  curl_easy_getinfo(curl,CURLINFO_RESPONSE_CODE,&http_code);
  curl_easy_cleanup(curl);
  curl_slist_free_all(headers);
  free(json);
  if(rc!=CURLE_OK) {
    fprintf(stderr,"MCP HTTP error for %s: %s\n",chat,curl_easy_strerror(rc));
    free(body.data);
    return NULL;
  }
  if(http_code!=200) {
    fprintf(stderr,"MCP HTTP status for %s: %ld\n",chat,http_code);
    free(body.data);
    return NULL;
  }
  reply=cJSON_Parse(body.data ? body.data : "");
  free(body.data);
  if(!reply) {
    fprintf(stderr,"invalid MCP JSON response for %s\n",chat);
    return NULL;
  }
  result=cJSON_GetObjectItemCaseSensitive(reply,"result");
  if(!cJSON_IsObject(result)) {
    cJSON_Delete(reply);
    fprintf(stderr,"MCP result missing for %s\n",chat);
    return NULL;
  }
  content=cJSON_GetObjectItemCaseSensitive(result,"structuredContent");
  if(!cJSON_IsArray(content)) {
    cJSON_Delete(reply);
    fprintf(stderr,"MCP jobs structuredContent missing for %s\n",chat);
    return NULL;
  }
  cJSON_DetachItemViaPointer(result,content);
  cJSON_Delete(reply);
  return content;
}

static int chrome_get(const char *url,struct Buffer *out) {
  CURL *curl;
  CURLcode rc;

  out->data=NULL;
  out->len=0;
  curl=curl_easy_init();
  if(!curl) return -1;
  curl_easy_setopt(curl,CURLOPT_URL,url);
  curl_easy_setopt(curl,CURLOPT_WRITEFUNCTION,write_cb);
  curl_easy_setopt(curl,CURLOPT_WRITEDATA,out);
  curl_easy_setopt(curl,CURLOPT_CONNECTTIMEOUT_MS,3000L);
  curl_easy_setopt(curl,CURLOPT_TIMEOUT_MS,3000L);
  curl_easy_setopt(curl,CURLOPT_NOSIGNAL,1L);
  rc=curl_easy_perform(curl);
  curl_easy_cleanup(curl);
  if(rc!=CURLE_OK) {
    fprintf(stderr,"Chrome HTTP error: %s\n",curl_easy_strerror(rc));
    free(out->data);
    out->data=NULL;
    return -1;
  }
  return 0;
}

static int extract_chat(const char *title,char *out,size_t out_size,int *number) {
  const char *p,*end;
  char *number_end;
  long value;
  size_t n;

  if(!title || !number || out_size<2) return -1;
  p=title;
  while(*p && isspace((unsigned char)*p)) p++;
  n=0;
  while(p[n] && !isspace((unsigned char)p[n])) n++;
  if(n==0 || n>=out_size) return -1;
  memcpy(out,p,n);
  out[n]=0;
  if(!valid_chat(out)) return -1;
  p+=n;
  while(*p && isspace((unsigned char)*p)) p++;
  if(!*p) {
    *number=0;
    return 0;
  }
  value=strtol(p,&number_end,10);
  if(number_end==p || value<0 || value>INT_MAX) return -1;
  end=number_end;
  while(*end && isspace((unsigned char)*end)) end++;
  if(*end) return -1;
  *number=(int)value;
  return 0;
}

static int extract_conversation_id(const char *url,char *out,size_t out_size) {
  const char *p;
  size_t n;

  if(!url || !out || out_size<2) return -1;
  if(!strstr(url,"chatgpt.com/") && !strstr(url,"chat.openai.com/")) return -1;
  p=strstr(url,"/c/");
  if(!p) return -1;
  p+=3;
  n=0;
  while(p[n] && p[n]!='?' && p[n]!='#' && p[n]!='/') n++;
  if(n==0 || n>=out_size) return -1;
  memcpy(out,p,n);
  out[n]=0;
  return valid_conversation_id(out) ? 0 : -1;
}

static int add_tab(struct TabInfo **tabs,size_t *count,size_t *cap,const char *chat,const char *conversation_id,int number) {
  struct TabInfo *p;
  size_t i,new_cap;

  for(i=0;i<*count;i++) {
    if(strcmp((*tabs)[i].chat,chat)!=0) continue;
    if(number>(*tabs)[i].number) {
      snprintf((*tabs)[i].conversation_id,sizeof((*tabs)[i].conversation_id),"%s",conversation_id);
      (*tabs)[i].number=number;
      (*tabs)[i].ambiguous=0;
    } else if(number==(*tabs)[i].number && strcmp((*tabs)[i].conversation_id,conversation_id)!=0)
      (*tabs)[i].ambiguous=1;
    return 0;
  }
  if(*count==*cap) {
    new_cap=*cap ? *cap*2 : 8;
    p=(struct TabInfo *)realloc(*tabs,new_cap*sizeof(**tabs));
    if(!p) return -1;
    *tabs=p;
    *cap=new_cap;
  }
  snprintf((*tabs)[*count].chat,sizeof((*tabs)[*count].chat),"%s",chat);
  snprintf((*tabs)[*count].conversation_id,sizeof((*tabs)[*count].conversation_id),"%s",conversation_id);
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
  if(snprintf(endpoint,sizeof(endpoint),"%s/json/list",CHROME_HOST)>=(int)sizeof(endpoint)) return -1;
  if(chrome_get(endpoint,&body)!=0) return -1;
  root=cJSON_Parse(body.data ? body.data : "");
  free(body.data);
  if(!cJSON_IsArray(root)) {
    cJSON_Delete(root);
    fprintf(stderr,"invalid Chrome /json/list response\n");
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
    if(add_tab(tabs,count,&cap,chat,conversation_id,number)!=0) {
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

static int candidate_id(cJSON *item,const char *conversation_id) {
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

static int find_target(const char *conversation_id,char **ws_url,char **title) {
  struct Buffer body;
  cJSON *root,*item,*cjs_title,*cjs_ws;
  char endpoint[256];
  int i,n,count;

  if(snprintf(endpoint,sizeof(endpoint),"%s/json/list",CHROME_HOST)>=(int)sizeof(endpoint)) return -1;
  if(chrome_get(endpoint,&body)!=0) return -1;
  root=cJSON_Parse(body.data ? body.data : "");
  free(body.data);
  if(!cJSON_IsArray(root)) {
    cJSON_Delete(root);
    fprintf(stderr,"invalid Chrome /json/list response\n");
    return -1;
  }
  n=cJSON_GetArraySize(root);
  count=0;
  for(i=0;i<n;i++) {
    item=cJSON_GetArrayItem(root,i);
    if(candidate_id(item,conversation_id)) count++;
  }
  if(count!=1) {
    if(count==0) fprintf(stderr,"ChatGPT tab disappeared for conversation %s\n",conversation_id);
    else fprintf(stderr,"multiple tabs use conversation %s\n",conversation_id);
    cJSON_Delete(root);
    return -1;
  }
  for(i=0;i<n;i++) {
    item=cJSON_GetArrayItem(root,i);
    if(!candidate_id(item,conversation_id)) continue;
    cjs_title=cJSON_GetObjectItemCaseSensitive(item,"title");
    cjs_ws=cJSON_GetObjectItemCaseSensitive(item,"webSocketDebuggerUrl");
    if(!cJSON_IsString(cjs_title) || !cJSON_IsString(cjs_ws)) break;
    *ws_url=strdup(cjs_ws->valuestring);
    *title=strdup(cjs_title->valuestring);
    cJSON_Delete(root);
    if(!*ws_url || !*title) {
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
  return n>0 ? 0 : -1;
}

static int ws_send_text(CURL *ws,const char *text) {
  CURLcode rc;
  size_t off,sent,len;

  off=0;
  len=strlen(text);
  while(off<len) {
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
      char *p;
      p=(char *)realloc(b.data,b.len+got+1);
      if(!p) {
        free(b.data);
        return -1;
      }
      b.data=p;
      memcpy(b.data+b.len,chunk,got);
      b.len+=got;
      b.data[b.len]=0;
    }
    if(meta && meta->bytesleft==0) {
      if(meta->flags&CURLWS_TEXT) {
        if(!b.data) b.data=strdup("");
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
  cJSON *req,*root,*rid,*err;
  char *json,*msg;
  int rc;

  req=cJSON_CreateObject();
  if(!req) return -1;
  cJSON_AddNumberToObject(req,"id",id);
  cJSON_AddStringToObject(req,"method",method);
  if(params) cJSON_AddItemToObject(req,"params",params);
  json=cJSON_PrintUnformatted(req);
  cJSON_Delete(req);
  if(!json) return -1;
  rc=ws_send_text(ws,json);
  free(json);
  if(rc!=0) return -1;
  for(;;) {
    msg=NULL;
    if(ws_recv_text(ws,&msg)!=0) return -1;
    root=cJSON_Parse(msg ? msg : "");
    free(msg);
    if(!root) continue;
    rid=cJSON_GetObjectItemCaseSensitive(root,"id");
    if(!cJSON_IsNumber(rid) || rid->valueint!=id) {
      cJSON_Delete(root);
      continue;
    }
    err=cJSON_GetObjectItemCaseSensitive(root,"error");
    if(err) {
      json=cJSON_PrintUnformatted(err);
      fprintf(stderr,"CDP %s error: %s\n",method,json ? json : "unknown");
      free(json);
      cJSON_Delete(root);
      return -1;
    }
    if(reply) *reply=root;
    else cJSON_Delete(root);
    return 0;
  }
}

static const char *eval_string(cJSON *reply) {
  cJSON *result,*remote,*value;

  result=cJSON_GetObjectItemCaseSensitive(reply,"result");
  if(!cJSON_IsObject(result)) return NULL;
  remote=cJSON_GetObjectItemCaseSensitive(result,"result");
  if(!cJSON_IsObject(remote)) return NULL;
  value=cJSON_GetObjectItemCaseSensitive(remote,"value");
  if(!cJSON_IsString(value)) return NULL;
  return value->valuestring;
}

static int send_check(const char *conversation_id) {
  const char *expr,*value;
  cJSON *params,*reply;
  char *ws_url,*title;
  CURL *ws;
  CURLcode rc;
  int id,result;

  ws_url=NULL;
  title=NULL;
  ws=NULL;
  result=-1;
  if(find_target(conversation_id,&ws_url,&title)!=0) goto done;
  ws=curl_easy_init();
  if(!ws) goto done;
  curl_easy_setopt(ws,CURLOPT_URL,ws_url);
  curl_easy_setopt(ws,CURLOPT_CONNECT_ONLY,2L);
  curl_easy_setopt(ws,CURLOPT_CONNECTTIMEOUT_MS,3000L);
  curl_easy_setopt(ws,CURLOPT_NOSIGNAL,1L);
  rc=curl_easy_perform(ws);
  if(rc!=CURLE_OK) {
    fprintf(stderr,"Chrome WebSocket error: %s\n",curl_easy_strerror(rc));
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
  if(cdp_call(ws,id++,"Runtime.evaluate",params,&reply)!=0) goto done;
  value=eval_string(reply);
  if(!value || strcmp(value,"READY")!=0) {
    fprintf(stderr,"Composer not ready in %s: %s\n",title,value ? value : "invalid response");
    cJSON_Delete(reply);
    goto done;
  }
  cJSON_Delete(reply);
  params=cJSON_CreateObject();
  cJSON_AddStringToObject(params,"text",CHECK_TEXT);
  if(cdp_call(ws,id++,"Input.insertText",params,NULL)!=0) goto done;
  usleep(100000);
  params=cJSON_CreateObject();
  cJSON_AddStringToObject(params,"type","rawKeyDown");
  cJSON_AddStringToObject(params,"key","Enter");
  cJSON_AddStringToObject(params,"code","Enter");
  cJSON_AddNumberToObject(params,"windowsVirtualKeyCode",13);
  cJSON_AddNumberToObject(params,"nativeVirtualKeyCode",13);
  if(cdp_call(ws,id++,"Input.dispatchKeyEvent",params,NULL)!=0) goto done;
  params=cJSON_CreateObject();
  cJSON_AddStringToObject(params,"type","keyUp");
  cJSON_AddStringToObject(params,"key","Enter");
  cJSON_AddStringToObject(params,"code","Enter");
  cJSON_AddNumberToObject(params,"windowsVirtualKeyCode",13);
  cJSON_AddNumberToObject(params,"nativeVirtualKeyCode",13);
  if(cdp_call(ws,id++,"Input.dispatchKeyEvent",params,NULL)!=0) goto done;
  printf("  check sent -> %s\n",title);
  result=0;

done:
  if(ws) curl_easy_cleanup(ws);
  free(ws_url);
  free(title);
  return result;
}

static struct ChatState *find_chat_state(struct ChatState *states,size_t count,const char *chat) {
  size_t i;

  for(i=0;i<count;i++) {
    if(strcmp(states[i].chat,chat)==0) return &states[i];
  }
  return NULL;
}

static struct ChatState *add_chat_state(struct ChatState **states,size_t *count,size_t *cap,const struct TabInfo *tab) {
  struct ChatState *p,*s;
  size_t new_cap;

  if(*count==*cap) {
    new_cap=*cap ? *cap*2 : 8;
    p=(struct ChatState *)realloc(*states,new_cap*sizeof(**states));
    if(!p) return NULL;
    *states=p;
    *cap=new_cap;
  }
  s=&(*states)[*count];
  memset(s,0,sizeof(*s));
  snprintf(s->chat,sizeof(s->chat),"%s",tab->chat);
  snprintf(s->conversation_id,sizeof(s->conversation_id),"%s",tab->conversation_id);
  s->active=1;
  (*count)++;
  return s;
}

static struct JobState *find_job(struct ChatState *state,const char *job_id) {
  size_t i;

  for(i=0;i<state->job_count;i++) {
    if(strcmp(state->jobs[i].job_id,job_id)==0) return &state->jobs[i];
  }
  return NULL;
}

static struct JobState *add_job(struct ChatState *state,const char *job_id) {
  struct JobState *p,*job;
  size_t new_cap;

  if(state->job_count==state->job_cap) {
    new_cap=state->job_cap ? state->job_cap*2 : 16;
    p=(struct JobState *)realloc(state->jobs,new_cap*sizeof(*state->jobs));
    if(!p) return NULL;
    state->jobs=p;
    state->job_cap=new_cap;
  }
  job=&state->jobs[state->job_count++];
  memset(job,0,sizeof(*job));
  snprintf(job->job_id,sizeof(job->job_id),"%s",job_id);
  return job;
}

static void print_elapsed(cJSON *elapsed) {
  long total;
  int days,hours,minutes,seconds;

  total=cJSON_IsNumber(elapsed) ? (long)elapsed->valuedouble : 0;
  if(total<0) total=0;
  days=(int)(total/86400L);
  hours=(int)((total%86400L)/3600L);
  minutes=(int)((total%3600L)/60L);
  seconds=(int)(total%60L);
  if(days>0) printf("%dd %02d:%02d:%02d",days,hours,minutes,seconds);
  else printf("%02d:%02d:%02d",hours,minutes,seconds);
}

static int update_jobs(struct ChatState *state,cJSON *jobs,int initial) {
  cJSON *item,*job_id,*job_state,*exit_code,*elapsed,*command;
  struct JobState *job;
  size_t i,j;
  int n,state_code,running,pending,exit_value;

  for(i=0;i<state->job_count;i++) state->jobs[i].seen=0;
  running=0;
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
    if(!job) {
      job=add_job(state,job_id->valuestring);
      if(!job) return -1;
      job->state=state_code;
      job->notified=(initial && state_code==JOB_EXITED) ? 1 : 0;
    } else if(job->state==JOB_RUNNING && state_code==JOB_EXITED) {
      job->state=JOB_EXITED;
      job->notified=0;
    } else job->state=state_code;
    job->seen=1;
    if(state_code==JOB_RUNNING) {
      elapsed=cJSON_GetObjectItemCaseSensitive(item,"elapsed_seconds");
      command=cJSON_GetObjectItemCaseSensitive(item,"command");
      printf("  running %s elapsed=",job_id->valuestring);
      print_elapsed(elapsed);
      if(cJSON_IsString(command)) printf(" command=%s",command->valuestring);
      printf("\n");
      running++;
    }
  }
  for(i=0;i<state->job_count;) {
    if(!state->jobs[i].seen && state->jobs[i].state==JOB_EXITED && state->jobs[i].notified) {
      state->jobs[i]=state->jobs[state->job_count-1];
      state->job_count--;
      continue;
    }
    i++;
  }
  pending=0;
  for(i=0;i<state->job_count;i++) {
    if(state->jobs[i].state==JOB_EXITED && !state->jobs[i].notified) pending++;
  }
  printf("  running=%d completed=%d\n",running,pending);
  if(initial) return 0;
  if(pending==0) return 0;
    for(i=0;i<state->job_count;i++) {
    if(state->jobs[i].state!=JOB_EXITED || state->jobs[i].notified) continue;
    exit_value=0;
    for(j=0;j<(size_t)n;j++) {
      item=cJSON_GetArrayItem(jobs,(int)j);
      job_id=cJSON_GetObjectItemCaseSensitive(item,"job_id");
      if(!cJSON_IsString(job_id) || strcmp(job_id->valuestring,state->jobs[i].job_id)!=0) continue;
      exit_code=cJSON_GetObjectItemCaseSensitive(item,"exit_code");
      if(cJSON_IsNumber(exit_code)) exit_value=exit_code->valueint;
      break;
    }
    printf("  completed %s exit=%d\n",state->jobs[i].job_id,exit_value);
  }
  if(send_check(state->conversation_id)!=0) return 0;
  for(i=0;i<state->job_count;i++) {
    if(state->jobs[i].state==JOB_EXITED && !state->jobs[i].notified) state->jobs[i].notified=1;
  }
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

static void print_stamp(void) {
  char stamp[32];
  time_t now;
  struct tm tmv;

  now=time(NULL);
  localtime_r(&now,&tmv);
  strftime(stamp,sizeof(stamp),"%Y-%m-%d %H:%M:%S",&tmv);
  printf("\n[%s]\n",stamp);
}

static int scan_once(struct ChatState **states,size_t *state_count,size_t *state_cap,const char *token,int notify) {
  struct TabInfo *tabs;
  struct ChatState *state;
  cJSON *jobs;
  size_t tab_count,i;
  int initial;

  if(discover_tabs(&tabs,&tab_count)!=0) return -1;
  for(i=0;i<*state_count;i++) (*states)[i].active=0;
  print_stamp();
  printf("ChatGPT chats=%lu\n",(unsigned long)tab_count);
  for(i=0;i<tab_count;i++) {
    printf("%s",tabs[i].chat);
    if(tabs[i].number>0) printf(" %d",tabs[i].number);
    printf(" -> %s",tabs[i].conversation_id);
    if(tabs[i].ambiguous) printf(" [AMBIGUOUS]");
    printf("\n");
    state=find_chat_state(*states,*state_count,tabs[i].chat);
    if(!state) {
      state=add_chat_state(states,state_count,state_cap,&tabs[i]);
      if(!state) {
        free(tabs);
        return -1;
      }
    }
    state->active=1;
    if(tabs[i].ambiguous) {
      free(state->jobs);
      state->jobs=NULL;
      state->job_count=0;
      state->job_cap=0;
      state->initialized=0;
      printf("  WARNING: chat=%s has multiple tabs with the same highest number; monitoring suspended\n",state->chat);
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
    jobs=query_jobs(state->chat,token);
    if(!jobs) continue;
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
  fflush(stdout);
  return 0;
}

static void free_states(struct ChatState *states,size_t count) {
  size_t i;

  for(i=0;i<count;i++) free(states[i].jobs);
  free(states);
}

static void usage(const char *prog) {
  printf("usage:\n");
  printf("  %s\n",prog);
  printf("  %s --query\n",prog);
  printf("\n");
  printf("The normal mode discovers all open ChatGPT tabs automatically.\n");
  printf("Tab titles must be <chat> or <chat> <number>; the first word is the MCP chat name.\n");
  printf("For multiple tabs of the same chat, only the highest numbered tab is monitored.\n");
  printf("Only jobs completing after the selected tab is first observed are notified.\n");
  printf("All state is kept in memory; watch interval is %d seconds.\n",WATCH_INTERVAL);
}

int main(int argc,char **argv) {
  struct ChatState *states;
  char token[512];
  size_t state_count,state_cap;
  int query_only,rc;

  query_only=0;
  if(argc==2 && (strcmp(argv[1],"-h")==0 || strcmp(argv[1],"--help")==0)) {
    usage(argv[0]);
    return 0;
  }
  if(argc==2 && strcmp(argv[1],"--query")==0) query_only=1;
  else if(argc!=1) {
    usage(argv[0]);
    return 2;
  }
  if(read_token(token,sizeof(token))!=0) return 1;
  if(curl_global_init(CURL_GLOBAL_DEFAULT)!=CURLE_OK) return 1;
  states=NULL;
  state_count=0;
  state_cap=0;
  if(query_only) rc=scan_once(&states,&state_count,&state_cap,token,0);
  else {
    rc=0;
    for(;;) {
      if(scan_once(&states,&state_count,&state_cap,token,1)!=0) rc=1;
      sleep(WATCH_INTERVAL);
    }
  }
  free_states(states,state_count);
  curl_global_cleanup();
  return rc==0 ? 0 : 1;
}
