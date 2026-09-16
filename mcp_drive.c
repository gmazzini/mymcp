// Gianluca Mazzini @2026- Version 1.02

#include "mcp_drive.h"

#include <curl/curl.h>
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define DRIVE_MAP_DEFAULT "/home/tools/mcp/drive.map"
#define GOOGLEAUTH_CONFIG_DEFAULT "/home/tools/mcp/mymcp.conf"
#define GOOGLEAUTH_URL_DEFAULT "https://google.mazzini.org/googleauth"
#define GOOGLEAUTH_CHANNEL "mymcp"
#define DRIVE_STAGE_DEFAULT "/home/tools/mcp/drive-stage"
#define DRIVE_API_DEFAULT "https://www.googleapis.com/drive/v3"
#define DRIVE_UPLOAD_DEFAULT "https://www.googleapis.com/upload/drive/v3"
#define DRIVE_ALIAS_MAX 64
#define DRIVE_ID_MAX 192
#define DRIVE_NAME_MAX 1024
#define DRIVE_MIME_MAX 256
#define DRIVE_VERSION_MAX 64
#define DRIVE_TIME_MAX 64
#define DRIVE_PATH_MAX 4096
#define DRIVE_TOKEN_MAX 4096
#define DRIVE_ERROR_MAX 512
#define DRIVE_MAX_MAP 256
#define DRIVE_MAX_LIST 10000
#define DRIVE_MAX_DEPTH 64
#define DRIVE_HTTP_TIMEOUT 120L
#define DRIVE_CONNECT_TIMEOUT 15L

struct DriveMapEntry {
  char alias[DRIVE_ALIAS_MAX+1];
  char id[DRIVE_ID_MAX+1];
  int writable;
};

struct DriveFile {
  char id[DRIVE_ID_MAX+1];
  char name[DRIVE_NAME_MAX+1];
  char mime[DRIVE_MIME_MAX+1];
  char version[DRIVE_VERSION_MAX+1];
  char modified[DRIVE_TIME_MAX+1];
  long long size;
  int size_known;
  int folder;
};

struct DriveBuffer {
  unsigned char *data;
  size_t len;
  size_t capacity;
};

struct DriveFileSink {
  FILE *file;
  unsigned long long bytes;
};

struct DriveSession {
  CURL *curl;
  char auth[DRIVE_TOKEN_MAX+32];
  int global_initialized;
};

struct DriveStageMeta {
  char chat[65];
  char path[DRIVE_PATH_MAX];
  char version[DRIVE_VERSION_MAX+1];
  char id[DRIVE_ID_MAX+1];
  int existed;
};

static void drive_error(char *error,size_t error_size,const char *text) {
  if(error==NULL || error_size==0) return;
  if(text==NULL) text="Google Drive operation failed";
  snprintf(error,error_size,"%s",text);
}

static const char *drive_env(const char *name,const char *fallback) {
  const char *value;

  value=getenv(name);
  if(value!=NULL && value[0]!=0) return value;
  return fallback;
}

static const char *drive_map_path(void) {
  return drive_env("MYMCP_DRIVE_MAP",DRIVE_MAP_DEFAULT);
}

static const char *drive_googleauth_config_path(void) {
  return drive_env("MYMCP_GOOGLEAUTH_CONFIG",GOOGLEAUTH_CONFIG_DEFAULT);
}

static const char *drive_googleauth_url(void) {
  return drive_env("MYMCP_GOOGLEAUTH_URL",GOOGLEAUTH_URL_DEFAULT);
}

static const char *drive_googleauth_channel(void) {
  return drive_env("MYMCP_GOOGLEAUTH_CHANNEL",GOOGLEAUTH_CHANNEL);
}

static const char *drive_stage_path(void) {
  return drive_env("MYMCP_DRIVE_STAGE",DRIVE_STAGE_DEFAULT);
}

static const char *drive_api_base(void) {
  return drive_env("MYMCP_DRIVE_API",DRIVE_API_DEFAULT);
}

static const char *drive_upload_base(void) {
  return drive_env("MYMCP_DRIVE_UPLOAD_API",DRIVE_UPLOAD_DEFAULT);
}

static int drive_valid_alias(const char *text) {
  size_t i,len;
  unsigned char c;

  if(text==NULL) return 0;
  len=strlen(text);
  if(len<1 || len>DRIVE_ALIAS_MAX) return 0;
  for(i=0;i<len;i++) {
    c=(unsigned char)text[i];
    if(!isalnum(c) && c!='_' && c!='-' && c!='.') return 0;
  }
  return 1;
}

static int drive_valid_id(const char *text) {
  size_t i,len;
  unsigned char c;

  if(text==NULL) return 0;
  len=strlen(text);
  if(len<1 || len>DRIVE_ID_MAX) return 0;
  for(i=0;i<len;i++) {
    c=(unsigned char)text[i];
    if(!isalnum(c) && c!='_' && c!='-') return 0;
  }
  return 1;
}

static int drive_valid_name(const char *text) {
  size_t i,len;
  unsigned char c;

  if(text==NULL) return 0;
  len=strlen(text);
  if(len<1 || len>DRIVE_NAME_MAX) return 0;
  if(strcmp(text,".")==0 || strcmp(text,"..")==0) return 0;
  for(i=0;i<len;i++) {
    c=(unsigned char)text[i];
    if(c=='/' || c=='\\' || c<32 || c==127) return 0;
  }
  return 1;
}

static int drive_valid_path(const char *path) {
  char copy[DRIVE_PATH_MAX],*p,*slash;

  if(path==NULL || path[0]==0 || path[0]=='/' || strlen(path)>=sizeof(copy)) return 0;
  strcpy(copy,path);
  p=copy;
  for(;;) {
    slash=strchr(p,'/');
    if(slash!=NULL) *slash=0;
    if(!drive_valid_name(p)) return 0;
    if(slash==NULL) break;
    p=slash+1;
    if(*p==0) return 0;
  }
  return 1;
}

static int drive_map_lookup(const char *alias,struct DriveMapEntry *entry,char *error,size_t error_size) {
  struct DriveMapEntry items[DRIVE_MAX_MAP];
  FILE *f;
  char line[2048],a[DRIVE_ALIAS_MAX+1],id[DRIVE_ID_MAX+1],mode[16],extra[16];
  char *hash,*p;
  int count,n,i,found;

  if(!drive_valid_alias(alias)) {
    drive_error(error,error_size,"invalid Drive alias");
    return 0;
  }
  f=fopen(drive_map_path(),"r");
  if(f==NULL) {
    snprintf(line,sizeof(line),"cannot read Drive map %s: %s",drive_map_path(),strerror(errno));
    drive_error(error,error_size,line);
    return 0;
  }
  count=0;
  found=-1;
  for(;fgets(line,sizeof(line),f)!=NULL;) {
    hash=strchr(line,'#');
    if(hash!=NULL) *hash=0;
    p=line;
    for(;*p!=0 && isspace((unsigned char)*p);p++);
    if(*p==0) continue;
    extra[0]=0;
    n=sscanf(p,"%64s %192s %15s %15s",a,id,mode,extra);
    if(n!=3 || !drive_valid_alias(a) || !drive_valid_id(id) || (strcmp(mode,"ro")!=0 && strcmp(mode,"rw")!=0)) {
      fclose(f);
      drive_error(error,error_size,"malformed Drive map; all Drive operations are disabled");
      return 0;
    }
    if(count>=DRIVE_MAX_MAP) {
      fclose(f);
      drive_error(error,error_size,"Drive map has too many entries");
      return 0;
    }
    for(i=0;i<count;i++) {
      if(strcmp(items[i].alias,a)==0) {
        fclose(f);
        drive_error(error,error_size,"duplicate alias in Drive map");
        return 0;
      }
    }
    snprintf(items[count].alias,sizeof(items[count].alias),"%s",a);
    snprintf(items[count].id,sizeof(items[count].id),"%s",id);
    items[count].writable=strcmp(mode,"rw")==0;
    if(strcmp(a,alias)==0) found=count;
    count++;
  }
  if(ferror(f)) {
    fclose(f);
    drive_error(error,error_size,"cannot read complete Drive map");
    return 0;
  }
  fclose(f);
  if(found<0) {
    drive_error(error,error_size,"Drive alias is not authorized");
    return 0;
  }
  *entry=items[found];
  return 1;
}

static int drive_path_alias(const char *path,char *alias,size_t alias_size) {
  const char *slash;
  size_t len;

  slash=strchr(path,'/');
  len=slash==NULL?strlen(path):(size_t)(slash-path);
  if(len<1 || len>=alias_size) return 0;
  memcpy(alias,path,len);
  alias[len]=0;
  return drive_valid_alias(alias);
}

static char *drive_trim(char *text) {
  char *end;

  for(;*text!=0 && isspace((unsigned char)*text);text++);
  end=text+strlen(text);
  for(;end>text && isspace((unsigned char)end[-1]);end--) end[-1]=0;
  return text;
}

static int drive_read_googleauth_key(char *key,size_t key_size,char *error,size_t error_size) {
  FILE *f;
  char line[2048],*p,*eq,*name,*value;
  int found;

  struct stat st;
  const char *path;

  key[0]=0;
  path=drive_googleauth_config_path();
  if(lstat(path,&st)!=0 || !S_ISREG(st.st_mode) || S_ISLNK(st.st_mode)) {
    drive_error(error,error_size,"cannot read mymcp Google authentication config");
    return 0;
  }
  if((st.st_mode&(S_IWGRP|S_IRWXO))!=0) {
    drive_error(error,error_size,"insecure permissions on mymcp Google authentication config");
    return 0;
  }
  f=fopen(path,"r");
  if(f==NULL) {
    drive_error(error,error_size,"cannot read mymcp Google authentication config");
    return 0;
  }
  found=0;
  for(;fgets(line,sizeof(line),f)!=NULL;) {
    p=drive_trim(line);
    if(*p==0 || *p=='#') continue;
    eq=strchr(p,'=');
    if(eq==NULL) continue;
    *eq=0;
    name=drive_trim(p);
    value=drive_trim(eq+1);
    if(strcmp(name,"googleauth_key")!=0) continue;
    if(found || *value==0 || strlen(value)>=key_size) {
      fclose(f);
      memset(key,0,key_size);
      drive_error(error,error_size,"invalid googleauth_key in mymcp config");
      return 0;
    }
    snprintf(key,key_size,"%s",value);
    found=1;
  }
  if(ferror(f)) {
    fclose(f);
    memset(key,0,key_size);
    drive_error(error,error_size,"cannot read complete mymcp Google authentication config");
    return 0;
  }
  fclose(f);
  if(!found) {
    drive_error(error,error_size,"googleauth_key missing from mymcp config");
    return 0;
  }
  return 1;
}

static size_t drive_write_callback(void *ptr,size_t size,size_t nmemb,void *userdata);
static size_t drive_file_write_callback(void *ptr,size_t size,size_t nmemb,void *userdata);

static int drive_googleauth_token(CURL *curl,char *token,size_t token_size,char *error,size_t error_size) {
  struct curl_slist *headers;
  struct DriveBuffer response;
  char key[512],payload[2048];
  char *escaped;
  CURLcode rc;
  long http;
  size_t len;
  int n;

  token[0]=0;
  key[0]=0;
  if(!drive_valid_alias(drive_googleauth_channel())) {
    drive_error(error,error_size,"invalid Google authentication channel");
    return 0;
  }
  if(!drive_read_googleauth_key(key,sizeof(key),error,error_size)) return 0;
  escaped=curl_easy_escape(curl,key,0);
  if(escaped==NULL) {
    memset(key,0,sizeof(key));
    drive_error(error,error_size,"cannot encode Google authentication request");
    return 0;
  }
  n=snprintf(payload,sizeof(payload),"action=token&channel=%s&key=%s",drive_googleauth_channel(),escaped);
  curl_free(escaped);
  memset(key,0,sizeof(key));
  if(n<0 || (size_t)n>=sizeof(payload)) {
    memset(payload,0,sizeof(payload));
    drive_error(error,error_size,"Google authentication request is too large");
    return 0;
  }
  memset(&response,0,sizeof(response));
  headers=curl_slist_append(NULL,"Content-Type: application/x-www-form-urlencoded");
  if(headers==NULL) {
    memset(payload,0,sizeof(payload));
    drive_error(error,error_size,"out of memory");
    return 0;
  }
  curl_easy_reset(curl);
  curl_easy_setopt(curl,CURLOPT_URL,drive_googleauth_url());
  curl_easy_setopt(curl,CURLOPT_HTTPHEADER,headers);
  curl_easy_setopt(curl,CURLOPT_POST,1L);
  curl_easy_setopt(curl,CURLOPT_POSTFIELDS,payload);
  curl_easy_setopt(curl,CURLOPT_POSTFIELDSIZE,(long)strlen(payload));
  curl_easy_setopt(curl,CURLOPT_WRITEFUNCTION,drive_write_callback);
  curl_easy_setopt(curl,CURLOPT_WRITEDATA,&response);
  curl_easy_setopt(curl,CURLOPT_SSL_VERIFYPEER,1L);
  curl_easy_setopt(curl,CURLOPT_SSL_VERIFYHOST,2L);
  curl_easy_setopt(curl,CURLOPT_CONNECTTIMEOUT,DRIVE_CONNECT_TIMEOUT);
  curl_easy_setopt(curl,CURLOPT_TIMEOUT,60L);
  curl_easy_setopt(curl,CURLOPT_USERAGENT,"mymcp/1.10");
  rc=curl_easy_perform(curl);
  http=0;
  if(rc==CURLE_OK) curl_easy_getinfo(curl,CURLINFO_RESPONSE_CODE,&http);
  memset(payload,0,sizeof(payload));
  curl_slist_free_all(headers);
  if(rc!=CURLE_OK || http!=200) {
    if(response.data!=NULL) {
      memset(response.data,0,response.len);
      free(response.data);
    }
    drive_error(error,error_size,"cannot obtain Google access token from googleauth");
    return 0;
  }
  len=response.len;
  for(;len>0 && isspace((unsigned char)response.data[len-1]);len--);
  if(len==0 || len>=token_size) {
    if(response.data!=NULL) {
      memset(response.data,0,response.len);
      free(response.data);
    }
    drive_error(error,error_size,"invalid Google access token returned by googleauth");
    return 0;
  }
  memcpy(token,response.data,len);
  token[len]=0;
  memset(response.data,0,response.len);
  free(response.data);
  return 1;
}

static int drive_session_open(struct DriveSession *session,char *error,size_t error_size) {
  char token[DRIVE_TOKEN_MAX];

  memset(session,0,sizeof(*session));
  token[0]=0;
  if(curl_global_init(CURL_GLOBAL_DEFAULT)!=CURLE_OK) {
    drive_error(error,error_size,"cannot initialize libcurl");
    return 0;
  }
  session->global_initialized=1;
  session->curl=curl_easy_init();
  if(session->curl==NULL) {
    curl_global_cleanup();
    session->global_initialized=0;
    drive_error(error,error_size,"cannot initialize Drive HTTP client");
    return 0;
  }
  if(!drive_googleauth_token(session->curl,token,sizeof(token),error,error_size)) {
    curl_easy_cleanup(session->curl);
    curl_global_cleanup();
    session->global_initialized=0;
    session->curl=NULL;
    memset(token,0,sizeof(token));
    return 0;
  }
  if(snprintf(session->auth,sizeof(session->auth),"Authorization: Bearer %s",token)<0 || strlen(session->auth)>=sizeof(session->auth)-1) {
    memset(token,0,sizeof(token));
    curl_easy_cleanup(session->curl);
    curl_global_cleanup();
    session->global_initialized=0;
    session->curl=NULL;
    drive_error(error,error_size,"Drive token is too long");
    return 0;
  }
  memset(token,0,sizeof(token));
  return 1;
}

static void drive_session_close(struct DriveSession *session) {
  if(session->curl!=NULL) curl_easy_cleanup(session->curl);
  session->curl=NULL;
  if(session->global_initialized) curl_global_cleanup();
  session->global_initialized=0;
}

static size_t drive_write_callback(void *ptr,size_t size,size_t nmemb,void *userdata) {
  struct DriveBuffer *buffer;
  unsigned char *next;
  size_t bytes,need,capacity;

  buffer=(struct DriveBuffer *)userdata;
  bytes=size*nmemb;
  if(bytes==0) return 0;
  if(buffer->len>SIZE_MAX-bytes-1) return 0;
  need=buffer->len+bytes+1;
  if(need>buffer->capacity) {
    capacity=buffer->capacity==0?4096:buffer->capacity;
    for(;capacity<need;) {
      if(capacity>SIZE_MAX/2) { capacity=need; break; }
      capacity*=2;
    }
    next=(unsigned char *)realloc(buffer->data,capacity);
    if(next==NULL) return 0;
    buffer->data=next;
    buffer->capacity=capacity;
  }
  memcpy(buffer->data+buffer->len,ptr,bytes);
  buffer->len+=bytes;
  buffer->data[buffer->len]=0;
  return bytes;
}

static size_t drive_file_write_callback(void *ptr,size_t size,size_t nmemb,void *userdata) {
  struct DriveFileSink *sink;
  size_t bytes,written;

  sink=(struct DriveFileSink *)userdata;
  if(size!=0 && nmemb>SIZE_MAX/size) return 0;
  bytes=size*nmemb;
  if(bytes==0) return 0;
  written=fwrite(ptr,1,bytes,sink->file);
  sink->bytes+=(unsigned long long)written;
  return written;
}

static void drive_buffer_free(struct DriveBuffer *buffer) {
  free(buffer->data);
  buffer->data=NULL;
  buffer->len=0;
  buffer->capacity=0;
}

static int drive_http(struct DriveSession *session,const char *method,const char *url,const char *content_type,const void *body,size_t body_len,const char *range,struct DriveBuffer *response,long *http,char *error,size_t error_size) {
  struct curl_slist *headers;
  CURLcode rc;
  char content_header[320];

  headers=NULL;
  memset(response,0,sizeof(*response));
  headers=curl_slist_append(headers,session->auth);
  headers=curl_slist_append(headers,"Accept: application/json");
  if(content_type!=NULL) {
    snprintf(content_header,sizeof(content_header),"Content-Type: %s",content_type);
    headers=curl_slist_append(headers,content_header);
    headers=curl_slist_append(headers,"Expect:");
  }
  if(headers==NULL) {
    drive_error(error,error_size,"out of memory");
    return 0;
  }
  curl_easy_reset(session->curl);
  curl_easy_setopt(session->curl,CURLOPT_URL,url);
  curl_easy_setopt(session->curl,CURLOPT_HTTPHEADER,headers);
  curl_easy_setopt(session->curl,CURLOPT_WRITEFUNCTION,drive_write_callback);
  curl_easy_setopt(session->curl,CURLOPT_WRITEDATA,response);
  curl_easy_setopt(session->curl,CURLOPT_SSL_VERIFYPEER,1L);
  curl_easy_setopt(session->curl,CURLOPT_SSL_VERIFYHOST,2L);
  curl_easy_setopt(session->curl,CURLOPT_CONNECTTIMEOUT,DRIVE_CONNECT_TIMEOUT);
  curl_easy_setopt(session->curl,CURLOPT_TIMEOUT,DRIVE_HTTP_TIMEOUT);
  curl_easy_setopt(session->curl,CURLOPT_USERAGENT,"mymcp-drive/1.02");
  if(strcmp(method,"GET")!=0) curl_easy_setopt(session->curl,CURLOPT_CUSTOMREQUEST,method);
  if(body!=NULL || body_len>0) {
    curl_easy_setopt(session->curl,CURLOPT_POSTFIELDS,body);
    curl_easy_setopt(session->curl,CURLOPT_POSTFIELDSIZE_LARGE,(curl_off_t)body_len);
  }
  if(range!=NULL) curl_easy_setopt(session->curl,CURLOPT_RANGE,range);
  rc=curl_easy_perform(session->curl);
  if(rc!=CURLE_OK) {
    snprintf(content_header,sizeof(content_header),"Drive HTTP error: %s",curl_easy_strerror(rc));
    drive_error(error,error_size,content_header);
    curl_slist_free_all(headers);
    drive_buffer_free(response);
    return 0;
  }
  curl_easy_getinfo(session->curl,CURLINFO_RESPONSE_CODE,http);
  curl_slist_free_all(headers);
  return 1;
}

static int drive_http_json(struct DriveSession *session,const char *method,const char *url,const char *content_type,const void *body,size_t body_len,cJSON **json,long *http,char *error,size_t error_size) {
  struct DriveBuffer response;
  cJSON *root,*message,*err_obj;
  char text[DRIVE_ERROR_MAX];

  *json=NULL;
  if(!drive_http(session,method,url,content_type,body,body_len,NULL,&response,http,error,error_size)) return 0;
  root=cJSON_ParseWithLength((const char *)(response.data!=NULL?response.data:(unsigned char *)""),response.len);
  if(*http<200 || *http>=300) {
    text[0]=0;
    if(cJSON_IsObject(root)) {
      err_obj=cJSON_GetObjectItemCaseSensitive(root,"error");
      message=cJSON_IsObject(err_obj)?cJSON_GetObjectItemCaseSensitive(err_obj,"message"):NULL;
      if(cJSON_IsString(message) && message->valuestring!=NULL) snprintf(text,sizeof(text),"Google Drive HTTP %ld: %s",*http,message->valuestring);
    }
    if(text[0]==0) snprintf(text,sizeof(text),"Google Drive HTTP %ld",*http);
    cJSON_Delete(root);
    drive_buffer_free(&response);
    drive_error(error,error_size,text);
    return 0;
  }
  drive_buffer_free(&response);
  if(!cJSON_IsObject(root)) {
    cJSON_Delete(root);
    drive_error(error,error_size,"invalid JSON response from Google Drive");
    return 0;
  }
  *json=root;
  return 1;
}

static void drive_copy_json_string(cJSON *root,const char *name,char *out,size_t out_size) {
  cJSON *item;

  if(out_size==0) return;
  out[0]=0;
  item=cJSON_GetObjectItemCaseSensitive(root,name);
  if(cJSON_IsString(item) && item->valuestring!=NULL) snprintf(out,out_size,"%s",item->valuestring);
}

static int drive_parse_file(cJSON *root,struct DriveFile *file,char *error,size_t error_size) {
  cJSON *size;
  char *end;
  long long value;

  memset(file,0,sizeof(*file));
  drive_copy_json_string(root,"id",file->id,sizeof(file->id));
  drive_copy_json_string(root,"name",file->name,sizeof(file->name));
  drive_copy_json_string(root,"mimeType",file->mime,sizeof(file->mime));
  drive_copy_json_string(root,"version",file->version,sizeof(file->version));
  drive_copy_json_string(root,"modifiedTime",file->modified,sizeof(file->modified));
  if(file->id[0]==0 || file->mime[0]==0) {
    drive_error(error,error_size,"incomplete file metadata from Google Drive");
    return 0;
  }
  file->folder=strcmp(file->mime,"application/vnd.google-apps.folder")==0;
  file->size_known=0;
  file->size=0;
  size=cJSON_GetObjectItemCaseSensitive(root,"size");
  if(cJSON_IsString(size) && size->valuestring!=NULL) {
    errno=0;
    value=strtoll(size->valuestring,&end,10);
    if(errno==0 && *size->valuestring!=0 && *end==0 && value>=0) {
      file->size=value;
      file->size_known=1;
    }
  } else if(cJSON_IsNumber(size) && size->valuedouble>=0.0) {
    file->size=(long long)size->valuedouble;
    file->size_known=1;
  }
  return 1;
}

static int drive_get_file(struct DriveSession *session,const char *id,struct DriveFile *file,char *error,size_t error_size) {
  char url[2048];
  cJSON *json;
  long http;
  int n,ok;

  n=snprintf(url,sizeof(url),"%s/files/%s?fields=id,name,mimeType,size,version,modifiedTime&supportsAllDrives=true",drive_api_base(),id);
  if(n<0 || (size_t)n>=sizeof(url)) {
    drive_error(error,error_size,"Drive URL is too long");
    return 0;
  }
  json=NULL;
  if(!drive_http_json(session,"GET",url,NULL,NULL,0,&json,&http,error,error_size)) return 0;
  ok=drive_parse_file(json,file,error,error_size);
  cJSON_Delete(json);
  return ok;
}

static char *drive_query_escape(const char *text) {
  char *out;
  size_t i,j,len;

  len=strlen(text);
  out=(char *)malloc(len*2+1);
  if(out==NULL) return NULL;
  j=0;
  for(i=0;i<len;i++) {
    if(text[i]=='\\' || text[i]=='\'') out[j++]='\\';
    out[j++]=text[i];
  }
  out[j]=0;
  return out;
}

static int drive_find_child(struct DriveSession *session,const char *parent,const char *name,struct DriveFile *file,int *found,char *error,size_t error_size) {
  char query[DRIVE_PATH_MAX+512],url[8192];
  char *escaped_name,*encoded;
  cJSON *json,*files,*item;
  long http;
  int count,n,ok;

  *found=0;
  escaped_name=drive_query_escape(name);
  if(escaped_name==NULL) {
    drive_error(error,error_size,"out of memory");
    return 0;
  }
  n=snprintf(query,sizeof(query),"'%s' in parents and name = '%s' and trashed = false",parent,escaped_name);
  free(escaped_name);
  if(n<0 || (size_t)n>=sizeof(query)) {
    drive_error(error,error_size,"Drive query is too long");
    return 0;
  }
  encoded=curl_easy_escape(session->curl,query,0);
  if(encoded==NULL) {
    drive_error(error,error_size,"cannot encode Drive query");
    return 0;
  }
  n=snprintf(url,sizeof(url),"%s/files?q=%s&fields=files(id,name,mimeType,size,version,modifiedTime)&pageSize=2&supportsAllDrives=true&includeItemsFromAllDrives=true",drive_api_base(),encoded);
  curl_free(encoded);
  if(n<0 || (size_t)n>=sizeof(url)) {
    drive_error(error,error_size,"Drive URL is too long");
    return 0;
  }
  json=NULL;
  if(!drive_http_json(session,"GET",url,NULL,NULL,0,&json,&http,error,error_size)) return 0;
  files=cJSON_GetObjectItemCaseSensitive(json,"files");
  if(!cJSON_IsArray(files)) {
    cJSON_Delete(json);
    drive_error(error,error_size,"invalid file list from Google Drive");
    return 0;
  }
  count=cJSON_GetArraySize(files);
  if(count>1) {
    cJSON_Delete(json);
    drive_error(error,error_size,"ambiguous Drive path: duplicate names in the same folder");
    return 0;
  }
  if(count==0) {
    cJSON_Delete(json);
    return 1;
  }
  item=cJSON_GetArrayItem(files,0);
  ok=drive_parse_file(item,file,error,error_size);
  cJSON_Delete(json);
  if(ok) *found=1;
  return ok;
}

static int drive_resolve(struct DriveSession *session,const char *path,struct DriveMapEntry *entry,struct DriveFile *file,char *error,size_t error_size) {
  char alias[DRIVE_ALIAS_MAX+1],copy[DRIVE_PATH_MAX],*p,*slash;
  struct DriveFile current,next;
  int found;

  if(!drive_valid_path(path) || !drive_path_alias(path,alias,sizeof(alias))) {
    drive_error(error,error_size,"invalid Drive path");
    return 0;
  }
  if(!drive_map_lookup(alias,entry,error,error_size)) return 0;
  if(!drive_session_open(session,error,error_size)) return 0;
  if(!drive_get_file(session,entry->id,&current,error,error_size)) return 0;
  if(!current.folder) {
    drive_error(error,error_size,"configured Drive root is not a folder");
    return 0;
  }
  p=strchr(path,'/');
  if(p==NULL) {
    *file=current;
    return 1;
  }
  snprintf(copy,sizeof(copy),"%s",p+1);
  p=copy;
  for(;;) {
    slash=strchr(p,'/');
    if(slash!=NULL) *slash=0;
    if(!current.folder) {
      drive_error(error,error_size,"Drive path crosses a non-folder item");
      return 0;
    }
    if(!drive_find_child(session,current.id,p,&next,&found,error,error_size)) return 0;
    if(!found) {
      drive_error(error,error_size,"Drive path not found");
      return 0;
    }
    current=next;
    if(slash==NULL) break;
    p=slash+1;
  }
  *file=current;
  return 1;
}

static int drive_resolve_parent(struct DriveSession *session,const char *path,struct DriveMapEntry *entry,struct DriveFile *parent,char *name,size_t name_size,char *error,size_t error_size) {
  char copy[DRIVE_PATH_MAX],*slash;

  if(!drive_valid_path(path) || strlen(path)>=sizeof(copy)) {
    drive_error(error,error_size,"invalid Drive path");
    return 0;
  }
  strcpy(copy,path);
  slash=strrchr(copy,'/');
  if(slash==NULL) {
    drive_error(error,error_size,"operation is not allowed on a Drive root alias");
    return 0;
  }
  *slash=0;
  if(!drive_valid_name(slash+1) || strlen(slash+1)>=name_size) {
    drive_error(error,error_size,"invalid Drive item name");
    return 0;
  }
  snprintf(name,name_size,"%s",slash+1);
  if(!drive_resolve(session,copy,entry,parent,error,error_size)) return 0;
  if(!parent->folder) {
    drive_error(error,error_size,"Drive parent is not a folder");
    return 0;
  }
  return 1;
}

static cJSON *drive_file_json(const char *path,const struct DriveFile *file,const struct DriveMapEntry *entry) {
  cJSON *data;

  data=cJSON_CreateObject();
  if(data==NULL) return NULL;
  cJSON_AddStringToObject(data,"path",path);
  cJSON_AddStringToObject(data,"type",file->folder?"dir":"file");
  cJSON_AddStringToObject(data,"mime_type",file->mime);
  if(file->size_known) cJSON_AddNumberToObject(data,"size",(double)file->size);
  if(file->version[0]!=0) cJSON_AddStringToObject(data,"version",file->version);
  if(file->modified[0]!=0) cJSON_AddStringToObject(data,"modified_time",file->modified);
  cJSON_AddStringToObject(data,"mode",entry->writable?"rw":"ro");
  return data;
}

static int drive_list_folder(struct DriveSession *session,const struct DriveMapEntry *entry,const struct DriveFile *parent,const char *logical,int recursive,int depth,cJSON *data,int *count,char *error,size_t error_size) {
  char query[512],url[8192],page[1024],child_path[DRIVE_PATH_MAX];
  char *encoded,*encoded_page;
  cJSON *json,*files,*item,*next_item,*obj;
  struct DriveFile child;
  long http;
  int n,i,total,ok;

  if(depth>DRIVE_MAX_DEPTH) {
    drive_error(error,error_size,"Drive folder nesting is too deep");
    return 0;
  }
  page[0]=0;
  for(;;) {
    n=snprintf(query,sizeof(query),"'%s' in parents and trashed = false",parent->id);
    if(n<0 || (size_t)n>=sizeof(query)) {
      drive_error(error,error_size,"Drive query is too long");
      return 0;
    }
    encoded=curl_easy_escape(session->curl,query,0);
    if(encoded==NULL) {
      drive_error(error,error_size,"cannot encode Drive query");
      return 0;
    }
    if(page[0]==0) {
      n=snprintf(url,sizeof(url),"%s/files?q=%s&fields=nextPageToken,files(id,name,mimeType,size,version,modifiedTime)&pageSize=1000&supportsAllDrives=true&includeItemsFromAllDrives=true",drive_api_base(),encoded);
    } else {
      encoded_page=curl_easy_escape(session->curl,page,0);
      if(encoded_page==NULL) { curl_free(encoded); drive_error(error,error_size,"cannot encode Drive page token"); return 0; }
      n=snprintf(url,sizeof(url),"%s/files?q=%s&fields=nextPageToken,files(id,name,mimeType,size,version,modifiedTime)&pageSize=1000&pageToken=%s&supportsAllDrives=true&includeItemsFromAllDrives=true",drive_api_base(),encoded,encoded_page);
      curl_free(encoded_page);
    }
    curl_free(encoded);
    if(n<0 || (size_t)n>=sizeof(url)) {
      drive_error(error,error_size,"Drive URL is too long");
      return 0;
    }
    json=NULL;
    if(!drive_http_json(session,"GET",url,NULL,NULL,0,&json,&http,error,error_size)) return 0;
    files=cJSON_GetObjectItemCaseSensitive(json,"files");
    if(!cJSON_IsArray(files)) {
      cJSON_Delete(json);
      drive_error(error,error_size,"invalid file list from Google Drive");
      return 0;
    }
    total=cJSON_GetArraySize(files);
    for(i=0;i<total;i++) {
      if(*count>=DRIVE_MAX_LIST) {
        cJSON_Delete(json);
        drive_error(error,error_size,"too many Drive directory entries");
        return 0;
      }
      item=cJSON_GetArrayItem(files,i);
      if(!drive_parse_file(item,&child,error,error_size)) { cJSON_Delete(json); return 0; }
      if(snprintf(child_path,sizeof(child_path),"%s/%s",logical,child.name)<0 || strlen(child_path)>=sizeof(child_path)-1) {
        cJSON_Delete(json);
        drive_error(error,error_size,"Drive path is too long");
        return 0;
      }
      obj=drive_file_json(child_path,&child,entry);
      if(obj==NULL) { cJSON_Delete(json); drive_error(error,error_size,"out of memory"); return 0; }
      cJSON_AddItemToArray(data,obj);
      (*count)++;
      if(recursive && child.folder) {
        ok=drive_list_folder(session,entry,&child,child_path,recursive,depth+1,data,count,error,error_size);
        if(!ok) { cJSON_Delete(json); return 0; }
      }
    }
    page[0]=0;
    next_item=cJSON_GetObjectItemCaseSensitive(json,"nextPageToken");
    if(cJSON_IsString(next_item) && next_item->valuestring!=NULL) snprintf(page,sizeof(page),"%s",next_item->valuestring);
    cJSON_Delete(json);
    if(page[0]==0) break;
  }
  return 1;
}

int mcp_drive_list(const char *path,int recursive,cJSON **data,char *error,size_t error_size) {
  struct DriveSession session;
  struct DriveMapEntry entry;
  struct DriveFile folder;
  int count,ok;

  *data=NULL;
  memset(&session,0,sizeof(session));
  if(!drive_resolve(&session,path,&entry,&folder,error,error_size)) {
    drive_session_close(&session);
    return 0;
  }
  if(!folder.folder) {
    drive_session_close(&session);
    drive_error(error,error_size,"Drive list path is not a folder");
    return 0;
  }
  *data=cJSON_CreateArray();
  if(*data==NULL) {
    drive_session_close(&session);
    drive_error(error,error_size,"out of memory");
    return 0;
  }
  count=0;
  ok=drive_list_folder(&session,&entry,&folder,path,recursive,0,*data,&count,error,error_size);
  drive_session_close(&session);
  if(!ok) {
    cJSON_Delete(*data);
    *data=NULL;
    return 0;
  }
  return 1;
}

int mcp_drive_stat(const char *path,cJSON **data,char *error,size_t error_size) {
  struct DriveSession session;
  struct DriveMapEntry entry;
  struct DriveFile file;

  *data=NULL;
  memset(&session,0,sizeof(session));
  if(!drive_resolve(&session,path,&entry,&file,error,error_size)) {
    drive_session_close(&session);
    return 0;
  }
  *data=drive_file_json(path,&file,&entry);
  drive_session_close(&session);
  if(*data==NULL) {
    drive_error(error,error_size,"out of memory");
    return 0;
  }
  return 1;
}

static char *drive_base64_encode(const unsigned char *data,size_t len) {
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

int mcp_drive_read_blob(const char *path,long offset,int length,cJSON **data,char *error,size_t error_size) {
  struct DriveSession session;
  struct DriveMapEntry entry;
  struct DriveFile file;
  struct DriveBuffer response;
  char url[2048],range[128],*encoded;
  long http;
  long long remaining,want,end;
  int n;

  *data=NULL;
  memset(&session,0,sizeof(session));
  if(offset<0 || length<1) { drive_error(error,error_size,"invalid Drive blob range"); return 0; }
  if(!drive_resolve(&session,path,&entry,&file,error,error_size)) { drive_session_close(&session); return 0; }
  if(file.folder) { drive_session_close(&session); drive_error(error,error_size,"Drive item is a folder"); return 0; }
  if(strncmp(file.mime,"application/vnd.google-apps.",28)==0) { drive_session_close(&session); drive_error(error,error_size,"Google-native documents are not binary files; export is not supported"); return 0; }
  if(!file.size_known) { drive_session_close(&session); drive_error(error,error_size,"Drive file size is unavailable"); return 0; }
  if((long long)offset>file.size) { drive_session_close(&session); drive_error(error,error_size,"offset beyond end of Drive file"); return 0; }
  remaining=file.size-(long long)offset;
  want=remaining<(long long)length?remaining:(long long)length;
  memset(&response,0,sizeof(response));
  if(want>0) {
    end=(long long)offset+want-1;
    snprintf(range,sizeof(range),"%ld-%lld",offset,end);
    n=snprintf(url,sizeof(url),"%s/files/%s?alt=media&supportsAllDrives=true",drive_api_base(),file.id);
    if(n<0 || (size_t)n>=sizeof(url)) { drive_session_close(&session); drive_error(error,error_size,"Drive URL is too long"); return 0; }
    if(!drive_http(&session,"GET",url,NULL,NULL,0,range,&response,&http,error,error_size)) { drive_session_close(&session); return 0; }
    if(http!=200 && http!=206) {
      drive_buffer_free(&response);
      drive_session_close(&session);
      snprintf(url,sizeof(url),"Google Drive HTTP %ld while reading file",http);
      drive_error(error,error_size,url);
      return 0;
    }
    if((long long)response.len!=want) {
      drive_buffer_free(&response);
      drive_session_close(&session);
      drive_error(error,error_size,"Google Drive returned an unexpected byte range");
      return 0;
    }
  }
  encoded=drive_base64_encode(response.data,response.len);
  drive_buffer_free(&response);
  if(encoded==NULL) { drive_session_close(&session); drive_error(error,error_size,"out of memory"); return 0; }
  *data=cJSON_CreateObject();
  if(*data==NULL) { free(encoded); drive_session_close(&session); drive_error(error,error_size,"out of memory"); return 0; }
  cJSON_AddStringToObject(*data,"path",path);
  cJSON_AddNumberToObject(*data,"size",(double)file.size);
  cJSON_AddNumberToObject(*data,"offset",(double)offset);
  cJSON_AddNumberToObject(*data,"length",(double)want);
  cJSON_AddStringToObject(*data,"data_base64",encoded);
  cJSON_AddBoolToObject(*data,"eof",(long long)offset+want>=file.size);
  if(file.version[0]!=0) cJSON_AddStringToObject(*data,"version",file.version);
  if(file.modified[0]!=0) cJSON_AddStringToObject(*data,"modified_time",file.modified);
  free(encoded);
  drive_session_close(&session);
  return 1;
}

int mcp_drive_get_file(const char *path,const char *local_path,cJSON **data,char *error,size_t error_size) {
  struct DriveSession session;
  struct DriveMapEntry entry;
  struct DriveFile file;
  struct DriveFileSink sink;
  struct curl_slist *headers;
  CURLcode rc;
  FILE *f;
  char url[2048],temp_path[DRIVE_PATH_MAX];
  long http;
  int fd,n,ok;

  *data=NULL;
  memset(&session,0,sizeof(session));
  if(!drive_valid_path(path) || local_path==NULL || local_path[0]==0) { drive_error(error,error_size,"invalid Drive get-file arguments"); return 0; }
  if(!drive_resolve(&session,path,&entry,&file,error,error_size)) { drive_session_close(&session); return 0; }
  if(file.folder) { drive_session_close(&session); drive_error(error,error_size,"Drive item is a folder"); return 0; }
  if(strncmp(file.mime,"application/vnd.google-apps.",28)==0) { drive_session_close(&session); drive_error(error,error_size,"Google-native documents are not binary files; export is not supported"); return 0; }
  n=snprintf(temp_path,sizeof(temp_path),"%s.tmp.XXXXXX",local_path);
  if(n<0 || (size_t)n>=sizeof(temp_path)) { drive_session_close(&session); drive_error(error,error_size,"local path is too long"); return 0; }
  fd=mkstemp(temp_path);
  if(fd<0) { drive_session_close(&session); drive_error(error,error_size,"cannot create temporary local file"); return 0; }
  f=fdopen(fd,"wb");
  if(f==NULL) { close(fd); unlink(temp_path); drive_session_close(&session); drive_error(error,error_size,"cannot open temporary local file"); return 0; }
  n=snprintf(url,sizeof(url),"%s/files/%s?alt=media&supportsAllDrives=true",drive_api_base(),file.id);
  if(n<0 || (size_t)n>=sizeof(url)) { fclose(f); unlink(temp_path); drive_session_close(&session); drive_error(error,error_size,"Drive URL is too long"); return 0; }
  headers=NULL;
  headers=curl_slist_append(headers,session.auth);
  headers=curl_slist_append(headers,"Accept: application/octet-stream");
  if(headers==NULL) { fclose(f); unlink(temp_path); drive_session_close(&session); drive_error(error,error_size,"out of memory"); return 0; }
  sink.file=f;
  sink.bytes=0;
  curl_easy_reset(session.curl);
  curl_easy_setopt(session.curl,CURLOPT_URL,url);
  curl_easy_setopt(session.curl,CURLOPT_HTTPHEADER,headers);
  curl_easy_setopt(session.curl,CURLOPT_WRITEFUNCTION,drive_file_write_callback);
  curl_easy_setopt(session.curl,CURLOPT_WRITEDATA,&sink);
  curl_easy_setopt(session.curl,CURLOPT_SSL_VERIFYPEER,1L);
  curl_easy_setopt(session.curl,CURLOPT_SSL_VERIFYHOST,2L);
  curl_easy_setopt(session.curl,CURLOPT_CONNECTTIMEOUT,DRIVE_CONNECT_TIMEOUT);
  curl_easy_setopt(session.curl,CURLOPT_TIMEOUT,DRIVE_HTTP_TIMEOUT);
  curl_easy_setopt(session.curl,CURLOPT_USERAGENT,"mymcp-drive/1.02");
  rc=curl_easy_perform(session.curl);
  curl_easy_getinfo(session.curl,CURLINFO_RESPONSE_CODE,&http);
  curl_slist_free_all(headers);
  ok=1;
  if(rc!=CURLE_OK || http!=200) ok=0;
  if(ok && file.size_known && sink.bytes!=(unsigned long long)file.size) ok=0;
  if(ok && fflush(f)!=0) ok=0;
  if(ok && fsync(fd)!=0) ok=0;
  if(fclose(f)!=0) ok=0;
  if(!ok) {
    unlink(temp_path);
    drive_session_close(&session);
    if(rc!=CURLE_OK) snprintf(url,sizeof(url),"Drive download error: %s",curl_easy_strerror(rc));
    else if(http!=200) snprintf(url,sizeof(url),"Google Drive HTTP %ld while downloading file",http);
    else snprintf(url,sizeof(url),"incomplete local Drive download");
    drive_error(error,error_size,url);
    return 0;
  }
  if(rename(temp_path,local_path)!=0) {
    unlink(temp_path);
    drive_session_close(&session);
    drive_error(error,error_size,"cannot replace local file after Drive download");
    return 0;
  }
  *data=drive_file_json(path,&file,&entry);
  if(*data!=NULL) cJSON_AddStringToObject(*data,"id",file.id);
  drive_session_close(&session);
  if(*data==NULL) { drive_error(error,error_size,"out of memory"); return 0; }
  return 1;
}

static unsigned long long drive_stage_hash(const char *chat,const char *path) {
  const unsigned char *p;
  unsigned long long value;

  value=1469598103934665603ULL;
  p=(const unsigned char *)chat;
  for(;*p!=0;p++) { value^=(unsigned long long)*p; value*=1099511628211ULL; }
  value^=0xffULL;
  value*=1099511628211ULL;
  p=(const unsigned char *)path;
  for(;*p!=0;p++) { value^=(unsigned long long)*p; value*=1099511628211ULL; }
  return value;
}

static int drive_stage_files(const char *chat,const char *path,char *data_path,size_t data_size,char *meta_path,size_t meta_size,char *error,size_t error_size) {
  unsigned long long hash;
  const char *root;
  int n;

  root=drive_stage_path();
  hash=drive_stage_hash(chat,path);
  n=snprintf(data_path,data_size,"%s/%016llx.data",root,hash);
  if(n<0 || (size_t)n>=data_size) { drive_error(error,error_size,"Drive stage path is too long"); return 0; }
  n=snprintf(meta_path,meta_size,"%s/%016llx.meta",root,hash);
  if(n<0 || (size_t)n>=meta_size) { drive_error(error,error_size,"Drive stage path is too long"); return 0; }
  return 1;
}

static int drive_stage_dir(char *error,size_t error_size) {
  struct stat st;
  const char *path;

  path=drive_stage_path();
  if(lstat(path,&st)==0) {
    if(!S_ISDIR(st.st_mode) || S_ISLNK(st.st_mode)) { drive_error(error,error_size,"Drive stage path is not a safe directory"); return 0; }
    return 1;
  }
  if(errno!=ENOENT) { drive_error(error,error_size,"cannot access Drive stage directory"); return 0; }
  if(mkdir(path,0700)!=0) {
    drive_error(error,error_size,"cannot create Drive stage directory; create it as an mcp-owned 0700 directory");
    return 0;
  }
  return 1;
}

static int drive_stage_write_meta(const char *meta_path,const struct DriveStageMeta *meta,char *error,size_t error_size) {
  FILE *f;

  f=fopen(meta_path,"w");
  if(f==NULL) { drive_error(error,error_size,"cannot create Drive stage metadata"); return 0; }
  if(fprintf(f,"%s\n%s\n%s\n%s\n%d\n",meta->chat,meta->path,meta->version[0]!=0?meta->version:"-",meta->id[0]!=0?meta->id:"-",meta->existed)<0 || ferror(f)) {
    fclose(f);
    drive_error(error,error_size,"cannot write Drive stage metadata");
    return 0;
  }
  if(fclose(f)!=0) { drive_error(error,error_size,"cannot write Drive stage metadata"); return 0; }
  chmod(meta_path,0600);
  return 1;
}

static int drive_stage_read_line(FILE *f,char *out,size_t out_size) {
  size_t len;

  if(fgets(out,(int)out_size,f)==NULL) return 0;
  len=strlen(out);
  if(len==0 || out[len-1]!='\n') return 0;
  out[--len]=0;
  if(len>0 && out[len-1]=='\r') out[--len]=0;
  return 1;
}

static int drive_stage_read_meta(const char *meta_path,struct DriveStageMeta *meta,char *error,size_t error_size) {
  FILE *f;
  char existed[16],extra[8];

  memset(meta,0,sizeof(*meta));
  f=fopen(meta_path,"r");
  if(f==NULL) { drive_error(error,error_size,"Drive write session does not exist; start with truncate=true"); return 0; }
  if(!drive_stage_read_line(f,meta->chat,sizeof(meta->chat)) || !drive_stage_read_line(f,meta->path,sizeof(meta->path)) || !drive_stage_read_line(f,meta->version,sizeof(meta->version)) || !drive_stage_read_line(f,meta->id,sizeof(meta->id)) || !drive_stage_read_line(f,existed,sizeof(existed))) {
    fclose(f); drive_error(error,error_size,"invalid Drive stage metadata"); return 0;
  }
  extra[0]=0;
  if(fgets(extra,sizeof(extra),f)!=NULL) { fclose(f); drive_error(error,error_size,"invalid Drive stage metadata"); return 0; }
  fclose(f);
  if(strcmp(meta->version,"-")==0) meta->version[0]=0;
  if(strcmp(meta->id,"-")==0) meta->id[0]=0;
  if(strcmp(existed,"0")==0) meta->existed=0;
  else if(strcmp(existed,"1")==0) meta->existed=1;
  else { drive_error(error,error_size,"invalid Drive stage metadata"); return 0; }
  return 1;
}

static int drive_existing_target(struct DriveSession *session,const char *path,struct DriveMapEntry *entry,struct DriveFile *parent,char *name,size_t name_size,struct DriveFile *file,int *found,char *error,size_t error_size) {
  if(!drive_resolve_parent(session,path,entry,parent,name,name_size,error,error_size)) return 0;
  if(!entry->writable) { drive_error(error,error_size,"Drive alias is read-only"); return 0; }
  if(!drive_find_child(session,parent->id,name,file,found,error,error_size)) return 0;
  if(*found && file->folder) { drive_error(error,error_size,"Drive target is a folder"); return 0; }
  if(*found && strncmp(file->mime,"application/vnd.google-apps.",28)==0) {
    drive_error(error,error_size,"Google-native documents cannot be replaced by drive_write_blob");
    return 0;
  }
  return 1;
}

static int drive_upload_stage(struct DriveSession *session,const char *file_id,const char *stage_path,const char *mime,struct DriveFile *result,char *error,size_t error_size) {
  struct curl_slist *headers;
  struct DriveBuffer response;
  struct stat st;
  FILE *f;
  CURLcode rc;
  cJSON *json;
  char url[2048],content_header[320];
  long http;
  int n,ok;

  if(stat(stage_path,&st)!=0 || !S_ISREG(st.st_mode)) { drive_error(error,error_size,"Drive staged file is unavailable"); return 0; }
  f=fopen(stage_path,"rb");
  if(f==NULL) { drive_error(error,error_size,"cannot read Drive staged file"); return 0; }
  n=snprintf(url,sizeof(url),"%s/files/%s?uploadType=media&supportsAllDrives=true&fields=id,name,mimeType,size,version,modifiedTime",drive_upload_base(),file_id);
  if(n<0 || (size_t)n>=sizeof(url)) { fclose(f); drive_error(error,error_size,"Drive upload URL is too long"); return 0; }
  headers=NULL;
  headers=curl_slist_append(headers,session->auth);
  snprintf(content_header,sizeof(content_header),"Content-Type: %s",mime!=NULL?mime:"application/octet-stream");
  headers=curl_slist_append(headers,content_header);
  headers=curl_slist_append(headers,"Accept: application/json");
  headers=curl_slist_append(headers,"Expect:");
  if(headers==NULL) { fclose(f); drive_error(error,error_size,"out of memory"); return 0; }
  memset(&response,0,sizeof(response));
  curl_easy_reset(session->curl);
  curl_easy_setopt(session->curl,CURLOPT_URL,url);
  curl_easy_setopt(session->curl,CURLOPT_HTTPHEADER,headers);
  curl_easy_setopt(session->curl,CURLOPT_CUSTOMREQUEST,"PATCH");
  curl_easy_setopt(session->curl,CURLOPT_UPLOAD,1L);
  curl_easy_setopt(session->curl,CURLOPT_READDATA,f);
  curl_easy_setopt(session->curl,CURLOPT_INFILESIZE_LARGE,(curl_off_t)st.st_size);
  curl_easy_setopt(session->curl,CURLOPT_WRITEFUNCTION,drive_write_callback);
  curl_easy_setopt(session->curl,CURLOPT_WRITEDATA,&response);
  curl_easy_setopt(session->curl,CURLOPT_SSL_VERIFYPEER,1L);
  curl_easy_setopt(session->curl,CURLOPT_SSL_VERIFYHOST,2L);
  curl_easy_setopt(session->curl,CURLOPT_CONNECTTIMEOUT,DRIVE_CONNECT_TIMEOUT);
  curl_easy_setopt(session->curl,CURLOPT_TIMEOUT,DRIVE_HTTP_TIMEOUT);
  curl_easy_setopt(session->curl,CURLOPT_USERAGENT,"mymcp-drive/1.02");
  rc=curl_easy_perform(session->curl);
  fclose(f);
  curl_slist_free_all(headers);
  if(rc!=CURLE_OK) { drive_buffer_free(&response); snprintf(url,sizeof(url),"Drive upload error: %s",curl_easy_strerror(rc)); drive_error(error,error_size,url); return 0; }
  curl_easy_getinfo(session->curl,CURLINFO_RESPONSE_CODE,&http);
  if(http<200 || http>=300) { drive_buffer_free(&response); snprintf(url,sizeof(url),"Google Drive HTTP %ld while uploading file",http); drive_error(error,error_size,url); return 0; }
  json=cJSON_ParseWithLength((const char *)(response.data!=NULL?response.data:(unsigned char *)""),response.len);
  drive_buffer_free(&response);
  if(!cJSON_IsObject(json)) { cJSON_Delete(json); drive_error(error,error_size,"invalid upload response from Google Drive"); return 0; }
  ok=drive_parse_file(json,result,error,error_size);
  cJSON_Delete(json);
  return ok;
}

static const char *drive_mime_for_name(const char *name) {
  const char *dot;

  dot=strrchr(name,'.');
  if(dot!=NULL && strcasecmp(dot,".docx")==0) return "application/vnd.openxmlformats-officedocument.wordprocessingml.document";
  if(dot!=NULL && strcasecmp(dot,".pptx")==0) return "application/vnd.openxmlformats-officedocument.presentationml.presentation";
  if(dot!=NULL && strcasecmp(dot,".xlsx")==0) return "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet";
  if(dot!=NULL && strcasecmp(dot,".pdf")==0) return "application/pdf";
  if(dot!=NULL && strcasecmp(dot,".txt")==0) return "text/plain";
  return "application/octet-stream";
}

static int drive_create_file(struct DriveSession *session,const struct DriveFile *parent,const char *name,struct DriveFile *file,char *error,size_t error_size) {
  cJSON *body,*parents,*json;
  char *text,url[2048];
  long http;
  size_t len;
  int n,ok;

  body=cJSON_CreateObject();
  if(body==NULL) { drive_error(error,error_size,"out of memory"); return 0; }
  cJSON_AddStringToObject(body,"name",name);
  cJSON_AddStringToObject(body,"mimeType",drive_mime_for_name(name));
  parents=cJSON_AddArrayToObject(body,"parents");
  cJSON_AddItemToArray(parents,cJSON_CreateString(parent->id));
  text=cJSON_PrintUnformatted(body);
  cJSON_Delete(body);
  if(text==NULL) { drive_error(error,error_size,"out of memory"); return 0; }
  len=strlen(text);
  n=snprintf(url,sizeof(url),"%s/files?supportsAllDrives=true&fields=id,name,mimeType,size,version,modifiedTime",drive_api_base());
  if(n<0 || (size_t)n>=sizeof(url)) { free(text); drive_error(error,error_size,"Drive URL is too long"); return 0; }
  json=NULL;
  if(!drive_http_json(session,"POST",url,"application/json",text,len,&json,&http,error,error_size)) { free(text); return 0; }
  free(text);
  ok=drive_parse_file(json,file,error,error_size);
  cJSON_Delete(json);
  return ok;
}

static void drive_trash_created(struct DriveSession *session,const char *id) {
  char url[2048];
  struct DriveBuffer response;
  long http;

  if(snprintf(url,sizeof(url),"%s/files/%s?supportsAllDrives=true",drive_api_base(),id)<0) return;
  drive_http(session,"PATCH",url,"application/json","{\"trashed\":true}",16,NULL,&response,&http,NULL,0);
  drive_buffer_free(&response);
}

int mcp_drive_write_blob(const char *chat,const char *path,const char *expected_version,long offset,const unsigned char *buf,size_t len,int truncate,int commit,cJSON **data,char *error,size_t error_size) {
  struct DriveSession session;
  struct DriveMapEntry entry,access_entry;
  struct DriveFile parent,current,uploaded,created;
  struct DriveStageMeta meta;
  struct stat st;
  FILE *f;
  char alias[DRIVE_ALIAS_MAX+1],name[DRIVE_NAME_MAX+1],data_path[DRIVE_PATH_MAX],meta_path[DRIVE_PATH_MAX];
  int found,created_now;
  size_t written;

  *data=NULL;
  memset(&session,0,sizeof(session));
  memset(&meta,0,sizeof(meta));
  if(chat==NULL || chat[0]==0 || !drive_valid_path(path) || offset<0) { drive_error(error,error_size,"invalid Drive write arguments"); return 0; }
  if(truncate && offset!=0) { drive_error(error,error_size,"truncate requires offset 0"); return 0; }
  if(!drive_path_alias(path,alias,sizeof(alias)) || !drive_map_lookup(alias,&access_entry,error,error_size)) return 0;
  if(!access_entry.writable) { drive_error(error,error_size,"Drive alias is read-only"); return 0; }
  if(!drive_stage_dir(error,error_size) || !drive_stage_files(chat,path,data_path,sizeof(data_path),meta_path,sizeof(meta_path),error,error_size)) return 0;
  if(truncate) {
    if(!drive_existing_target(&session,path,&entry,&parent,name,sizeof(name),&current,&found,error,error_size)) { drive_session_close(&session); return 0; }
    if(found && expected_version!=NULL && expected_version[0]!=0 && strcmp(expected_version,current.version)!=0) { drive_session_close(&session); drive_error(error,error_size,"Drive version conflict before write"); return 0; }
    snprintf(meta.chat,sizeof(meta.chat),"%s",chat);
    snprintf(meta.path,sizeof(meta.path),"%s",path);
    meta.existed=found;
    if(found) {
      snprintf(meta.version,sizeof(meta.version),"%s",current.version);
      snprintf(meta.id,sizeof(meta.id),"%s",current.id);
    }
    f=fopen(data_path,"wb");
    if(f==NULL) { drive_session_close(&session); drive_error(error,error_size,"cannot create Drive staged file"); return 0; }
    written=fwrite(buf,1,len,f);
    if(fclose(f)!=0 || written!=len) { drive_session_close(&session); drive_error(error,error_size,"cannot write Drive staged file"); return 0; }
    chmod(data_path,0600);
    if(!drive_stage_write_meta(meta_path,&meta,error,error_size)) { unlink(data_path); drive_session_close(&session); return 0; }
    drive_session_close(&session);
  } else {
    if(!drive_stage_read_meta(meta_path,&meta,error,error_size)) return 0;
    if(strcmp(meta.chat,chat)!=0 || strcmp(meta.path,path)!=0) { drive_error(error,error_size,"Drive stage collision detected"); return 0; }
    if(stat(data_path,&st)!=0 || !S_ISREG(st.st_mode)) { drive_error(error,error_size,"Drive staged file is unavailable"); return 0; }
    if((long long)offset>(long long)st.st_size) { drive_error(error,error_size,"offset beyond end of staged Drive file"); return 0; }
    f=fopen(data_path,"r+b");
    if(f==NULL) { drive_error(error,error_size,"cannot open Drive staged file"); return 0; }
    if(fseek(f,offset,SEEK_SET)!=0) { fclose(f); drive_error(error,error_size,"cannot seek Drive staged file"); return 0; }
    written=fwrite(buf,1,len,f);
    if(fclose(f)!=0 || written!=len) { drive_error(error,error_size,"cannot write Drive staged file"); return 0; }
  }
  if(stat(data_path,&st)!=0) { drive_error(error,error_size,"cannot stat Drive staged file"); return 0; }
  if(!commit) {
    *data=cJSON_CreateObject();
    if(*data==NULL) { drive_error(error,error_size,"out of memory"); return 0; }
    cJSON_AddStringToObject(*data,"path",path);
    cJSON_AddBoolToObject(*data,"staged",1);
    cJSON_AddNumberToObject(*data,"size",(double)st.st_size);
    if(meta.version[0]!=0) cJSON_AddStringToObject(*data,"version_at_start",meta.version);
    return 1;
  }
  if(!drive_stage_read_meta(meta_path,&meta,error,error_size)) return 0;
  if(strcmp(meta.chat,chat)!=0 || strcmp(meta.path,path)!=0) { drive_error(error,error_size,"Drive stage collision detected"); return 0; }
  if(!drive_existing_target(&session,path,&entry,&parent,name,sizeof(name),&current,&found,error,error_size)) { drive_session_close(&session); return 0; }
  if(meta.existed) {
    if(!found || strcmp(meta.id,current.id)!=0 || strcmp(meta.version,current.version)!=0) { drive_session_close(&session); drive_error(error,error_size,"Drive version conflict at commit; staged data was kept"); return 0; }
  } else if(found) {
    drive_session_close(&session);
    drive_error(error,error_size,"Drive target was created by another writer; staged data was kept");
    return 0;
  }
  created_now=0;
  if(found) {
    if(!drive_upload_stage(&session,current.id,data_path,current.mime,&uploaded,error,error_size)) { drive_session_close(&session); return 0; }
  } else {
    if(!drive_create_file(&session,&parent,name,&created,error,error_size)) { drive_session_close(&session); return 0; }
    created_now=1;
    if(!drive_upload_stage(&session,created.id,data_path,drive_mime_for_name(name),&uploaded,error,error_size)) {
      drive_trash_created(&session,created.id);
      drive_session_close(&session);
      return 0;
    }
  }
  (void)created_now;
  unlink(data_path);
  unlink(meta_path);
  *data=drive_file_json(path,&uploaded,&entry);
  if(*data!=NULL) {
    cJSON_AddBoolToObject(*data,"committed",1);
  }
  drive_session_close(&session);
  if(*data==NULL) { drive_error(error,error_size,"out of memory"); return 0; }
  return 1;
}

int mcp_drive_put_file(const char *path,const char *local_path,const char *expected_version,cJSON **data,char *error,size_t error_size) {
  struct DriveSession session;
  struct DriveMapEntry entry,access_entry;
  struct DriveFile parent,current,uploaded,created;
  struct stat st;
  char alias[DRIVE_ALIAS_MAX+1],name[DRIVE_NAME_MAX+1];
  int found;

  *data=NULL;
  memset(&session,0,sizeof(session));
  if(!drive_valid_path(path) || local_path==NULL || local_path[0]==0) { drive_error(error,error_size,"invalid Drive put-file arguments"); return 0; }
  if(stat(local_path,&st)!=0 || !S_ISREG(st.st_mode)) { drive_error(error,error_size,"local file is unavailable"); return 0; }
  if(!drive_path_alias(path,alias,sizeof(alias)) || !drive_map_lookup(alias,&access_entry,error,error_size)) return 0;
  if(!access_entry.writable) { drive_error(error,error_size,"Drive alias is read-only"); return 0; }
  if(!drive_existing_target(&session,path,&entry,&parent,name,sizeof(name),&current,&found,error,error_size)) { drive_session_close(&session); return 0; }
  if(found && expected_version!=NULL && expected_version[0]!=0 && strcmp(expected_version,current.version)!=0) {
    drive_session_close(&session);
    drive_error(error,error_size,"Drive version conflict before write");
    return 0;
  }
  if(found) {
    if(!drive_upload_stage(&session,current.id,local_path,current.mime,&uploaded,error,error_size)) { drive_session_close(&session); return 0; }
  } else {
    if(!drive_create_file(&session,&parent,name,&created,error,error_size)) { drive_session_close(&session); return 0; }
    if(!drive_upload_stage(&session,created.id,local_path,drive_mime_for_name(name),&uploaded,error,error_size)) {
      drive_trash_created(&session,created.id);
      drive_session_close(&session);
      return 0;
    }
  }
  *data=drive_file_json(path,&uploaded,&entry);
  if(*data!=NULL) {
    cJSON_AddStringToObject(*data,"id",uploaded.id);
    cJSON_AddBoolToObject(*data,"committed",1);
  }
  drive_session_close(&session);
  if(*data==NULL) { drive_error(error,error_size,"out of memory"); return 0; }
  return 1;
}

static int drive_patch_metadata(struct DriveSession *session,const char *id,cJSON *body,struct DriveFile *result,char *error,size_t error_size) {
  char url[2048],*text;
  cJSON *json;
  long http;
  size_t len;
  int n,ok;

  text=cJSON_PrintUnformatted(body);
  if(text==NULL) { drive_error(error,error_size,"out of memory"); return 0; }
  len=strlen(text);
  n=snprintf(url,sizeof(url),"%s/files/%s?supportsAllDrives=true&fields=id,name,mimeType,size,version,modifiedTime",drive_api_base(),id);
  if(n<0 || (size_t)n>=sizeof(url)) { free(text); drive_error(error,error_size,"Drive URL is too long"); return 0; }
  json=NULL;
  if(!drive_http_json(session,"PATCH",url,"application/json",text,len,&json,&http,error,error_size)) { free(text); return 0; }
  free(text);
  ok=drive_parse_file(json,result,error,error_size);
  cJSON_Delete(json);
  return ok;
}

int mcp_drive_mkdir(const char *path,cJSON **data,char *error,size_t error_size) {
  struct DriveSession session;
  struct DriveMapEntry entry;
  struct DriveFile parent,existing,created;
  cJSON *body,*parents,*json;
  char name[DRIVE_NAME_MAX+1],url[2048],*text;
  long http;
  size_t len;
  int found,n,ok;

  *data=NULL;
  memset(&session,0,sizeof(session));
  if(!drive_resolve_parent(&session,path,&entry,&parent,name,sizeof(name),error,error_size)) { drive_session_close(&session); return 0; }
  if(!entry.writable) { drive_session_close(&session); drive_error(error,error_size,"Drive alias is read-only"); return 0; }
  if(!drive_find_child(&session,parent.id,name,&existing,&found,error,error_size)) { drive_session_close(&session); return 0; }
  if(found) { drive_session_close(&session); drive_error(error,error_size,"Drive item already exists"); return 0; }
  body=cJSON_CreateObject();
  if(body==NULL) { drive_session_close(&session); drive_error(error,error_size,"out of memory"); return 0; }
  cJSON_AddStringToObject(body,"name",name);
  cJSON_AddStringToObject(body,"mimeType","application/vnd.google-apps.folder");
  parents=cJSON_AddArrayToObject(body,"parents");
  cJSON_AddItemToArray(parents,cJSON_CreateString(parent.id));
  text=cJSON_PrintUnformatted(body);
  cJSON_Delete(body);
  if(text==NULL) { drive_session_close(&session); drive_error(error,error_size,"out of memory"); return 0; }
  len=strlen(text);
  n=snprintf(url,sizeof(url),"%s/files?supportsAllDrives=true&fields=id,name,mimeType,size,version,modifiedTime",drive_api_base());
  if(n<0 || (size_t)n>=sizeof(url)) { free(text); drive_session_close(&session); drive_error(error,error_size,"Drive URL is too long"); return 0; }
  json=NULL;
  if(!drive_http_json(&session,"POST",url,"application/json",text,len,&json,&http,error,error_size)) { free(text); drive_session_close(&session); return 0; }
  free(text);
  ok=drive_parse_file(json,&created,error,error_size);
  cJSON_Delete(json);
  if(ok) *data=drive_file_json(path,&created,&entry);
  drive_session_close(&session);
  if(!ok) return 0;
  if(*data==NULL) { drive_error(error,error_size,"out of memory"); return 0; }
  return 1;
}

int mcp_drive_rename(const char *path,const char *new_name,const char *expected_version,cJSON **data,char *error,size_t error_size) {
  struct DriveSession session;
  struct DriveMapEntry entry;
  struct DriveFile parent,file,other,updated;
  cJSON *body;
  char old_name[DRIVE_NAME_MAX+1],new_path[DRIVE_PATH_MAX],parent_path[DRIVE_PATH_MAX],*slash;
  int found;

  *data=NULL;
  memset(&session,0,sizeof(session));
  if(!drive_valid_name(new_name)) { drive_error(error,error_size,"invalid new Drive name"); return 0; }
  if(!drive_resolve_parent(&session,path,&entry,&parent,old_name,sizeof(old_name),error,error_size)) { drive_session_close(&session); return 0; }
  if(!entry.writable) { drive_session_close(&session); drive_error(error,error_size,"Drive alias is read-only"); return 0; }
  if(!drive_find_child(&session,parent.id,old_name,&file,&found,error,error_size)) { drive_session_close(&session); return 0; }
  if(!found) { drive_session_close(&session); drive_error(error,error_size,"Drive path not found"); return 0; }
  if(expected_version!=NULL && expected_version[0]!=0 && strcmp(expected_version,file.version)!=0) { drive_session_close(&session); drive_error(error,error_size,"Drive version conflict"); return 0; }
  if(strcmp(old_name,new_name)==0) {
    *data=drive_file_json(path,&file,&entry);
    drive_session_close(&session);
    if(*data==NULL) { drive_error(error,error_size,"out of memory"); return 0; }
    return 1;
  }
  if(!drive_find_child(&session,parent.id,new_name,&other,&found,error,error_size)) { drive_session_close(&session); return 0; }
  if(found) { drive_session_close(&session); drive_error(error,error_size,"another Drive item already has the new name"); return 0; }
  body=cJSON_CreateObject();
  if(body==NULL) { drive_session_close(&session); drive_error(error,error_size,"out of memory"); return 0; }
  cJSON_AddStringToObject(body,"name",new_name);
  if(!drive_patch_metadata(&session,file.id,body,&updated,error,error_size)) { cJSON_Delete(body); drive_session_close(&session); return 0; }
  cJSON_Delete(body);
  snprintf(parent_path,sizeof(parent_path),"%s",path);
  slash=strrchr(parent_path,'/');
  if(slash==NULL) { drive_session_close(&session); drive_error(error,error_size,"invalid Drive path"); return 0; }
  *slash=0;
  if(snprintf(new_path,sizeof(new_path),"%s/%s",parent_path,new_name)<0 || strlen(new_path)>=sizeof(new_path)-1) { drive_session_close(&session); drive_error(error,error_size,"Drive path is too long"); return 0; }
  *data=drive_file_json(new_path,&updated,&entry);
  drive_session_close(&session);
  if(*data==NULL) { drive_error(error,error_size,"out of memory"); return 0; }
  return 1;
}

int mcp_drive_delete(const char *path,const char *expected_version,cJSON **data,char *error,size_t error_size) {
  struct DriveSession session;
  struct DriveMapEntry entry;
  struct DriveFile parent,file,updated;
  cJSON *body;
  char name[DRIVE_NAME_MAX+1];
  int found;

  *data=NULL;
  memset(&session,0,sizeof(session));
  if(!drive_resolve_parent(&session,path,&entry,&parent,name,sizeof(name),error,error_size)) { drive_session_close(&session); return 0; }
  if(!entry.writable) { drive_session_close(&session); drive_error(error,error_size,"Drive alias is read-only"); return 0; }
  if(!drive_find_child(&session,parent.id,name,&file,&found,error,error_size)) { drive_session_close(&session); return 0; }
  if(!found) { drive_session_close(&session); drive_error(error,error_size,"Drive path not found"); return 0; }
  if(expected_version!=NULL && expected_version[0]!=0 && strcmp(expected_version,file.version)!=0) { drive_session_close(&session); drive_error(error,error_size,"Drive version conflict"); return 0; }
  body=cJSON_CreateObject();
  if(body==NULL) { drive_session_close(&session); drive_error(error,error_size,"out of memory"); return 0; }
  cJSON_AddBoolToObject(body,"trashed",1);
  if(!drive_patch_metadata(&session,file.id,body,&updated,error,error_size)) { cJSON_Delete(body); drive_session_close(&session); return 0; }
  cJSON_Delete(body);
  *data=drive_file_json(path,&updated,&entry);
  if(*data!=NULL) cJSON_AddBoolToObject(*data,"trashed",1);
  drive_session_close(&session);
  if(*data==NULL) { drive_error(error,error_size,"out of memory"); return 0; }
  return 1;
}
