// Gianluca Mazzini @2026- Version 1.01

#ifndef MCP_DRIVE_H
#define MCP_DRIVE_H

#include <cjson/cJSON.h>
#include <stddef.h>

int mcp_drive_list(const char *path,int recursive,cJSON **data,char *error,size_t error_size);
int mcp_drive_stat(const char *path,cJSON **data,char *error,size_t error_size);
int mcp_drive_read_blob(const char *path,long offset,int length,cJSON **data,char *error,size_t error_size);
int mcp_drive_get_file(const char *path,const char *local_path,cJSON **data,char *error,size_t error_size);
int mcp_drive_put_file(const char *path,const char *local_path,const char *expected_version,cJSON **data,char *error,size_t error_size);
int mcp_drive_write_blob(const char *chat,const char *path,const char *expected_version,long offset,const unsigned char *buf,size_t len,int truncate,int commit,cJSON **data,char *error,size_t error_size);
int mcp_drive_mkdir(const char *path,cJSON **data,char *error,size_t error_size);
int mcp_drive_rename(const char *path,const char *new_name,const char *expected_version,cJSON **data,char *error,size_t error_size);
int mcp_drive_delete(const char *path,const char *expected_version,cJSON **data,char *error,size_t error_size);

#endif
