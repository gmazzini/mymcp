// Gianluca Mazzini @2026- Version 1.02
#include <cjson/cJSON.h>
#include <curl/curl.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define GMCP_VERSION "1.02"
#define DEFAULT_URL "https://www.mazzini.org/mcp"
#define PROTOCOL_VERSION "2026-07-28"
#define CLIENT_NAME "gmcp"
#define BLOB_CHUNK 1048576
#define MAX_EDIT_BYTES 16777216
#define HTTP_TIMEOUT_MS 30000L
#define CHAT_MAX 64
#define WORK_PREFIX "/home/tools/mcp/work/"
#define GITHUB_MAP "github.map"
#define GITHUB_COMPONENT_MAX 128
#define GITHUB_MAX_ENTRIES 10000

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

struct Buffer {
  char *data;
  size_t len;
};

struct Blob {
  unsigned char *data;
  size_t len;
  long size;
  long offset;
  int eof;
};

struct GithubEntry {
  char source[PATH_MAX];
  char owner[GITHUB_COMPONENT_MAX+1];
  char repo[GITHUB_COMPONENT_MAX+1];
  char dest[PATH_MAX];
  long size;
};

static long request_id=1;

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

static int valid_chat(const char *s) {
  size_t i,n;
  unsigned char c;

  if(s==NULL) return 0;
  n=strlen(s);
  if(n<1 || n>CHAT_MAX) return 0;
  for(i=0;i<n;i++) {
    c=(unsigned char)s[i];
    if(!isalnum(c) && c!='_' && c!='-' && c!='.') return 0;
  }
  return 1;
}

static int valid_project(const char *s) {
  if(!valid_chat(s)) return 0;
  if(strcmp(s,".")==0 || strcmp(s,"..")==0) return 0;
  return 1;
}

static int valid_relpath(const char *s,int allow_empty) {
  const char *p,*q;
  size_t n;

  if(s==NULL) return allow_empty;
  if(s[0]==0) return allow_empty;
  if(s[0]=='/' || strchr(s,'\\')!=NULL) return 0;
  p=s;
  for(;;) {
    q=strchr(p,'/');
    n=q==NULL?strlen(p):(size_t)(q-p);
    if(n==0) return 0;
    if(n==1 && p[0]=='.') return 0;
    if(n==2 && p[0]=='.' && p[1]=='.') return 0;
    if(q==NULL) break;
    p=q+1;
  }
  return 1;
}

static int make_path(char *out,size_t out_size,const char *project,const char *rel) {
  int n;

  if(!valid_project(project) || !valid_relpath(rel,1)) return -1;
  if(rel!=NULL && rel[0]!=0) n=snprintf(out,out_size,"%s/%s",project,rel);
  else n=snprintf(out,out_size,"%s",project);
  if(n<0 || (size_t)n>=out_size) return -1;
  return 0;
}

static const char *gmcp_url(void) {
  const char *url;

  url=getenv("GMCP_URL");
  if(url!=NULL && url[0]!=0) return url;
  return DEFAULT_URL;
}

static int read_token(char *out,size_t out_size) {
  const char *env,*home;
  char path[PATH_MAX];
  FILE *f;
  size_t n;

  env=getenv("GMCP_TOKEN");
  if(env!=NULL && env[0]!=0) {
    if(strlen(env)+1>out_size) return -1;
    strcpy(out,env);
    return 0;
  }
  home=getenv("HOME");
  if(home==NULL || home[0]==0) {
    fprintf(stderr,"HOME is not set\n");
    return -1;
  }
  if(snprintf(path,sizeof(path),"%s/mcp/token.txt",home)>=(int)sizeof(path)) return -1;
  f=fopen(path,"r");
  if(f==NULL) {
    fprintf(stderr,"cannot open %s: %s\n",path,strerror(errno));
    return -1;
  }
  if(fgets(out,(int)out_size,f)==NULL) {
    fclose(f);
    fprintf(stderr,"cannot read %s\n",path);
    return -1;
  }
  fclose(f);
  n=strlen(out);
  for(;n>0 && isspace((unsigned char)out[n-1]);n--) out[n-1]=0;
  if(out[0]==0) {
    fprintf(stderr,"empty token in %s\n",path);
    return -1;
  }
  return 0;
}

static void print_tool_error(cJSON *result) {
  cJSON *content,*item,*text,*structured,*value;

  content=cJSON_GetObjectItemCaseSensitive(result,"content");
  if(cJSON_IsArray(content) && cJSON_GetArraySize(content)>0) {
    item=cJSON_GetArrayItem(content,0);
    text=cJSON_GetObjectItemCaseSensitive(item,"text");
    if(cJSON_IsString(text)) {
      fprintf(stderr,"%s\n",text->valuestring);
      return;
    }
  }
  structured=cJSON_GetObjectItemCaseSensitive(result,"structuredContent");
  if(cJSON_IsObject(structured)) {
    value=cJSON_GetObjectItemCaseSensitive(structured,"result");
    if(cJSON_IsString(value)) {
      fprintf(stderr,"%s\n",value->valuestring);
      return;
    }
  }
  fprintf(stderr,"MCP tool failed\n");
}

static cJSON *mcp_call(const char *chat,const char *name,cJSON *args) {
  char token[512],auth[640],method_header[160],name_header[256];
  struct Buffer body;
  struct curl_slist *headers;
  CURL *curl;
  CURLcode rc;
  cJSON *root,*params,*meta,*info,*reply,*error,*result,*is_error,*structured,*copy;
  char *json,*error_text;
  long http_code;

  if(!valid_chat(chat)) {
    fprintf(stderr,"invalid chat: %s\n",chat!=NULL?chat:"(null)");
    cJSON_Delete(args);
    return NULL;
  }
  if(read_token(token,sizeof(token))!=0) {
    cJSON_Delete(args);
    return NULL;
  }
  if(args==NULL) args=cJSON_CreateObject();
  if(args==NULL) return NULL;
  cJSON_AddStringToObject(args,"chat",chat);
  root=cJSON_CreateObject();
  if(root==NULL) {
    cJSON_Delete(args);
    return NULL;
  }
  cJSON_AddStringToObject(root,"jsonrpc","2.0");
  cJSON_AddNumberToObject(root,"id",(double)request_id++);
  cJSON_AddStringToObject(root,"method","tools/call");
  params=cJSON_AddObjectToObject(root,"params");
  meta=cJSON_AddObjectToObject(params,"_meta");
  cJSON_AddStringToObject(meta,"io.modelcontextprotocol/protocolVersion",PROTOCOL_VERSION);
  cJSON_AddObjectToObject(meta,"io.modelcontextprotocol/clientCapabilities");
  info=cJSON_AddObjectToObject(meta,"io.modelcontextprotocol/clientInfo");
  cJSON_AddStringToObject(info,"name",CLIENT_NAME);
  cJSON_AddStringToObject(info,"version",GMCP_VERSION);
  cJSON_AddStringToObject(params,"name",name);
  cJSON_AddItemToObject(params,"arguments",args);
  json=cJSON_PrintUnformatted(root);
  cJSON_Delete(root);
  if(json==NULL) return NULL;
  if(snprintf(auth,sizeof(auth),"Authorization: Bearer %s",token)>=(int)sizeof(auth)) {
    free(json);
    return NULL;
  }
  if(snprintf(method_header,sizeof(method_header),"Mcp-Method: tools/call")>=(int)sizeof(method_header)) {
    free(json);
    return NULL;
  }
  if(snprintf(name_header,sizeof(name_header),"Mcp-Name: %s",name)>=(int)sizeof(name_header)) {
    free(json);
    return NULL;
  }
  body.data=NULL;
  body.len=0;
  headers=NULL;
  headers=curl_slist_append(headers,"Content-Type: application/json");
  headers=curl_slist_append(headers,"MCP-Protocol-Version: 2026-07-28");
  headers=curl_slist_append(headers,method_header);
  headers=curl_slist_append(headers,name_header);
  headers=curl_slist_append(headers,auth);
  curl=curl_easy_init();
  if(curl==NULL) {
    curl_slist_free_all(headers);
    free(json);
    return NULL;
  }
  curl_easy_setopt(curl,CURLOPT_URL,gmcp_url());
  curl_easy_setopt(curl,CURLOPT_POST,1L);
  curl_easy_setopt(curl,CURLOPT_HTTPHEADER,headers);
  curl_easy_setopt(curl,CURLOPT_POSTFIELDS,json);
  curl_easy_setopt(curl,CURLOPT_POSTFIELDSIZE,(long)strlen(json));
  curl_easy_setopt(curl,CURLOPT_WRITEFUNCTION,http_write);
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
    if(body.data!=NULL && body.data[0]!=0) fprintf(stderr,"%s\n",body.data);
    free(body.data);
    return NULL;
  }
  reply=cJSON_Parse(body.data!=NULL?body.data:"");
  free(body.data);
  if(reply==NULL) {
    fprintf(stderr,"invalid MCP JSON response\n");
    return NULL;
  }
  error=cJSON_GetObjectItemCaseSensitive(reply,"error");
  if(cJSON_IsObject(error)) {
    error_text=cJSON_PrintUnformatted(error);
    fprintf(stderr,"MCP error: %s\n",error_text!=NULL?error_text:"unknown");
    free(error_text);
    cJSON_Delete(reply);
    return NULL;
  }
  result=cJSON_GetObjectItemCaseSensitive(reply,"result");
  if(!cJSON_IsObject(result)) {
    fprintf(stderr,"MCP result missing\n");
    cJSON_Delete(reply);
    return NULL;
  }
  is_error=cJSON_GetObjectItemCaseSensitive(result,"isError");
  if(cJSON_IsTrue(is_error)) {
    print_tool_error(result);
    cJSON_Delete(reply);
    return NULL;
  }
  structured=cJSON_GetObjectItemCaseSensitive(result,"structuredContent");
  if(structured==NULL) {
    fprintf(stderr,"MCP structuredContent missing\n");
    cJSON_Delete(reply);
    return NULL;
  }
  copy=cJSON_Duplicate(structured,1);
  cJSON_Delete(reply);
  return copy;
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

static cJSON *api_list_files(const char *chat,const char *path,int recursive) {
  cJSON *args,*result;

  args=cJSON_CreateObject();
  cJSON_AddStringToObject(args,"path",path);
  cJSON_AddBoolToObject(args,"recursive",recursive?1:0);
  result=mcp_call(chat,"list_files",args);
  if(result!=NULL && !cJSON_IsArray(result)) {
    fprintf(stderr,"invalid list_files response\n");
    cJSON_Delete(result);
    return NULL;
  }
  return result;
}

static int api_read_blob(const char *chat,const char *path,long offset,int length,struct Blob *blob) {
  cJSON *args,*result,*item;
  const char *encoded;

  memset(blob,0,sizeof(*blob));
  args=cJSON_CreateObject();
  cJSON_AddStringToObject(args,"path",path);
  cJSON_AddNumberToObject(args,"offset",(double)offset);
  cJSON_AddNumberToObject(args,"length",length);
  result=mcp_call(chat,"read_blob",args);
  if(result==NULL) return -1;
  item=cJSON_GetObjectItemCaseSensitive(result,"size");
  if(!cJSON_IsNumber(item)) goto bad;
  blob->size=(long)item->valuedouble;
  item=cJSON_GetObjectItemCaseSensitive(result,"offset");
  if(!cJSON_IsNumber(item)) goto bad;
  blob->offset=(long)item->valuedouble;
  item=cJSON_GetObjectItemCaseSensitive(result,"length");
  if(!cJSON_IsNumber(item)) goto bad;
  item=cJSON_GetObjectItemCaseSensitive(result,"eof");
  if(!cJSON_IsBool(item)) goto bad;
  blob->eof=cJSON_IsTrue(item)?1:0;
  item=cJSON_GetObjectItemCaseSensitive(result,"data_base64");
  if(!cJSON_IsString(item)) goto bad;
  encoded=item->valuestring;
  blob->data=base64_decode(encoded,&blob->len);
  if(blob->data==NULL) goto bad;
  cJSON_Delete(result);
  return 0;

bad:
  fprintf(stderr,"invalid read_blob response\n");
  free(blob->data);
  blob->data=NULL;
  cJSON_Delete(result);
  return -1;
}

static int api_write_blob(const char *chat,const char *path,long offset,const unsigned char *data,size_t len,int truncate) {
  cJSON *args,*result;
  char *encoded;

  encoded=base64_encode(data,len);
  if(encoded==NULL) return -1;
  args=cJSON_CreateObject();
  cJSON_AddStringToObject(args,"path",path);
  cJSON_AddNumberToObject(args,"offset",(double)offset);
  cJSON_AddStringToObject(args,"data_base64",encoded);
  cJSON_AddBoolToObject(args,"truncate",truncate?1:0);
  free(encoded);
  result=mcp_call(chat,"write_blob",args);
  if(result==NULL) return -1;
  cJSON_Delete(result);
  return 0;
}

static int ensure_local_dir(const char *path) {
  char tmp[PATH_MAX];
  char *p;
  struct stat st;

  if(path==NULL || path[0]==0 || path[0]=='/' || strlen(path)>=sizeof(tmp)) return -1;
  strcpy(tmp,path);
  p=tmp;
  for(;;) {
    p=strchr(p,'/');
    if(p!=NULL) *p=0;
    if(lstat(tmp,&st)==0) {
      if(S_ISLNK(st.st_mode) || !S_ISDIR(st.st_mode)) return -1;
    } else {
      if(errno!=ENOENT || mkdir(tmp,0755)!=0) return -1;
    }
    if(p==NULL) break;
    *p='/';
    p++;
  }
  return 0;
}

static int ensure_local_parent(const char *path) {
  char tmp[PATH_MAX];
  char *slash;

  if(path==NULL || strlen(path)>=sizeof(tmp)) return -1;
  strcpy(tmp,path);
  slash=strrchr(tmp,'/');
  if(slash==NULL) return 0;
  *slash=0;
  if(tmp[0]==0) return -1;
  return ensure_local_dir(tmp);
}

static int remote_size(const char *chat,const char *remote,long *size) {
  struct Blob blob;

  if(api_read_blob(chat,remote,0,1,&blob)!=0) return -1;
  *size=blob.size;
  free(blob.data);
  return 0;
}

static int remote_matches_local(const char *chat,const char *remote,const char *local,long size) {
  struct stat st;
  struct Blob blob;
  FILE *f;
  unsigned char *buf;
  long offset;
  size_t got;
  int same;

  if(lstat(local,&st)!=0 || !S_ISREG(st.st_mode) || S_ISLNK(st.st_mode)) return 0;
  if(st.st_size!=(off_t)size) return 0;
  if(size==0) return 1;
  f=fopen(local,"rb");
  if(f==NULL) return 0;
  buf=(unsigned char *)malloc(BLOB_CHUNK);
  if(buf==NULL) {
    fclose(f);
    return 0;
  }
  offset=0;
  same=1;
  while(offset<size) {
    if(api_read_blob(chat,remote,offset,BLOB_CHUNK,&blob)!=0) {
      same=0;
      break;
    }
    if(blob.size!=size || blob.offset!=offset || blob.len==0) {
      free(blob.data);
      same=0;
      break;
    }
    got=fread(buf,1,blob.len,f);
    if(got!=blob.len || memcmp(buf,blob.data,blob.len)!=0) same=0;
    offset+=(long)blob.len;
    free(blob.data);
    if(!same) break;
  }
  free(buf);
  fclose(f);
  return same;
}

static int download_file(const char *chat,const char *remote,const char *local,long size,int protect) {
  struct stat st;
  struct Blob blob;
  FILE *f;
  long offset;
  size_t written;

  if(lstat(local,&st)==0) {
    if(S_ISLNK(st.st_mode) || !S_ISREG(st.st_mode)) {
      fprintf(stderr,"REFUSED %s: local target is not a regular file\n",local);
      return -1;
    }
    if(remote_matches_local(chat,remote,local,size)) {
      printf("SAME %s\n",local);
      return 0;
    }
    if(protect) {
      printf("CONFLICT %s\n",local);
      return 2;
    }
  } else if(errno!=ENOENT) {
    fprintf(stderr,"cannot inspect %s: %s\n",local,strerror(errno));
    return -1;
  }
  if(ensure_local_parent(local)!=0) {
    fprintf(stderr,"cannot create local parent for %s\n",local);
    return -1;
  }
  f=fopen(local,"wb");
  if(f==NULL) {
    fprintf(stderr,"cannot write %s: %s\n",local,strerror(errno));
    return -1;
  }
  offset=0;
  while(offset<size) {
    if(api_read_blob(chat,remote,offset,BLOB_CHUNK,&blob)!=0) {
      fclose(f);
      return -1;
    }
    if(blob.size!=size || blob.offset!=offset || blob.len==0) {
      free(blob.data);
      fclose(f);
      fprintf(stderr,"remote file changed while downloading %s\n",remote);
      return -1;
    }
    written=fwrite(blob.data,1,blob.len,f);
    if(written!=blob.len) {
      free(blob.data);
      fclose(f);
      fprintf(stderr,"local write failed: %s\n",local);
      return -1;
    }
    offset+=(long)blob.len;
    free(blob.data);
  }
  if(fclose(f)!=0) return -1;
  printf("GET %s\n",local);
  return 0;
}

static int upload_file(const char *chat,const char *local,const char *remote) {
  struct stat st;
  FILE *f;
  unsigned char *buf;
  size_t got;
  long offset,size;
  int first;

  if(lstat(local,&st)!=0 || !S_ISREG(st.st_mode) || S_ISLNK(st.st_mode)) {
    fprintf(stderr,"local file not found: %s\n",local);
    return -1;
  }
  if(st.st_size>(off_t)LONG_MAX) {
    fprintf(stderr,"file too large: %s\n",local);
    return -1;
  }
  size=(long)st.st_size;
  f=fopen(local,"rb");
  if(f==NULL) return -1;
  buf=(unsigned char *)malloc(BLOB_CHUNK);
  if(buf==NULL) {
    fclose(f);
    return -1;
  }
  offset=0;
  first=1;
  if(size==0) {
    if(api_write_blob(chat,remote,0,(const unsigned char *)"",0,1)!=0) {
      free(buf);
      fclose(f);
      return -1;
    }
  }
  while(offset<size) {
    got=fread(buf,1,BLOB_CHUNK,f);
    if(got==0) {
      free(buf);
      fclose(f);
      return -1;
    }
    if(api_write_blob(chat,remote,offset,buf,got,first)!=0) {
      free(buf);
      fclose(f);
      return -1;
    }
    first=0;
    offset+=(long)got;
  }
  free(buf);
  fclose(f);
  printf("PUT %s -> %s\n",local,remote);
  return 0;
}

static int read_remote_all(const char *chat,const char *remote,unsigned char **data,size_t *len) {
  struct Blob blob;
  unsigned char *buf;
  long size,offset;

  if(remote_size(chat,remote,&size)!=0) return -1;
  if(size<0 || size>MAX_EDIT_BYTES) {
    fprintf(stderr,"edit file is too large: %ld bytes\n",size);
    return -1;
  }
  buf=(unsigned char *)malloc((size_t)size+1);
  if(buf==NULL) return -1;
  offset=0;
  while(offset<size) {
    if(api_read_blob(chat,remote,offset,BLOB_CHUNK,&blob)!=0) {
      free(buf);
      return -1;
    }
    if(blob.size!=size || blob.offset!=offset || blob.len==0) {
      free(blob.data);
      free(buf);
      return -1;
    }
    memcpy(buf+offset,blob.data,blob.len);
    offset+=(long)blob.len;
    free(blob.data);
  }
  buf[size]=0;
  *data=buf;
  *len=(size_t)size;
  return 0;
}

static int read_local_all(const char *path,unsigned char **data,size_t *len) {
  struct stat st;
  FILE *f;
  unsigned char *buf;
  size_t got;

  if(lstat(path,&st)!=0 || !S_ISREG(st.st_mode) || S_ISLNK(st.st_mode)) return -1;
  if(st.st_size<0 || st.st_size>MAX_EDIT_BYTES) return -1;
  buf=(unsigned char *)malloc((size_t)st.st_size+1);
  if(buf==NULL) return -1;
  f=fopen(path,"rb");
  if(f==NULL) {
    free(buf);
    return -1;
  }
  got=fread(buf,1,(size_t)st.st_size,f);
  fclose(f);
  if(got!=(size_t)st.st_size) {
    free(buf);
    return -1;
  }
  buf[got]=0;
  *data=buf;
  *len=got;
  return 0;
}

static int write_local_all(const char *path,const unsigned char *data,size_t len) {
  FILE *f;
  size_t written;

  if(ensure_local_parent(path)!=0) return -1;
  f=fopen(path,"wb");
  if(f==NULL) return -1;
  written=fwrite(data,1,len,f);
  if(fclose(f)!=0 || written!=len) return -1;
  return 0;
}

static int run_editor(const char *path) {
  const char *editor;
  pid_t pid;
  int status;

  editor=getenv("EDITOR");
  if(editor==NULL || editor[0]==0) editor="nano";
  pid=fork();
  if(pid<0) return -1;
  if(pid==0) {
    execlp(editor,editor,path,(char *)NULL);
    _exit(127);
  }
  if(waitpid(pid,&status,0)<0) return -1;
  if(!WIFEXITED(status) || WEXITSTATUS(status)!=0) return -1;
  return 0;
}

static int remote_has_path(cJSON *list,const char *path) {
  cJSON *item,*value;
  int i,n;

  n=cJSON_GetArraySize(list);
  for(i=0;i<n;i++) {
    item=cJSON_GetArrayItem(list,i);
    value=cJSON_GetObjectItemCaseSensitive(item,"path");
    if(cJSON_IsString(value) && strcmp(value->valuestring,path)==0) return 1;
  }
  return 0;
}

static int walk_local_only(const char *path,cJSON *remote,int *count) {
  DIR *d;
  struct dirent *de;
  struct stat st;
  char child[PATH_MAX];
  int n;

  d=opendir(path);
  if(d==NULL) return -1;
  for(;;) {
    de=readdir(d);
    if(de==NULL) break;
    if(strcmp(de->d_name,".")==0 || strcmp(de->d_name,"..")==0) continue;
    n=snprintf(child,sizeof(child),"%s/%s",path,de->d_name);
    if(n<0 || (size_t)n>=sizeof(child)) {
      closedir(d);
      return -1;
    }
    if(lstat(child,&st)!=0) {
      closedir(d);
      return -1;
    }
    if(S_ISLNK(st.st_mode)) {
      if(!remote_has_path(remote,child)) {
        printf("LOCAL_ONLY %s\n",child);
        (*count)++;
      }
      continue;
    }
    if(!remote_has_path(remote,child)) {
      printf("LOCAL_ONLY %s\n",child);
      (*count)++;
    }
    if(S_ISDIR(st.st_mode)) {
      if(walk_local_only(child,remote,count)!=0) {
        closedir(d);
        return -1;
      }
    }
  }
  closedir(d);
  return 0;
}

static char *api_read_text(const char *chat,const char *path) {
  cJSON *args,*result,*value;
  char *text;
  size_t len;

  args=cJSON_CreateObject();
  cJSON_AddStringToObject(args,"path",path);
  result=mcp_call(chat,"read_file",args);
  if(result==NULL) return NULL;
  value=cJSON_GetObjectItemCaseSensitive(result,"result");
  if(!cJSON_IsString(value) || value->valuestring==NULL) {
    cJSON_Delete(result);
    fprintf(stderr,"invalid read_file response\n");
    return NULL;
  }
  len=strlen(value->valuestring);
  text=(char *)malloc(len+1);
  if(text!=NULL) memcpy(text,value->valuestring,len+1);
  cJSON_Delete(result);
  return text;
}

static int valid_github_component(const char *s) {
  size_t i,n;
  unsigned char c;

  if(s==NULL) return 0;
  n=strlen(s);
  if(n<1 || n>GITHUB_COMPONENT_MAX) return 0;
  for(i=0;i<n;i++) {
    c=(unsigned char)s[i];
    if(!isalnum(c) && c!='_' && c!='-' && c!='.') return 0;
  }
  return 1;
}

static int split_github_destination(char *text,struct GithubEntry *entry) {
  char *p,*q;

  p=strchr(text,'/');
  if(p==NULL) return -1;
  *p=0;
  q=strchr(p+1,'/');
  if(q==NULL) return -1;
  *q=0;
  if(!valid_github_component(text) || !valid_github_component(p+1) || !valid_relpath(q+1,0)) return -1;
  if(strlen(text)>=sizeof(entry->owner) || strlen(p+1)>=sizeof(entry->repo) || strlen(q+1)>=sizeof(entry->dest)) return -1;
  strcpy(entry->owner,text);
  strcpy(entry->repo,p+1);
  strcpy(entry->dest,q+1);
  return 0;
}

static int parse_github_line(char *line,struct GithubEntry *entry) {
  char *p,*source,*dest,*end;
  size_t n;

  p=line;
  for(;*p!=0 && isspace((unsigned char)*p);p++);
  if(*p==0 || *p=='#') return 0;
  source=p;
  for(;*p!=0 && !isspace((unsigned char)*p);p++);
  if(*p==0) return -1;
  *p++=0;
  for(;*p!=0 && isspace((unsigned char)*p);p++);
  if(*p==0 || *p=='#') return -1;
  dest=p;
  for(;*p!=0 && !isspace((unsigned char)*p);p++);
  if(*p!=0) {
    *p++=0;
    for(;*p!=0 && isspace((unsigned char)*p);p++);
    if(*p!=0 && *p!='#') return -1;
  }
  if(!valid_relpath(source,0)) return -1;
  n=strlen(source);
  if(n>=sizeof(entry->source)) return -1;
  strcpy(entry->source,source);
  end=dest+strlen(dest);
  if(end==dest) return -1;
  return split_github_destination(dest,entry)==0?1:-1;
}

static int read_github_map(const char *chat,struct GithubEntry **entries_out,int *count_out) {
  struct GithubEntry *entries,*tmp;
  char *text,*line,*next;
  int count,capacity,line_no,rc,i;

  text=api_read_text(chat,GITHUB_MAP);
  if(text==NULL) {
    fprintf(stderr,"cannot read %s from MCP work root\n",GITHUB_MAP);
    return -1;
  }
  entries=NULL;
  count=0;
  capacity=0;
  line_no=0;
  line=text;
  for(;;) {
    next=strchr(line,'\n');
    if(next!=NULL) *next=0;
    line_no++;
    if(count>=GITHUB_MAX_ENTRIES) {
      fprintf(stderr,"%s: too many entries\n",GITHUB_MAP);
      free(entries);
      free(text);
      return -1;
    }
    if(count==capacity) {
      capacity=capacity==0?32:capacity*2;
      tmp=(struct GithubEntry *)realloc(entries,(size_t)capacity*sizeof(*entries));
      if(tmp==NULL) {
        free(entries);
        free(text);
        return -1;
      }
      entries=tmp;
    }
    memset(&entries[count],0,sizeof(entries[count]));
    rc=parse_github_line(line,&entries[count]);
    if(rc<0) {
      fprintf(stderr,"%s:%d: invalid mapping\n",GITHUB_MAP,line_no);
      free(entries);
      free(text);
      return -1;
    }
    if(rc>0) count++;
    if(next==NULL) break;
    line=next+1;
  }
  free(text);
  for(i=0;i<count;i++) {
    int j;
    for(j=0;j<i;j++) {
      if(strcmp(entries[i].owner,entries[j].owner)==0 && strcmp(entries[i].repo,entries[j].repo)==0 && strcmp(entries[i].dest,entries[j].dest)==0) {
        fprintf(stderr,"%s: duplicate destination %s/%s/%s\n",GITHUB_MAP,entries[i].owner,entries[i].repo,entries[i].dest);
        free(entries);
        return -1;
      }
    }
    if(remote_size(chat,entries[i].source,&entries[i].size)!=0) {
      fprintf(stderr,"%s: source not readable: %s\n",GITHUB_MAP,entries[i].source);
      free(entries);
      return -1;
    }
  }
  *entries_out=entries;
  *count_out=count;
  return 0;
}

static int run_program(char *const argv[]) {
  pid_t pid;
  int status;

  pid=fork();
  if(pid<0) return -1;
  if(pid==0) {
    execvp(argv[0],argv);
    _exit(127);
  }
  if(waitpid(pid,&status,0)<0) return -1;
  if(!WIFEXITED(status)) return -1;
  return WEXITSTATUS(status);
}

static int remove_tree(const char *path) {
  DIR *d;
  struct dirent *de;
  struct stat st;
  char child[PATH_MAX];
  int n,rc;

  if(lstat(path,&st)!=0) return errno==ENOENT?0:-1;
  if(!S_ISDIR(st.st_mode) || S_ISLNK(st.st_mode)) return unlink(path);
  d=opendir(path);
  if(d==NULL) return -1;
  rc=0;
  for(;;) {
    de=readdir(d);
    if(de==NULL) break;
    if(strcmp(de->d_name,".")==0 || strcmp(de->d_name,"..")==0) continue;
    n=snprintf(child,sizeof(child),"%s/%s",path,de->d_name);
    if(n<0 || (size_t)n>=sizeof(child) || remove_tree(child)!=0) {
      rc=-1;
      break;
    }
  }
  closedir(d);
  if(rc!=0) return -1;
  return rmdir(path);
}

static int copy_remote_exact(const char *chat,const char *remote,const char *local,long size) {
  struct Blob blob;
  struct stat st;
  FILE *f;
  long offset;
  size_t written,chunk_len;

  if(lstat(local,&st)==0 && (S_ISLNK(st.st_mode) || !S_ISREG(st.st_mode))) {
    fprintf(stderr,"refusing non-regular destination: %s\n",local);
    return -1;
  }
  if(ensure_local_parent(local)!=0) return -1;
  f=fopen(local,"wb");
  if(f==NULL) return -1;
  offset=0;
  while(offset<size) {
    if(api_read_blob(chat,remote,offset,BLOB_CHUNK,&blob)!=0) {
      fclose(f);
      return -1;
    }
    if(blob.size!=size || blob.offset!=offset || blob.len==0) {
      free(blob.data);
      fclose(f);
      fprintf(stderr,"remote source changed while reading: %s\n",remote);
      return -1;
    }
    chunk_len=blob.len;
    written=fwrite(blob.data,1,chunk_len,f);
    free(blob.data);
    if(written!=chunk_len) {
      fclose(f);
      return -1;
    }
    offset+=(long)chunk_len;
  }
  return fclose(f)==0?0:-1;
}

static int github_repo_processed(struct GithubEntry *entries,int index) {
  int i;

  for(i=0;i<index;i++) {
    if(strcmp(entries[i].owner,entries[index].owner)==0 && strcmp(entries[i].repo,entries[index].repo)==0) return 1;
  }
  return 0;
}

static int github_repo_has_destination(struct GithubEntry *entries,int count,int index,const char *path) {
  int i;

  for(i=0;i<count;i++) {
    if(strcmp(entries[i].owner,entries[index].owner)!=0 || strcmp(entries[i].repo,entries[index].repo)!=0) continue;
    if(strcmp(entries[i].dest,path)==0) return 1;
  }
  return 0;
}

static int git_ls_files(const char *dir,char **data_out,size_t *len_out) {
  char *argv[6],*data,*tmp;
  unsigned char buf[4096];
  size_t len,capacity,needed,new_capacity;
  ssize_t nr;
  pid_t pid;
  int fd[2],status,read_error;

  *data_out=NULL;
  *len_out=0;
  if(pipe(fd)!=0) return -1;
  pid=fork();
  if(pid<0) {
    close(fd[0]);
    close(fd[1]);
    return -1;
  }
  if(pid==0) {
    close(fd[0]);
    if(dup2(fd[1],STDOUT_FILENO)<0) _exit(127);
    close(fd[1]);
    argv[0]="git"; argv[1]="-C"; argv[2]=(char *)dir; argv[3]="ls-files"; argv[4]="-z"; argv[5]=NULL;
    execvp(argv[0],argv);
    _exit(127);
  }
  close(fd[1]);
  data=NULL;
  len=0;
  capacity=0;
  read_error=0;
  for(;;) {
    nr=read(fd[0],buf,sizeof(buf));
    if(nr<0) {
      if(errno==EINTR) continue;
      read_error=1;
      break;
    }
    if(nr==0) break;
    needed=len+(size_t)nr+1;
    if(needed<len) {
      read_error=1;
      break;
    }
    if(needed>capacity) {
      new_capacity=capacity==0?4096:capacity;
      for(;new_capacity<needed;) {
        if(new_capacity>((size_t)-1)/2) {
          new_capacity=needed;
          break;
        }
        new_capacity*=2;
      }
      tmp=(char *)realloc(data,new_capacity);
      if(tmp==NULL) {
        read_error=1;
        break;
      }
      data=tmp;
      capacity=new_capacity;
    }
    memcpy(data+len,buf,(size_t)nr);
    len+=(size_t)nr;
  }
  close(fd[0]);
  if(waitpid(pid,&status,0)<0) {
    free(data);
    return -1;
  }
  if(read_error || !WIFEXITED(status) || WEXITSTATUS(status)!=0) {
    free(data);
    return -1;
  }
  if(data==NULL) {
    data=(char *)malloc(1);
    if(data==NULL) return -1;
  }
  data[len]=0;
  *data_out=data;
  *len_out=len;
  return 0;
}

static int remove_unmapped_github_files(struct GithubEntry *entries,int count,int index,const char *dir) {
  char *data,*path,*end;
  char *rm_argv[8],*clean_argv[7];
  size_t len;
  int deleted;

  if(git_ls_files(dir,&data,&len)!=0) {
    fprintf(stderr,"git ls-files failed: %s/%s\n",entries[index].owner,entries[index].repo);
    return -1;
  }
  deleted=0;
  path=data;
  end=data+len;
  for(;path<end;path+=strlen(path)+1) {
    if(github_repo_has_destination(entries,count,index,path)) continue;
    printf("DELETE %s/%s/%s\n",entries[index].owner,entries[index].repo,path);
    fflush(stdout);
    rm_argv[0]="git"; rm_argv[1]="-C"; rm_argv[2]=(char *)dir; rm_argv[3]="rm"; rm_argv[4]="-q"; rm_argv[5]="--"; rm_argv[6]=path; rm_argv[7]=NULL;
    if(run_program(rm_argv)!=0) {
      fprintf(stderr,"git rm failed: %s/%s/%s\n",entries[index].owner,entries[index].repo,path);
      free(data);
      return -1;
    }
    deleted++;
  }
  free(data);
  if(deleted>0) {
    clean_argv[0]="git"; clean_argv[1]="-C"; clean_argv[2]=(char *)dir; clean_argv[3]="clean"; clean_argv[4]="-fdq"; clean_argv[5]=NULL; clean_argv[6]=NULL;
    if(run_program(clean_argv)!=0) {
      fprintf(stderr,"git clean failed: %s/%s\n",entries[index].owner,entries[index].repo);
      return -1;
    }
  }
  return deleted;
}

static int process_github_repo(const char *chat,struct GithubEntry *entries,int count,int index,int repo_number) {
  char clone_url[512],dir[PATH_MAX],local[PATH_MAX];
  char *clone_argv[8],*add_argv[7],*commit_argv[7],*push_argv[7];
  struct stat st;
  int i,n,changed,state,rc;

  n=snprintf(clone_url,sizeof(clone_url),"git@github.com:%s/%s.git",entries[index].owner,entries[index].repo);
  if(n<0 || (size_t)n>=sizeof(clone_url)) return -1;
  n=snprintf(dir,sizeof(dir),"gmcp-github-%ld-%d",(long)getpid(),repo_number);
  if(n<0 || (size_t)n>=sizeof(dir)) return -1;
  if(lstat(dir,&st)==0 || errno!=ENOENT) {
    fprintf(stderr,"temporary path already exists: %s\n",dir);
    return -1;
  }
  clone_argv[0]="git"; clone_argv[1]="clone"; clone_argv[2]="--quiet"; clone_argv[3]="--depth"; clone_argv[4]="1"; clone_argv[5]=clone_url; clone_argv[6]=dir; clone_argv[7]=NULL;
  printf("REPO %s/%s\n",entries[index].owner,entries[index].repo);
  fflush(stdout);
  if(run_program(clone_argv)!=0) {
    fprintf(stderr,"git clone failed: %s/%s\n",entries[index].owner,entries[index].repo);
    remove_tree(dir);
    return -1;
  }
  changed=remove_unmapped_github_files(entries,count,index,dir);
  if(changed<0) {
    remove_tree(dir);
    return -1;
  }
  rc=0;
  for(i=0;i<count;i++) {
    if(strcmp(entries[i].owner,entries[index].owner)!=0 || strcmp(entries[i].repo,entries[index].repo)!=0) continue;
    n=snprintf(local,sizeof(local),"%s/%s",dir,entries[i].dest);
    if(n<0 || (size_t)n>=sizeof(local)) {
      rc=-1;
      break;
    }
    state=lstat(local,&st);
    if(state==0) {
      if(S_ISLNK(st.st_mode) || !S_ISREG(st.st_mode)) {
        fprintf(stderr,"refusing non-regular GitHub destination: %s/%s/%s\n",entries[i].owner,entries[i].repo,entries[i].dest);
        rc=-1;
        break;
      }
      if(remote_matches_local(chat,entries[i].source,local,entries[i].size)) {
        printf("SAME   %s -> %s/%s/%s\n",entries[i].source,entries[i].owner,entries[i].repo,entries[i].dest);
        continue;
      }
      printf("UPDATE %s -> %s/%s/%s\n",entries[i].source,entries[i].owner,entries[i].repo,entries[i].dest);
    } else if(errno==ENOENT) {
      printf("CREATE %s -> %s/%s/%s\n",entries[i].source,entries[i].owner,entries[i].repo,entries[i].dest);
    } else {
      rc=-1;
      break;
    }
    if(copy_remote_exact(chat,entries[i].source,local,entries[i].size)!=0) {
      rc=-1;
      break;
    }
    add_argv[0]="git"; add_argv[1]="-C"; add_argv[2]=dir; add_argv[3]="add"; add_argv[4]="--"; add_argv[5]=entries[i].dest; add_argv[6]=NULL;
    if(run_program(add_argv)!=0) {
      fprintf(stderr,"git add failed: %s\n",entries[i].dest);
      rc=-1;
      break;
    }
    changed++;
  }
  if(rc==0 && changed>0) {
    commit_argv[0]="git"; commit_argv[1]="-C"; commit_argv[2]=dir; commit_argv[3]="commit"; commit_argv[4]="-m"; commit_argv[5]="Update from MCP"; commit_argv[6]=NULL;
    if(run_program(commit_argv)!=0) {
      fprintf(stderr,"git commit failed: %s/%s\n",entries[index].owner,entries[index].repo);
      rc=-1;
    }
  }
  if(rc==0 && changed>0) {
    push_argv[0]="git"; push_argv[1]="-C"; push_argv[2]=dir; push_argv[3]="push"; push_argv[4]="origin"; push_argv[5]="HEAD"; push_argv[6]=NULL;
    if(run_program(push_argv)!=0) {
      fprintf(stderr,"git push failed: %s/%s\n",entries[index].owner,entries[index].repo);
      rc=-1;
    } else printf("PUSH   %s/%s files=%d\n",entries[index].owner,entries[index].repo,changed);
  }
  if(rc==0 && changed==0) printf("NO_CHANGE %s/%s\n",entries[index].owner,entries[index].repo);
  if(remove_tree(dir)!=0) fprintf(stderr,"warning: cannot remove temporary directory %s\n",dir);
  return rc;
}

static int cmd_github(const char *chat) {
  struct GithubEntry *entries;
  int count,i,repos,rc;

  entries=NULL;
  count=0;
  if(read_github_map(chat,&entries,&count)!=0) return 1;
  if(count==0) {
    printf("github: empty map\n");
    free(entries);
    return 0;
  }
  repos=0;
  rc=0;
  for(i=0;i<count;i++) {
    if(github_repo_processed(entries,i)) continue;
    repos++;
    if(process_github_repo(chat,entries,count,i,repos)!=0) {
      rc=1;
      break;
    }
  }
  if(rc==0) printf("github: entries=%d repositories=%d OK\n",count,repos);
  free(entries);
  return rc;
}

static int cmd_projects(const char *chat) {
  cJSON *list,*item,*path,*type;
  int i,n;

  list=api_list_files(chat,".",0);
  if(list==NULL) return 1;
  n=cJSON_GetArraySize(list);
  for(i=0;i<n;i++) {
    item=cJSON_GetArrayItem(list,i);
    path=cJSON_GetObjectItemCaseSensitive(item,"path");
    type=cJSON_GetObjectItemCaseSensitive(item,"type");
    if(cJSON_IsString(path) && cJSON_IsString(type) && strcmp(type->valuestring,"dir")==0)
      printf("%s\n",path->valuestring);
  }
  cJSON_Delete(list);
  return 0;
}

static int cmd_ls(const char *chat,const char *project,const char *rel) {
  char remote[PATH_MAX];
  cJSON *list,*item,*path,*type,*size;
  int i,n;

  if(make_path(remote,sizeof(remote),project,rel)!=0) return 2;
  list=api_list_files(chat,remote,1);
  if(list==NULL) return 1;
  n=cJSON_GetArraySize(list);
  for(i=0;i<n;i++) {
    item=cJSON_GetArrayItem(list,i);
    path=cJSON_GetObjectItemCaseSensitive(item,"path");
    type=cJSON_GetObjectItemCaseSensitive(item,"type");
    size=cJSON_GetObjectItemCaseSensitive(item,"size");
    if(cJSON_IsString(path) && cJSON_IsString(type) && cJSON_IsNumber(size))
      printf("%c %10ld %s\n",strcmp(type->valuestring,"dir")==0?'d':'f',(long)size->valuedouble,path->valuestring);
  }
  cJSON_Delete(list);
  return 0;
}

static int cmd_pull(const char *chat,const char *project) {
  cJSON *list,*item,*path,*type,*size;
  struct stat st;
  const char *name;
  int i,n,conflicts,errors;

  list=api_list_files(chat,project,1);
  if(list==NULL) return 1;
  if(lstat(project,&st)==0) {
    if(!S_ISDIR(st.st_mode) || S_ISLNK(st.st_mode)) {
      fprintf(stderr,"local project path is not a directory: %s\n",project);
      cJSON_Delete(list);
      return 1;
    }
  } else if(errno==ENOENT) {
    if(ensure_local_dir(project)!=0) {
      cJSON_Delete(list);
      return 1;
    }
  } else {
    cJSON_Delete(list);
    return 1;
  }
  n=cJSON_GetArraySize(list);
  conflicts=0;
  errors=0;
  for(i=0;i<n;i++) {
    item=cJSON_GetArrayItem(list,i);
    path=cJSON_GetObjectItemCaseSensitive(item,"path");
    type=cJSON_GetObjectItemCaseSensitive(item,"type");
    size=cJSON_GetObjectItemCaseSensitive(item,"size");
    if(!cJSON_IsString(path) || !cJSON_IsString(type) || !cJSON_IsNumber(size)) continue;
    name=path->valuestring;
    if(strncmp(name,project,strlen(project))!=0 || (name[strlen(project)]!='/' && name[strlen(project)]!=0)) {
      fprintf(stderr,"invalid remote path returned: %s\n",name);
      errors++;
      continue;
    }
    if(strcmp(type->valuestring,"dir")==0) {
      if(ensure_local_dir(name)!=0) {
        fprintf(stderr,"cannot create local directory: %s\n",name);
        errors++;
      }
    } else {
      switch(download_file(chat,name,name,(long)size->valuedouble,1)) {
        case 2: conflicts++; break;
        case 0: break;
        default: errors++; break;
      }
    }
  }
  cJSON_Delete(list);
  printf("pull project=%s conflicts=%d errors=%d\n",project,conflicts,errors);
  if(errors) return 1;
  if(conflicts) return 2;
  return 0;
}

static int cmd_get(const char *chat,const char *project,const char *rel) {
  char path[PATH_MAX];
  long size;

  if(make_path(path,sizeof(path),project,rel)!=0) return 2;
  if(remote_size(chat,path,&size)!=0) return 1;
  return download_file(chat,path,path,size,1)==0?0:1;
}

static int cmd_put(const char *chat,const char *project,const char *rel) {
  char path[PATH_MAX];

  if(make_path(path,sizeof(path),project,rel)!=0) return 2;
  return upload_file(chat,path,path)==0?0:1;
}

static int cmd_edit(const char *chat,const char *project,const char *rel) {
  char path[PATH_MAX];
  unsigned char *original,*local,*current;
  size_t original_len,local_len,current_len;
  struct stat st;
  int exists,rc;

  original=NULL;
  local=NULL;
  current=NULL;
  if(make_path(path,sizeof(path),project,rel)!=0) return 2;
  if(read_remote_all(chat,path,&original,&original_len)!=0) return 1;
  exists=lstat(path,&st)==0;
  if(exists) {
    if(read_local_all(path,&local,&local_len)!=0) {
      fprintf(stderr,"cannot read local file: %s\n",path);
      free(original);
      return 1;
    }
    if(local_len!=original_len || memcmp(local,original,original_len)!=0) {
      fprintf(stderr,"CONFLICT %s: local file differs from server\n",path);
      free(local);
      free(original);
      return 2;
    }
    free(local);
    local=NULL;
  } else if(errno==ENOENT) {
    if(write_local_all(path,original,original_len)!=0) {
      free(original);
      return 1;
    }
  } else {
    free(original);
    return 1;
  }
  if(run_editor(path)!=0) {
    fprintf(stderr,"editor failed\n");
    free(original);
    return 1;
  }
  if(read_local_all(path,&local,&local_len)!=0) {
    free(original);
    return 1;
  }
  if(local_len==original_len && memcmp(local,original,original_len)==0) {
    printf("NO_CHANGE %s\n",path);
    free(local);
    free(original);
    return 0;
  }
  if(read_remote_all(chat,path,&current,&current_len)!=0) {
    free(local);
    free(original);
    return 1;
  }
  if(current_len!=original_len || memcmp(current,original,original_len)!=0) {
    fprintf(stderr,"CONFLICT %s: server changed while editing\n",path);
    free(current);
    free(local);
    free(original);
    return 2;
  }
  free(current);
  free(original);
  rc=upload_file(chat,path,path);
  free(local);
  return rc==0?0:1;
}

static int cmd_diff(const char *chat,const char *project) {
  cJSON *list,*item,*path,*type,*size;
  struct stat st;
  const char *name;
  int i,n,differences,same,local_only;

  list=api_list_files(chat,project,1);
  if(list==NULL) return 1;
  differences=0;
  same=0;
  n=cJSON_GetArraySize(list);
  for(i=0;i<n;i++) {
    item=cJSON_GetArrayItem(list,i);
    path=cJSON_GetObjectItemCaseSensitive(item,"path");
    type=cJSON_GetObjectItemCaseSensitive(item,"type");
    size=cJSON_GetObjectItemCaseSensitive(item,"size");
    if(!cJSON_IsString(path) || !cJSON_IsString(type) || !cJSON_IsNumber(size)) continue;
    name=path->valuestring;
    if(lstat(name,&st)!=0) {
      if(errno==ENOENT) {
        printf("REMOTE_ONLY %s\n",name);
        differences++;
        continue;
      }
      cJSON_Delete(list);
      return 1;
    }
    if(strcmp(type->valuestring,"dir")==0) {
      if(!S_ISDIR(st.st_mode) || S_ISLNK(st.st_mode)) {
        printf("MODIFIED %s\n",name);
        differences++;
      } else same++;
    } else {
      if(!S_ISREG(st.st_mode) || S_ISLNK(st.st_mode) || !remote_matches_local(chat,name,name,(long)size->valuedouble)) {
        printf("MODIFIED %s\n",name);
        differences++;
      } else same++;
    }
  }
  local_only=0;
  if(lstat(project,&st)==0 && S_ISDIR(st.st_mode) && !S_ISLNK(st.st_mode)) {
    if(walk_local_only(project,list,&local_only)!=0) {
      cJSON_Delete(list);
      return 1;
    }
  } else {
    printf("REMOTE_ONLY %s\n",project);
    differences++;
  }
  differences+=local_only;
  cJSON_Delete(list);
  printf("diff project=%s same=%d differences=%d\n",project,same,differences);
  return differences?2:0;
}

static const char *effective_chat(const char *override,const char *project) {
  if(override!=NULL) return override;
  if(project!=NULL) return project;
  return "gmcp";
}

static int cwd_in_project(const char *cwd,const char *project) {
  char prefix[PATH_MAX];
  size_t n;

  if(snprintf(prefix,sizeof(prefix),"%s%s",WORK_PREFIX,project)>=(int)sizeof(prefix)) return 0;
  n=strlen(prefix);
  if(strncmp(cwd,prefix,n)!=0) return 0;
  return cwd[n]==0 || cwd[n]=='/';
}

static void print_elapsed(double seconds) {
  long total,days,hours,minutes,secs;

  if(seconds<0) seconds=0;
  total=(long)seconds;
  days=total/86400;
  hours=(total%86400)/3600;
  minutes=(total%3600)/60;
  secs=total%60;
  if(days>0) printf("%ldd %02ld:%02ld:%02ld",days,hours,minutes,secs);
  else printf("%02ld:%02ld:%02ld",hours,minutes,secs);
}

static int cmd_jobs(const char *chat,const char *project) {
  cJSON *args,*list,*item,*state,*cwd,*job_id,*elapsed,*pid,*cpu,*rss,*command;
  int i,n,count;

  args=cJSON_CreateObject();
  cJSON_AddNumberToObject(args,"limit",1000);
  list=mcp_call(chat,"jobs",args);
  if(list==NULL) return 1;
  if(!cJSON_IsArray(list)) {
    cJSON_Delete(list);
    return 1;
  }
  n=cJSON_GetArraySize(list);
  count=0;
  for(i=0;i<n;i++) {
    item=cJSON_GetArrayItem(list,i);
    state=cJSON_GetObjectItemCaseSensitive(item,"state");
    cwd=cJSON_GetObjectItemCaseSensitive(item,"cwd");
    if(!cJSON_IsString(state) || strcmp(state->valuestring,"running")!=0 || !cJSON_IsString(cwd)) continue;
    if(!cwd_in_project(cwd->valuestring,project)) continue;
    job_id=cJSON_GetObjectItemCaseSensitive(item,"job_id");
    elapsed=cJSON_GetObjectItemCaseSensitive(item,"elapsed_seconds");
    pid=cJSON_GetObjectItemCaseSensitive(item,"pid");
    cpu=cJSON_GetObjectItemCaseSensitive(item,"cpu_percent");
    rss=cJSON_GetObjectItemCaseSensitive(item,"rss_bytes");
    command=cJSON_GetObjectItemCaseSensitive(item,"command");
    printf("%s pid=%ld elapsed=",cJSON_IsString(job_id)?job_id->valuestring:"?",cJSON_IsNumber(pid)?(long)pid->valuedouble:0L);
    print_elapsed(cJSON_IsNumber(elapsed)?elapsed->valuedouble:0.0);
    printf(" cpu=%.1f%% rss=%.1fMB %s\n",cJSON_IsNumber(cpu)?cpu->valuedouble:0.0,cJSON_IsNumber(rss)?rss->valuedouble/1048576.0:0.0,cJSON_IsString(command)?command->valuestring:"");
    count++;
  }
  if(count==0) printf("no running jobs for project=%s chat=%s\n",project,chat);
  cJSON_Delete(list);
  return 0;
}

static char *join_command(int argc,char **argv,int first) {
  char *out,*p;
  size_t len;
  int i;

  len=1;
  for(i=first;i<argc;i++) len+=strlen(argv[i])+1;
  out=(char *)malloc(len);
  if(out==NULL) return NULL;
  p=out;
  for(i=first;i<argc;i++) {
    if(i>first) *p++=' ';
    memcpy(p,argv[i],strlen(argv[i]));
    p+=strlen(argv[i]);
  }
  *p=0;
  return out;
}

static int cmd_run(const char *chat,const char *project,const char *command) {
  cJSON *args,*result,*value;

  args=cJSON_CreateObject();
  cJSON_AddStringToObject(args,"command",command);
  cJSON_AddStringToObject(args,"cwd",project);
  result=mcp_call(chat,"run",args);
  if(result==NULL) return 1;
  value=cJSON_GetObjectItemCaseSensitive(result,"result");
  if(cJSON_IsString(value)) printf("%s",value->valuestring);
  else {
    char *text;
    text=cJSON_Print(result);
    if(text!=NULL) {
      printf("%s\n",text);
      free(text);
    }
  }
  cJSON_Delete(result);
  return 0;
}

static int cmd_start(const char *chat,const char *project,const char *command) {
  cJSON *args,*result,*job_id,*pid,*state;

  args=cJSON_CreateObject();
  cJSON_AddStringToObject(args,"command",command);
  cJSON_AddStringToObject(args,"cwd",project);
  result=mcp_call(chat,"start",args);
  if(result==NULL) return 1;
  job_id=cJSON_GetObjectItemCaseSensitive(result,"job_id");
  pid=cJSON_GetObjectItemCaseSensitive(result,"pid");
  state=cJSON_GetObjectItemCaseSensitive(result,"state");
  printf("job_id=%s pid=%ld state=%s chat=%s\n",cJSON_IsString(job_id)?job_id->valuestring:"?",cJSON_IsNumber(pid)?(long)pid->valuedouble:0L,cJSON_IsString(state)?state->valuestring:"?",chat);
  cJSON_Delete(result);
  return 0;
}

static int print_json_result(cJSON *result) {
  char *text;

  if(result==NULL) return 1;
  text=cJSON_Print(result);
  if(text==NULL) {
    cJSON_Delete(result);
    return 1;
  }
  printf("%s\n",text);
  free(text);
  cJSON_Delete(result);
  return 0;
}

static int cmd_status(const char *chat,const char *job_id) {
  cJSON *args;

  args=cJSON_CreateObject();
  cJSON_AddStringToObject(args,"job_id",job_id);
  return print_json_result(mcp_call(chat,"status",args));
}

static int cmd_tail(const char *chat,const char *job_id,int lines,const char *stream) {
  cJSON *args,*result,*out,*err;

  args=cJSON_CreateObject();
  cJSON_AddStringToObject(args,"job_id",job_id);
  cJSON_AddNumberToObject(args,"lines",lines);
  cJSON_AddStringToObject(args,"stream",stream);
  result=mcp_call(chat,"tail",args);
  if(result==NULL) return 1;
  out=cJSON_GetObjectItemCaseSensitive(result,"stdout");
  err=cJSON_GetObjectItemCaseSensitive(result,"stderr");
  if(cJSON_IsString(out) && out->valuestring[0]!=0) printf("%s",out->valuestring);
  if(cJSON_IsString(err) && err->valuestring[0]!=0) fprintf(stderr,"%s",err->valuestring);
  cJSON_Delete(result);
  return 0;
}

static int cmd_stop(const char *chat,const char *job_id,int force) {
  cJSON *args;

  args=cJSON_CreateObject();
  cJSON_AddStringToObject(args,"job_id",job_id);
  cJSON_AddBoolToObject(args,"force",force?1:0);
  return print_json_result(mcp_call(chat,"stop",args));
}

static int cmd_hello(const char *chat) {
  cJSON *result,*value;

  result=mcp_call(chat,"hello",cJSON_CreateObject());
  if(result==NULL) return 1;
  value=cJSON_GetObjectItemCaseSensitive(result,"result");
  if(cJSON_IsString(value)) printf("%s\n",value->valuestring);
  cJSON_Delete(result);
  return 0;
}

static void usage(const char *prog) {
  printf("gmcp %s\n",GMCP_VERSION);
  printf("usage: %s [-c chat] command ...\n\n",prog);
  printf("  hello\n");
  printf("  projects\n");
  printf("  github                 synchronize GitHub exactly to github.map\n");
  printf("  ls <project> [path]\n");
  printf("  pull <project>\n");
  printf("  get <project> <path>\n");
  printf("  put <project> <path>\n");
  printf("  edit <project> <path>\n");
  printf("  diff <project>\n");
  printf("  jobs <project>\n");
  printf("  run <project> <command...>\n");
  printf("  start <project> <command...>\n");
  printf("  status <project> <job_id>\n");
  printf("  tail <project> <job_id> [lines] [stdout|stderr|both]\n");
  printf("  stop <project> <job_id> [force]\n\n");
  printf("default endpoint: %s\n",DEFAULT_URL);
  printf("token: ~/mcp/token.txt\n");
  printf("default chat: project name; override with -c chat\n");
  printf("environment: GMCP_URL, GMCP_TOKEN, EDITOR\n");
}

int main(int argc,char **argv) {
  const char *chat_override,*cmd,*project,*chat,*rel,*stream;
  char *command;
  int i,lines,force,rc;

  chat_override=NULL;
  i=1;
  if(i<argc && strcmp(argv[i],"-c")==0) {
    if(i+1>=argc || !valid_chat(argv[i+1])) {
      fprintf(stderr,"invalid or missing chat\n");
      return 2;
    }
    chat_override=argv[i+1];
    i+=2;
  }
  if(i>=argc || strcmp(argv[i],"help")==0 || strcmp(argv[i],"--help")==0 || strcmp(argv[i],"-h")==0) {
    usage(argv[0]);
    return 0;
  }
  cmd=argv[i++];
  if(curl_global_init(CURL_GLOBAL_DEFAULT)!=CURLE_OK) return 1;
  rc=2;
  if(strcmp(cmd,"hello")==0) {
    chat=effective_chat(chat_override,NULL);
    rc=cmd_hello(chat);
  } else if(strcmp(cmd,"projects")==0) {
    chat=effective_chat(chat_override,NULL);
    rc=cmd_projects(chat);
  } else if(strcmp(cmd,"github")==0) {
    chat=effective_chat(chat_override,NULL);
    rc=cmd_github(chat);
  } else {
    if(i>=argc || !valid_project(argv[i])) {
      fprintf(stderr,"project is required and must be a simple directory name\n");
      usage(argv[0]);
      curl_global_cleanup();
      return 2;
    }
    project=argv[i++];
    chat=effective_chat(chat_override,project);
    if(strcmp(cmd,"ls")==0) {
      rel=i<argc?argv[i]:"";
      rc=cmd_ls(chat,project,rel);
    } else if(strcmp(cmd,"pull")==0) {
      rc=cmd_pull(chat,project);
    } else if(strcmp(cmd,"get")==0) {
      if(i>=argc || !valid_relpath(argv[i],0)) fprintf(stderr,"path is required\n");
      else rc=cmd_get(chat,project,argv[i]);
    } else if(strcmp(cmd,"put")==0) {
      if(i>=argc || !valid_relpath(argv[i],0)) fprintf(stderr,"path is required\n");
      else rc=cmd_put(chat,project,argv[i]);
    } else if(strcmp(cmd,"edit")==0) {
      if(i>=argc || !valid_relpath(argv[i],0)) fprintf(stderr,"path is required\n");
      else rc=cmd_edit(chat,project,argv[i]);
    } else if(strcmp(cmd,"diff")==0) {
      rc=cmd_diff(chat,project);
    } else if(strcmp(cmd,"jobs")==0) {
      rc=cmd_jobs(chat,project);
    } else if(strcmp(cmd,"run")==0 || strcmp(cmd,"start")==0) {
      if(i>=argc) fprintf(stderr,"command is required\n");
      else {
        command=join_command(argc,argv,i);
        if(command==NULL) rc=1;
        else {
          if(strcmp(cmd,"run")==0) rc=cmd_run(chat,project,command);
          else rc=cmd_start(chat,project,command);
          free(command);
        }
      }
    } else if(strcmp(cmd,"status")==0) {
      if(i>=argc) fprintf(stderr,"job_id is required\n");
      else rc=cmd_status(chat,argv[i]);
    } else if(strcmp(cmd,"tail")==0) {
      if(i>=argc) fprintf(stderr,"job_id is required\n");
      else {
        lines=50;
        stream="stdout";
        if(i+1<argc) lines=atoi(argv[i+1]);
        if(i+2<argc) stream=argv[i+2];
        if(lines<1 || lines>1000 || (strcmp(stream,"stdout")!=0 && strcmp(stream,"stderr")!=0 && strcmp(stream,"both")!=0))
          fprintf(stderr,"invalid tail arguments\n");
        else rc=cmd_tail(chat,argv[i],lines,stream);
      }
    } else if(strcmp(cmd,"stop")==0) {
      if(i>=argc) fprintf(stderr,"job_id is required\n");
      else {
        force=i+1<argc && strcmp(argv[i+1],"force")==0;
        rc=cmd_stop(chat,argv[i],force);
      }
    } else {
      fprintf(stderr,"unknown command: %s\n",cmd);
      usage(argv[0]);
    }
  }
  curl_global_cleanup();
  return rc;
}
