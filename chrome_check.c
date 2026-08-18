// Gianluca Mazzini @2026- Version 1.01
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <unistd.h>
#include <poll.h>
#include <curl/curl.h>
#include <cjson/cJSON.h>

#define DEFAULT_HOST "http://127.0.0.1:9222"
#define DEFAULT_TAB "mymcp"
#define DEFAULT_TEXT "check"
#define HTTP_TIMEOUT_MS 3000L
#define WS_TIMEOUT_MS 5000

struct Buffer {
  char *data;
  size_t len;
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

static int contains_nocase(const char *s,const char *needle) {
  size_t i,j,n,m;

  n=strlen(s);
  m=strlen(needle);
  if(m==0) return 1;
  if(m>n) return 0;
  for(i=0;i<=n-m;i++) {
    for(j=0;j<m;j++) {
      if(tolower((unsigned char)s[i+j])!=tolower((unsigned char)needle[j])) break;
    }
    if(j==m) return 1;
  }
  return 0;
}

static int http_get(const char *url,struct Buffer *out) {
  CURL *curl;
  CURLcode rc;

  out->data=NULL;
  out->len=0;
  curl=curl_easy_init();
  if(!curl) return -1;
  curl_easy_setopt(curl,CURLOPT_URL,url);
  curl_easy_setopt(curl,CURLOPT_WRITEFUNCTION,write_cb);
  curl_easy_setopt(curl,CURLOPT_WRITEDATA,out);
  curl_easy_setopt(curl,CURLOPT_CONNECTTIMEOUT_MS,HTTP_TIMEOUT_MS);
  curl_easy_setopt(curl,CURLOPT_TIMEOUT_MS,HTTP_TIMEOUT_MS);
  curl_easy_setopt(curl,CURLOPT_NOSIGNAL,1L);
  rc=curl_easy_perform(curl);
  curl_easy_cleanup(curl);
  if(rc!=CURLE_OK) {
    fprintf(stderr,"HTTP error: %s\n",curl_easy_strerror(rc));
    free(out->data);
    out->data=NULL;
    out->len=0;
    return -1;
  }
  return 0;
}

static int candidate(cJSON *item,const char *tab,int exact) {
  cJSON *type,*title,*url,*ws;

  type=cJSON_GetObjectItemCaseSensitive(item,"type");
  title=cJSON_GetObjectItemCaseSensitive(item,"title");
  url=cJSON_GetObjectItemCaseSensitive(item,"url");
  ws=cJSON_GetObjectItemCaseSensitive(item,"webSocketDebuggerUrl");
  if(!cJSON_IsString(type) || strcmp(type->valuestring,"page")!=0) return 0;
  if(!cJSON_IsString(title) || !cJSON_IsString(url) || !cJSON_IsString(ws)) return 0;
  if(!contains_nocase(url->valuestring,"chatgpt.com/")) return 0;
  if(exact) return strcasecmp(title->valuestring,tab)==0;
  return contains_nocase(title->valuestring,tab);
}

static int valid_conversation_id(const char *id) {
  size_t i,n;

  n=strlen(id);
  if(n<8 || n>128) return 0;
  for(i=0;i<n;i++) {
    if(!isalnum((unsigned char)id[i]) && id[i]!='-' && id[i]!='_') return 0;
  }
  return 1;
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

static int find_target_id(const char *host,const char *conversation_id,char **ws_url,char **title_out,char **url_out) {
  struct Buffer body;
  cJSON *root,*item,*title,*url,*ws;
  char endpoint[512];
  int i,n,count;

  if(!valid_conversation_id(conversation_id)) {
    fprintf(stderr,"Invalid conversation id: %s\n",conversation_id);
    return -1;
  }
  if(snprintf(endpoint,sizeof(endpoint),"%s/json/list",host)>=(int)sizeof(endpoint)) return -1;
  if(http_get(endpoint,&body)!=0) return -1;
  root=cJSON_Parse(body.data);
  free(body.data);
  if(!cJSON_IsArray(root)) {
    fprintf(stderr,"Invalid Chrome /json/list response\n");
    cJSON_Delete(root);
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
    title=cJSON_GetObjectItemCaseSensitive(item,"title");
    url=cJSON_GetObjectItemCaseSensitive(item,"url");
    ws=cJSON_GetObjectItemCaseSensitive(item,"webSocketDebuggerUrl");
    *ws_url=strdup(ws->valuestring);
    *title_out=strdup(title->valuestring);
    *url_out=strdup(url->valuestring);
    cJSON_Delete(root);
    if(!*ws_url || !*title_out || !*url_out) return -1;
    return 0;
  }
  cJSON_Delete(root);
  return -1;
}

static int find_target(const char *host,const char *tab,char **ws_url,char **title_out,char **url_out) {
  struct Buffer body;
  cJSON *root,*item,*title,*url,*ws;
  char endpoint[512];
  int i,n,count,exact;

  if(snprintf(endpoint,sizeof(endpoint),"%s/json/list",host)>=(int)sizeof(endpoint)) return -1;
  if(http_get(endpoint,&body)!=0) return -1;
  root=cJSON_Parse(body.data);
  free(body.data);
  if(!cJSON_IsArray(root)) {
    fprintf(stderr,"Invalid Chrome /json/list response\n");
    cJSON_Delete(root);
    return -1;
  }
  n=cJSON_GetArraySize(root);
  count=0;
  exact=1;
  for(i=0;i<n;i++) {
    item=cJSON_GetArrayItem(root,i);
    if(candidate(item,tab,1)) count++;
  }
  if(count==0) {
    exact=0;
    for(i=0;i<n;i++) {
      item=cJSON_GetArrayItem(root,i);
      if(candidate(item,tab,0)) count++;
    }
  }
  if(count!=1) {
    if(count==0) fprintf(stderr,"No ChatGPT tab matching '%s'\n",tab);
    else fprintf(stderr,"Refusing: %d ChatGPT tabs match '%s'\n",count,tab);
    cJSON_Delete(root);
    return -1;
  }
  for(i=0;i<n;i++) {
    item=cJSON_GetArrayItem(root,i);
    if(!candidate(item,tab,exact)) continue;
    title=cJSON_GetObjectItemCaseSensitive(item,"title");
    url=cJSON_GetObjectItemCaseSensitive(item,"url");
    ws=cJSON_GetObjectItemCaseSensitive(item,"webSocketDebuggerUrl");
    *ws_url=strdup(ws->valuestring);
    *title_out=strdup(title->valuestring);
    *url_out=strdup(url->valuestring);
    cJSON_Delete(root);
    if(!*ws_url || !*title_out || !*url_out) return -1;
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
  if(n<=0) return -1;
  return 0;
}

static int ws_send_text(CURL *ws,const char *text) {
  CURLcode rc;
  size_t off,sent,len;

  off=0;
  len=strlen(text);
  for(;;) {
    sent=0;
    rc=curl_ws_send(ws,text+off,len-off,&sent,0,CURLWS_TEXT);
    off+=sent;
    if(off==len) return 0;
    if(rc==CURLE_AGAIN) {
      if(wait_socket(ws,POLLOUT,WS_TIMEOUT_MS)!=0) return -1;
      continue;
    }
    if(rc!=CURLE_OK) {
      fprintf(stderr,"WebSocket send error: %s\n",curl_easy_strerror(rc));
      return -1;
    }
  }
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
      fprintf(stderr,"WebSocket receive error: %s\n",curl_easy_strerror(rc));
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
    root=cJSON_Parse(msg);
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

static int focus_empty_composer(CURL *ws,int *id) {
  const char *expr;
  const char *value;
  cJSON *params,*reply;

  expr="(() => {"
       "const visible=e=>{const r=e.getBoundingClientRect();return r.width>0&&r.height>0;};"
       "const e=document.querySelector('#prompt-textarea')||"
       "[...document.querySelectorAll('textarea,[contenteditable=\"true\"]')].find(visible);"
       "if(!e)return 'NO_COMPOSER';"
       "if(e.disabled||e.getAttribute('aria-disabled')==='true')return 'DISABLED';"
       "const t=('value' in e?e.value:e.innerText).trim();"
       "if(t)return 'BUSY';"
       "e.focus();"
       "return document.activeElement===e?'READY':'NO_FOCUS';"
       "})()";
  params=cJSON_CreateObject();
  cJSON_AddStringToObject(params,"expression",expr);
  cJSON_AddBoolToObject(params,"returnByValue",1);
  cJSON_AddBoolToObject(params,"userGesture",1);
  reply=NULL;
  if(cdp_call(ws,(*id)++,"Runtime.evaluate",params,&reply)!=0) return -1;
  value=eval_string(reply);
  if(!value || strcmp(value,"READY")!=0) {
    fprintf(stderr,"Composer not ready: %s\n",value ? value : "invalid response");
    cJSON_Delete(reply);
    return -1;
  }
  cJSON_Delete(reply);
  return 0;
}

static int insert_text(CURL *ws,int *id,const char *text) {
  cJSON *params;

  params=cJSON_CreateObject();
  cJSON_AddStringToObject(params,"text",text);
  return cdp_call(ws,(*id)++,"Input.insertText",params,NULL);
}

static int press_enter(CURL *ws,int *id) {
  cJSON *params;

  params=cJSON_CreateObject();
  cJSON_AddStringToObject(params,"type","rawKeyDown");
  cJSON_AddStringToObject(params,"key","Enter");
  cJSON_AddStringToObject(params,"code","Enter");
  cJSON_AddNumberToObject(params,"windowsVirtualKeyCode",13);
  cJSON_AddNumberToObject(params,"nativeVirtualKeyCode",13);
  if(cdp_call(ws,(*id)++,"Input.dispatchKeyEvent",params,NULL)!=0) return -1;
  params=cJSON_CreateObject();
  cJSON_AddStringToObject(params,"type","keyUp");
  cJSON_AddStringToObject(params,"key","Enter");
  cJSON_AddStringToObject(params,"code","Enter");
  cJSON_AddNumberToObject(params,"windowsVirtualKeyCode",13);
  cJSON_AddNumberToObject(params,"nativeVirtualKeyCode",13);
  return cdp_call(ws,(*id)++,"Input.dispatchKeyEvent",params,NULL);
}

static void usage(const char *prog) {
  fprintf(stderr,"usage: %s [tab-title] [text] [debug-url]\n",prog);
  fprintf(stderr,"       %s --id conversation-id [text] [debug-url]\n",prog);
  fprintf(stderr,"default: tab='%s' text='%s' debug-url='%s'\n",DEFAULT_TAB,DEFAULT_TEXT,DEFAULT_HOST);
}

int main(int argc,char **argv) {
  const char *tab,*text,*host,*conversation_id;
  int id_mode;
  char *ws_url,*title,*url;
  CURL *ws;
  CURLcode rc;
  int id,result;

  id_mode=0;
  conversation_id=NULL;
  if(argc>1 && (strcmp(argv[1],"-h")==0 || strcmp(argv[1],"--help")==0)) {
    usage(argv[0]);
    return 0;
  }
  if(argc>1 && strcmp(argv[1],"--id")==0) {
    if(argc<3 || argc>5) {
      usage(argv[0]);
      return 2;
    }
    id_mode=1;
    conversation_id=argv[2];
    tab=NULL;
    text=argc>3 ? argv[3] : DEFAULT_TEXT;
    host=argc>4 ? argv[4] : DEFAULT_HOST;
  } else {
    if(argc>4) {
      usage(argv[0]);
      return 2;
    }
    tab=argc>1 ? argv[1] : DEFAULT_TAB;
    text=argc>2 ? argv[2] : DEFAULT_TEXT;
    host=argc>3 ? argv[3] : DEFAULT_HOST;
  }
  ws_url=NULL;
  title=NULL;
  url=NULL;
  ws=NULL;
  result=1;
  if(curl_global_init(CURL_GLOBAL_DEFAULT)!=CURLE_OK) return 1;
  if(id_mode) {
    if(find_target_id(host,conversation_id,&ws_url,&title,&url)!=0) goto done;
    printf("conversation_id: %s\n",conversation_id);
  } else {
    if(find_target(host,tab,&ws_url,&title,&url)!=0) goto done;
  }
  printf("tab: %s\nurl: %s\n",title,url);
  ws=curl_easy_init();
  if(!ws) goto done;
  curl_easy_setopt(ws,CURLOPT_URL,ws_url);
  curl_easy_setopt(ws,CURLOPT_CONNECT_ONLY,2L);
  curl_easy_setopt(ws,CURLOPT_CONNECTTIMEOUT_MS,HTTP_TIMEOUT_MS);
  curl_easy_setopt(ws,CURLOPT_NOSIGNAL,1L);
  rc=curl_easy_perform(ws);
  if(rc!=CURLE_OK) {
    fprintf(stderr,"Chrome WebSocket error: %s\n",curl_easy_strerror(rc));
    goto done;
  }
  id=1;
  if(focus_empty_composer(ws,&id)!=0) goto done;
  if(insert_text(ws,&id,text)!=0) goto done;
  usleep(100000);
  if(press_enter(ws,&id)!=0) goto done;
  printf("OK: sent '%s' to tab '%s'\n",text,title);
  result=0;

done:
  if(ws) curl_easy_cleanup(ws);
  free(ws_url);
  free(title);
  free(url);
  curl_global_cleanup();
  return result;
}
