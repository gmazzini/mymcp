// Gianluca Mazzini @2026- Version 1.03
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <poll.h>
#include <errno.h>
#include <time.h>
#include <sys/stat.h>
#include <limits.h>
#include <curl/curl.h>
#include <cjson/cJSON.h>

#define MCP_URL "https://www.mazzini.org/mcp"
#define MCP_PROTOCOL "2026-07-28"
#define MCP_CLIENT_NAME "mcp-watch"
#define MCP_CLIENT_VERSION "1.03"
#define CHROME_HOST "http://127.0.0.1:9222"
#define CHECK_TEXT "check"
#define WATCH_INTERVAL 60
#define HTTP_TIMEOUT_MS 10000L
#define WS_TIMEOUT_MS 5000
#define MAX_PENDING 1000
#define JOB_ID_MAX 63

struct Buffer {
  char *data;
  size_t len;
};

struct PendingJob {
  char job_id[JOB_ID_MAX+1];
  int exit_code;
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
  if(n<1 || n>64) return 0;
  for(i=0;i<n;i++) {
    if(!isalnum((unsigned char)s[i]) && s[i]!='_' && s[i]!='-' && s[i]!='.') return 0;
  }
  return 1;
}

static int valid_conversation_id(const char *s) {
  size_t i,n;

  if(!s) return 0;
  n=strlen(s);
  if(n<8 || n>128) return 0;
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

static int home_path(char *out,size_t out_size,const char *leaf) {
  const char *home;
  int n;

  home=getenv("HOME");
  if(!home || !*home) return -1;
  n=snprintf(out,out_size,"%s/mcp/%s",home,leaf);
  if(n<0 || (size_t)n>=out_size) return -1;
  return 0;
}

static int ensure_jobs_dir(char *out,size_t out_size) {
  int rc;

  if(home_path(out,out_size,"jobs")!=0) return -1;
  rc=mkdir(out,0755);
  if(rc!=0 && errno!=EEXIST) return -1;
  return 0;
}

static int read_token(char *out,size_t out_size) {
  char path[PATH_MAX];
  FILE *f;

  if(home_path(path,sizeof(path),"token.txt")!=0) return -1;
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

static void log_line(const char *text) {
  char path[PATH_MAX],stamp[64];
  FILE *f;
  time_t now;
  struct tm tmv;

  if(home_path(path,sizeof(path),"watcher.log")!=0) return;
  now=time(NULL);
  localtime_r(&now,&tmv);
  strftime(stamp,sizeof(stamp),"%Y-%m-%dT%H:%M:%S%z",&tmv);
  f=fopen(path,"a");
  if(!f) return;
  fprintf(f,"%s %s\n",stamp,text);
  fclose(f);
}

static int marker_path(const char *job_id,char *out,size_t out_size) {
  char jobs[PATH_MAX];
  int n;

  if(ensure_jobs_dir(jobs,sizeof(jobs))!=0) return -1;
  n=snprintf(out,out_size,"%s/%s",jobs,job_id);
  if(n<0 || (size_t)n>=out_size) return -1;
  return 0;
}

static int baseline_path(const char *chat,char *out,size_t out_size) {
  char jobs[PATH_MAX];
  int n;

  if(ensure_jobs_dir(jobs,sizeof(jobs))!=0) return -1;
  n=snprintf(out,out_size,"%s/baseline_%s",jobs,chat);
  if(n<0 || (size_t)n>=out_size) return -1;
  return 0;
}

static int marker_exists(const char *job_id) {
  char path[PATH_MAX];

  if(marker_path(job_id,path,sizeof(path))!=0) return 0;
  return access(path,F_OK)==0;
}

static int baseline_exists(const char *chat) {
  char path[PATH_MAX];

  if(baseline_path(chat,path,sizeof(path))!=0) return 0;
  return access(path,F_OK)==0;
}

static int write_marker(const char *chat,const char *job_id,int exit_code,const char *reason) {
  char path[PATH_MAX],stamp[64];
  FILE *f;
  time_t now;
  struct tm tmv;

  if(marker_path(job_id,path,sizeof(path))!=0) return -1;
  now=time(NULL);
  localtime_r(&now,&tmv);
  strftime(stamp,sizeof(stamp),"%Y-%m-%dT%H:%M:%S%z",&tmv);
  f=fopen(path,"w");
  if(!f) return -1;
  fprintf(f,"chat=%s\n",chat);
  fprintf(f,"exit_code=%d\n",exit_code);
  fprintf(f,"marked_at=%s\n",stamp);
  fprintf(f,"reason=%s\n",reason);
  fclose(f);
  return 0;
}

static int write_baseline(const char *chat) {
  char path[PATH_MAX];
  FILE *f;

  if(baseline_path(chat,path,sizeof(path))!=0) return -1;
  f=fopen(path,"w");
  if(!f) return -1;
  fprintf(f,"chat=%s\n",chat);
  fclose(f);
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

static cJSON *query_jobs(const char *chat) {
  char token[512],auth[640];
  struct Buffer body;
  struct curl_slist *headers;
  CURL *curl;
  CURLcode rc;
  cJSON *req,*reply,*result,*content;
  char *json;
  long http_code;

  if(read_token(token,sizeof(token))!=0) return NULL;
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
  headers=curl_slist_append(headers,"MCP-Protocol-Version: 2026-07-28");
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
    fprintf(stderr,"MCP HTTP error: %s\n",curl_easy_strerror(rc));
    free(body.data);
    return NULL;
  }
  if(http_code!=200) {
    fprintf(stderr,"MCP HTTP status: %ld\n",http_code);
    free(body.data);
    return NULL;
  }
  reply=cJSON_Parse(body.data ? body.data : "");
  free(body.data);
  if(!reply) {
    fprintf(stderr,"invalid MCP JSON response\n");
    return NULL;
  }
  result=cJSON_GetObjectItemCaseSensitive(reply,"result");
  if(!cJSON_IsObject(result)) {
    cJSON_Delete(reply);
    fprintf(stderr,"MCP result missing\n");
    return NULL;
  }
  content=cJSON_GetObjectItemCaseSensitive(result,"structuredContent");
  if(!cJSON_IsArray(content)) {
    cJSON_Delete(reply);
    fprintf(stderr,"MCP jobs structuredContent missing\n");
    return NULL;
  }
  cJSON_DetachItemViaPointer(result,content);
  cJSON_Delete(reply);
  return content;
}

static int print_running_jobs(cJSON *jobs) {
  cJSON *item,*job_id,*state,*started_at,*elapsed,*command;
  long total;
  int i,n,count,days,hours,minutes,seconds;

  count=0;
  n=cJSON_GetArraySize(jobs);
  for(i=0;i<n;i++) {
    item=cJSON_GetArrayItem(jobs,i);
    job_id=cJSON_GetObjectItemCaseSensitive(item,"job_id");
    state=cJSON_GetObjectItemCaseSensitive(item,"state");
    started_at=cJSON_GetObjectItemCaseSensitive(item,"started_at");
    elapsed=cJSON_GetObjectItemCaseSensitive(item,"elapsed_seconds");
    command=cJSON_GetObjectItemCaseSensitive(item,"command");
    if(!cJSON_IsString(job_id) || !cJSON_IsString(state)) continue;
    if(strcmp(state->valuestring,"running")!=0) continue;
    total=cJSON_IsNumber(elapsed) ? (long)elapsed->valuedouble : 0;
    if(total<0) total=0;
    days=(int)(total/86400L);
    hours=(int)((total%86400L)/3600L);
    minutes=(int)((total%3600L)/60L);
    seconds=(int)(total%60L);
    printf("running %s\n",job_id->valuestring);
    printf("  started=%s\n",cJSON_IsString(started_at) ? started_at->valuestring : "unknown");
    if(days>0) printf("  elapsed=%dd %02d:%02d:%02d\n",days,hours,minutes,seconds);
    else printf("  elapsed=%02d:%02d:%02d\n",hours,minutes,seconds);
    printf("  command=%s\n",cJSON_IsString(command) ? command->valuestring : "unknown");
    count++;
  }
  return count;
}

static int collect_pending(cJSON *jobs,struct PendingJob *pending,int max_pending) {
  cJSON *item,*job_id,*state,*exit_code;
  int i,n,count;

  count=0;
  n=cJSON_GetArraySize(jobs);
  for(i=0;i<n;i++) {
    item=cJSON_GetArrayItem(jobs,i);
    job_id=cJSON_GetObjectItemCaseSensitive(item,"job_id");
    state=cJSON_GetObjectItemCaseSensitive(item,"state");
    exit_code=cJSON_GetObjectItemCaseSensitive(item,"exit_code");
    if(!cJSON_IsString(job_id) || !cJSON_IsString(state)) continue;
    if(strcmp(state->valuestring,"exited")!=0 || !cJSON_IsNumber(exit_code)) continue;
    if(marker_exists(job_id->valuestring)) continue;
    if(count>=max_pending) continue;
    snprintf(pending[count].job_id,sizeof(pending[count].job_id),"%s",job_id->valuestring);
    pending[count].exit_code=exit_code->valueint;
    count++;
  }
  return count;
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

static int candidate_id(cJSON *item,const char *conversation_id) {
  cJSON *type,*title,*url,*ws;
  char pattern[160];

  type=cJSON_GetObjectItemCaseSensitive(item,"type");
  title=cJSON_GetObjectItemCaseSensitive(item,"title");
  url=cJSON_GetObjectItemCaseSensitive(item,"url");
  ws=cJSON_GetObjectItemCaseSensitive(item,"webSocketDebuggerUrl");
  if(!cJSON_IsString(type) || strcmp(type->valuestring,"page")!=0) return 0;
  if(!cJSON_IsString(title) || !cJSON_IsString(url) || !cJSON_IsString(ws)) return 0;
  if(snprintf(pattern,sizeof(pattern),"/c/%s",conversation_id)>=(int)sizeof(pattern)) return 0;
  return strstr(url->valuestring,pattern)!=NULL;
}

static int find_target(const char *conversation_id,char **ws_url,char **title,char **url_out) {
  struct Buffer body;
  cJSON *root,*item,*cjs_title,*cjs_url,*cjs_ws;
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
    if(count==0) fprintf(stderr,"No ChatGPT tab with conversation id '%s'\n",conversation_id);
    else fprintf(stderr,"Refusing: %d ChatGPT tabs use conversation id '%s'\n",count,conversation_id);
    cJSON_Delete(root);
    return -1;
  }
  for(i=0;i<n;i++) {
    item=cJSON_GetArrayItem(root,i);
    if(!candidate_id(item,conversation_id)) continue;
    cjs_title=cJSON_GetObjectItemCaseSensitive(item,"title");
    cjs_url=cJSON_GetObjectItemCaseSensitive(item,"url");
    cjs_ws=cJSON_GetObjectItemCaseSensitive(item,"webSocketDebuggerUrl");
    *ws_url=strdup(cjs_ws->valuestring);
    *title=strdup(cjs_title->valuestring);
    *url_out=strdup(cjs_url->valuestring);
    cJSON_Delete(root);
    if(!*ws_url || !*title || !*url_out) return -1;
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
  char *ws_url,*title,*url;
  CURL *ws;
  CURLcode rc;
  int id,result;

  ws_url=NULL;
  title=NULL;
  url=NULL;
  ws=NULL;
  result=-1;
  if(find_target(conversation_id,&ws_url,&title,&url)!=0) goto done;
  printf("tab: %s\nurl: %s\n",title,url);
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
    fprintf(stderr,"Composer not ready: %s\n",value ? value : "invalid response");
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
  printf("OK: sent '%s' to tab '%s'\n",CHECK_TEXT,title);
  result=0;

done:
  if(ws) curl_easy_cleanup(ws);
  free(ws_url);
  free(title);
  free(url);
  return result;
}

static int do_baseline(const char *chat) {
  cJSON *jobs,*item,*job_id,*state,*exit_code;
  int i,n,marked;

  jobs=query_jobs(chat);
  if(!jobs) return 1;
  marked=0;
  n=cJSON_GetArraySize(jobs);
  for(i=0;i<n;i++) {
    item=cJSON_GetArrayItem(jobs,i);
    job_id=cJSON_GetObjectItemCaseSensitive(item,"job_id");
    state=cJSON_GetObjectItemCaseSensitive(item,"state");
    exit_code=cJSON_GetObjectItemCaseSensitive(item,"exit_code");
    if(!cJSON_IsString(job_id) || !cJSON_IsString(state) || !cJSON_IsNumber(exit_code)) continue;
    if(strcmp(state->valuestring,"exited")!=0) continue;
    if(!marker_exists(job_id->valuestring)) {
      if(write_marker(chat,job_id->valuestring,exit_code->valueint,"baseline")!=0) {
        cJSON_Delete(jobs);
        fprintf(stderr,"cannot write baseline marker for %s\n",job_id->valuestring);
        return 1;
      }
      marked++;
    }
  }
  cJSON_Delete(jobs);
  if(write_baseline(chat)!=0) {
    fprintf(stderr,"cannot write baseline marker for chat %s\n",chat);
    return 1;
  }
  printf("baseline chat=%s marked=%d\n",chat,marked);
  log_line("baseline completed");
  return 0;
}

static int ack_pending(const char *chat) {
  struct PendingJob pending[MAX_PENDING];
  cJSON *jobs;
  int count,i;

  if(!baseline_exists(chat)) {
    fprintf(stderr,"baseline missing for chat %s; run --baseline first\n",chat);
    return 2;
  }
  jobs=query_jobs(chat);
  if(!jobs) return 1;
  count=collect_pending(jobs,pending,MAX_PENDING);
  cJSON_Delete(jobs);
  for(i=0;i<count;i++) {
    if(write_marker(chat,pending[i].job_id,pending[i].exit_code,"ack")!=0) {
      fprintf(stderr,"cannot ack %s\n",pending[i].job_id);
      return 1;
    }
    printf("acked %s exit=%d\n",pending[i].job_id,pending[i].exit_code);
  }
  printf("chat=%s acked=%d\n",chat,count);
  return 0;
}

static int check_once(const char *chat,const char *conversation_id,int query_only,int show_running) {
  struct PendingJob pending[MAX_PENDING];
  cJSON *jobs;
  char line[256];
  int count,running,i;

  if(!baseline_exists(chat)) {
    fprintf(stderr,"baseline missing for chat %s; run --baseline first\n",chat);
    return 2;
  }
  jobs=query_jobs(chat);
  if(!jobs) return 1;
  count=collect_pending(jobs,pending,MAX_PENDING);
  running=show_running ? print_running_jobs(jobs) : 0;
  cJSON_Delete(jobs);
  if(show_running) printf("chat=%s running=%d pending=%d\n",chat,running,count);
  else printf("chat=%s pending=%d\n",chat,count);
  for(i=0;i<count;i++) printf("pending %s exit=%d\n",pending[i].job_id,pending[i].exit_code);
  if(query_only || count==0) return 0;
  if(send_check(conversation_id)!=0) {
    snprintf(line,sizeof(line),"chat=%s check failed pending=%d",chat,count);
    log_line(line);
    return 1;
  }
  for(i=0;i<count;i++) {
    if(write_marker(chat,pending[i].job_id,pending[i].exit_code,"notified")!=0)
      fprintf(stderr,"warning: cannot mark %s\n",pending[i].job_id);
  }
  snprintf(line,sizeof(line),"chat=%s check sent pending=%d",chat,count);
  log_line(line);
  return 0;
}

static void usage(const char *prog) {
  printf("usage:\n");
  printf("  %s --baseline <chat> <conversation-id>\n",prog);
  printf("  %s --query    <chat> <conversation-id>\n",prog);
  printf("  %s --ack      <chat> <conversation-id>\n",prog);
  printf("  %s            <chat> <conversation-id>\n",prog);
  printf("  %s --watch    <chat> <conversation-id>\n",prog);
  printf("\n");
  printf("visible files under ~/mcp/: token.txt, watcher.log, jobs/\n");
  printf("watch interval: %d seconds\n",WATCH_INTERVAL);
}

int main(int argc,char **argv) {
  const char *chat,*conversation_id;
  int mode,rc;

  mode=0;
  if(argc==2 && (strcmp(argv[1],"-h")==0 || strcmp(argv[1],"--help")==0)) {
    usage(argv[0]);
    return 0;
  }
  if(argc==4 && strcmp(argv[1],"--baseline")==0) mode=1;
  else if(argc==4 && strcmp(argv[1],"--query")==0) mode=2;
  else if(argc==4 && strcmp(argv[1],"--watch")==0) mode=3;
  else if(argc==4 && strcmp(argv[1],"--ack")==0) mode=5;
  else if(argc==3) mode=4;
  else {
    usage(argv[0]);
    return 2;
  }
  if(mode==4) {
    chat=argv[1];
    conversation_id=argv[2];
  } else {
    chat=argv[2];
    conversation_id=argv[3];
  }
  if(!valid_chat(chat)) {
    fprintf(stderr,"invalid chat: %s\n",chat);
    return 2;
  }
  if(!valid_conversation_id(conversation_id)) {
    fprintf(stderr,"invalid conversation id: %s\n",conversation_id);
    return 2;
  }
  if(curl_global_init(CURL_GLOBAL_DEFAULT)!=CURLE_OK) return 1;
  if(mode==1) rc=do_baseline(chat);
  else if(mode==2) rc=check_once(chat,conversation_id,1,1);
  else if(mode==5) rc=ack_pending(chat);
  else if(mode==4) rc=check_once(chat,conversation_id,0,0);
  else {
    rc=0;
    for(;;) {
      rc=check_once(chat,conversation_id,0,1);
      if(rc==2) break;
      sleep(WATCH_INTERVAL);
    }
  }
  curl_global_cleanup();
  return rc;
}
