#!/usr/bin/env python3

import base64
import json
import os
import subprocess
import sys
import time
import threading
import http.server
import hashlib
import struct
import re
import signal
import urllib.parse

PORT=18080
DRIVE_PORT=18081
CHROME_PORT=18082
URL="http://127.0.0.1:%d/mcp" % PORT
CHAT="mymcp-test"
AGENT_CONFIG=os.environ.get("MYMCP_AGENT_CONFIG")
AGENT_DIR=os.environ.get("MYMCP_AGENT_DIR")
META={
  "io.modelcontextprotocol/protocolVersion":"2026-07-28",
  "io.modelcontextprotocol/clientCapabilities":{},
  "io.modelcontextprotocol/clientInfo":{"name":"mymcp-test","version":"1.02"}
}


def request(req_id,method,name=None,arguments=None):
  params={"_meta":META}
  headers=[
    "Content-Type: application/json",
    "MCP-Protocol-Version: 2026-07-28",
    "Mcp-Method: "+method
  ]
  if name is not None:
    params["name"]=name
    params["arguments"]=arguments or {}
    headers.append("Mcp-Name: "+name)
  body={"jsonrpc":"2.0","id":req_id,"method":method,"params":params}
  command=["curl","-sS","-X","POST",URL]
  for header in headers:
    command.extend(["-H",header])
  command.extend(["--data",json.dumps(body)])
  return json.loads(subprocess.check_output(command,text=True))


def tool(req_id,name,arguments=None):
  args={} if arguments is None else dict(arguments)
  args["chat"]=CHAT
  return request(req_id,"tools/call",name,args)["result"]


def agent_request(action,body=None,token="test-token"):
  command=["curl","-sS","-X","POST",URL,
    "-H","X-MCP-Agent: test-agent",
    "-H","X-MCP-Agent-Action: "+action,
    "-H","X-MCP-Agent-Token: "+token]
  if body is not None:
    command.extend(["-H","Content-Type: application/json","--data",json.dumps(body)])
  return json.loads(subprocess.check_output(command,text=True))


def fake_agent_once():
  request_data=agent_request("wait")
  check(request_data.get("status")=="request","agent wait did not receive request")
  check(request_data.get("module")=="test" and request_data.get("action")=="echo","wrong agent request")
  payload=request_data.get("payload")
  result=agent_request("result",{
    "request_id":request_data["request_id"],
    "status":"ok",
    "result":{"echo":payload}
  })
  check(result.get("status")=="accepted","agent result was not accepted")


def fake_edistribuzione_once():
  request_data=agent_request("wait")
  check(request_data.get("status")=="request","edistribuzione agent wait did not receive request")
  check(request_data.get("module")=="edistribuzione" and request_data.get("action")=="load_profile.month",
    "wrong edistribuzione agent request")
  payload=request_data.get("payload",{})
  check(payload.get("year")==2026 and payload.get("month")==9 and "magnitude" not in payload,
    "wrong edistribuzione CLI payload")
  result=agent_request("result",{
    "request_id":request_data["request_id"],
    "status":"ok",
    "result":{
      "code":"OK",
      "pod":"TESTPOD",
      "year":2026,
      "month":9,
      "days":[{
        "date_key":"20260901",
        "samples":[{"key":"1","value":1.5},{"key":"96","value":2.5}]
      }]
    }
  })
  check(result.get("status")=="accepted","edistribuzione agent result was not accepted")


def check(condition,message):
  if not condition:
    raise RuntimeError(message)


class MockDriveHandler(http.server.BaseHTTPRequestHandler):
  files={
    "root1":{"id":"root1","name":"Docs","mimeType":"application/vnd.google-apps.folder","version":1,"modifiedTime":"2026-09-09T00:00:00Z","parent":None,"trashed":False,"content":b""},
    "file1":{"id":"file1","name":"doc.docx","mimeType":"application/vnd.openxmlformats-officedocument.wordprocessingml.document","version":1,"modifiedTime":"2026-09-09T00:00:01Z","parent":"root1","trashed":False,"content":b"abcdef"}
  }
  next_id=2

  def log_message(self,format,*args):
    pass

  def metadata(self,item):
    data={"id":item["id"],"name":item["name"],"mimeType":item["mimeType"],"version":str(item["version"]),"modifiedTime":item["modifiedTime"]}
    if item["mimeType"]!="application/vnd.google-apps.folder":
      data["size"]=str(len(item["content"]))
    return data

  def send_bytes(self,status,data,content_type="application/json"):
    self.send_response(status)
    self.send_header("Content-Type",content_type)
    self.send_header("Content-Length",str(len(data)))
    self.end_headers()
    self.wfile.write(data)

  def send_json(self,status,obj):
    self.send_bytes(status,json.dumps(obj,separators=(",",":")).encode("utf-8"))

  def item_from_path(self,path,prefix):
    if not path.startswith(prefix):
      return None
    file_id=path[len(prefix):].split("/",1)[0]
    return self.files.get(file_id)

  def do_GET(self):
    parsed=urllib.parse.urlparse(self.path)
    qs=urllib.parse.parse_qs(parsed.query)
    if parsed.path=="/drive/v3/files":
      query=qs.get("q",[""])[0]
      parent_match=re.search(r"'([^']+)' in parents",query)
      name_match=re.search(r"name = '([^']*)'",query)
      parent=parent_match.group(1) if parent_match else None
      name=name_match.group(1).replace("\\'","'").replace("\\\\","\\") if name_match else None
      matches=[]
      for item in self.files.values():
        if item["trashed"] or item["parent"]!=parent:
          continue
        if name is not None and item["name"]!=name:
          continue
        matches.append(self.metadata(item))
      self.send_json(200,{"files":matches[:1000]})
      return
    item=self.item_from_path(parsed.path,"/drive/v3/files/")
    if item is None or item["trashed"]:
      self.send_json(404,{"error":{"message":"not found"}})
      return
    if qs.get("alt")==["media"]:
      data=item["content"]
      header=self.headers.get("Range")
      if header:
        match=re.match(r"bytes=(\d+)-(\d+)",header)
        if not match:
          self.send_json(416,{"error":{"message":"bad range"}})
          return
        start=int(match.group(1)); end=int(match.group(2))
        part=data[start:end+1]
        self.send_bytes(206,part,"application/octet-stream")
      else:
        self.send_bytes(200,data,"application/octet-stream")
      return
    self.send_json(200,self.metadata(item))

  def do_POST(self):
    parsed=urllib.parse.urlparse(self.path)
    if parsed.path=="/googleauth":
      length=int(self.headers.get("Content-Length","0"))
      body=self.rfile.read(length).decode("utf-8")
      form=urllib.parse.parse_qs(body,keep_blank_values=True)
      content_type=self.headers.get("Content-Type","")
      if not content_type.startswith("application/x-www-form-urlencoded"):
        self.send_bytes(400,b"bad content type\n","text/plain")
        return
      if form.get("action")!=["token"] or form.get("channel")!=["mymcp"] or form.get("key")!=["test-api-key"]:
        self.send_bytes(403,b"forbidden\n","text/plain")
        return
      self.send_bytes(200,b"test-token\n","text/plain")
      return
    if parsed.path!="/drive/v3/files":
      self.send_json(404,{"error":{"message":"not found"}})
      return
    length=int(self.headers.get("Content-Length","0"))
    body=json.loads(self.rfile.read(length).decode("utf-8"))
    file_id="new%d" % self.__class__.next_id
    self.__class__.next_id+=1
    item={"id":file_id,"name":body["name"],"mimeType":body.get("mimeType","application/octet-stream"),"version":1,"modifiedTime":"2026-09-09T00:00:10Z","parent":body.get("parents",[None])[0],"trashed":False,"content":b""}
    self.files[file_id]=item
    self.send_json(200,self.metadata(item))

  def do_PATCH(self):
    parsed=urllib.parse.urlparse(self.path)
    upload_prefix="/upload/drive/v3/files/"
    meta_prefix="/drive/v3/files/"
    if parsed.path.startswith(upload_prefix):
      item=self.item_from_path(parsed.path,upload_prefix)
      if item is None:
        self.send_json(404,{"error":{"message":"not found"}})
        return
      length=int(self.headers.get("Content-Length","0"))
      item["content"]=self.rfile.read(length)
      item["mimeType"]=self.headers.get("Content-Type",item["mimeType"])
      item["version"]+=1
      item["modifiedTime"]="2026-09-09T00:00:20Z"
      self.send_json(200,self.metadata(item))
      return
    if parsed.path.startswith(meta_prefix):
      item=self.item_from_path(parsed.path,meta_prefix)
      if item is None:
        self.send_json(404,{"error":{"message":"not found"}})
        return
      length=int(self.headers.get("Content-Length","0"))
      body=json.loads(self.rfile.read(length).decode("utf-8"))
      if "name" in body:
        item["name"]=body["name"]
      if body.get("trashed") is True:
        item["trashed"]=True
      item["version"]+=1
      item["modifiedTime"]="2026-09-09T00:00:30Z"
      self.send_json(200,self.metadata(item))
      return
    self.send_json(404,{"error":{"message":"not found"}})


def start_mock_drive():
  server=http.server.HTTPServer(("127.0.0.1",DRIVE_PORT),MockDriveHandler)
  thread=threading.Thread(target=server.serve_forever)
  thread.daemon=True
  thread.start()
  return server,thread


class MockChromeHandler(http.server.BaseHTTPRequestHandler):
  protocol_version="HTTP/1.1"
  requests=0

  def log_message(self,format,*args):
    pass

  def websocket_frame(self,payload):
    data=payload.encode("utf-8")
    if len(data)<126:
      return bytes([0x81,len(data)])+data
    return bytes([0x81,126])+struct.pack("!H",len(data))+data

  def websocket_read(self):
    header=self.rfile.read(2)
    if len(header)!=2:
      return None
    length=header[1]&0x7f
    masked=(header[1]&0x80)!=0
    if length==126:
      length=struct.unpack("!H",self.rfile.read(2))[0]
    elif length==127:
      length=struct.unpack("!Q",self.rfile.read(8))[0]
    mask=self.rfile.read(4) if masked else b""
    data=bytearray(self.rfile.read(length))
    if masked:
      for i in range(len(data)):
        data[i]^=mask[i%4]
    return data.decode("utf-8")

  def websocket_session(self):
    key=self.headers.get("Sec-WebSocket-Key","")
    accept=base64.b64encode(hashlib.sha1((key+"258EAFA5-E914-47DA-95CA-C5AB0DC85B11").encode("ascii")).digest()).decode("ascii")
    self.send_response(101,"Switching Protocols")
    self.send_header("Upgrade","websocket")
    self.send_header("Connection","Upgrade")
    self.send_header("Sec-WebSocket-Accept",accept)
    self.end_headers()
    for _ in range(4):
      text=self.websocket_read()
      if text is None:
        return
      request=json.loads(text)
      req_id=request.get("id",0)
      if request.get("method")=="Runtime.evaluate":
        reply={"id":req_id,"result":{"result":{"type":"string","value":"READY"}}}
      else:
        reply={"id":req_id,"result":{}}
      self.wfile.write(self.websocket_frame(json.dumps(reply,separators=(",",":"))))
      self.wfile.flush()

  def do_GET(self):
    self.__class__.requests+=1
    if self.headers.get("Upgrade","").lower()=="websocket":
      self.websocket_session()
      return
    if self.path!="/json/list":
      self.send_response(404)
      self.end_headers()
      return
    body=json.dumps([{
      "type":"page",
      "title":CHAT+" 2",
      "url":"https://chatgpt.com/c/abcdefgh12345678",
      "webSocketDebuggerUrl":"ws://127.0.0.1:%d/devtools/page/test" % CHROME_PORT
    }]).encode("utf-8")
    self.send_response(200)
    self.send_header("Content-Type","application/json")
    self.send_header("Content-Length",str(len(body)))
    self.end_headers()
    self.wfile.write(body)


def start_mock_chrome():
  server=http.server.HTTPServer(("127.0.0.1",CHROME_PORT),MockChromeHandler)
  thread=threading.Thread(target=server.serve_forever)
  thread.daemon=True
  thread.start()
  return server,thread


def test_watch_check_path(root):
  source=os.path.join(root,"send_check_test.c")
  binary=os.path.join(root,"send_check_test")
  with open(source,"w",encoding="utf-8") as f:
    f.write('int mcp_agent_original_main(int argc,char **argv);\n')
    f.write('#define main mcp_agent_original_main\n')
    f.write('#include "../../mcp_agent.c"\n')
    f.write('#undef main\n\n')
    f.write('int main(void) {\n')
    f.write('  struct ChatState state;\n')
    f.write('  int rc;\n\n')
    f.write('  memset(&state,0,sizeof(state));\n')
    f.write('  snprintf(state.chat,sizeof(state.chat),"%s");\n' % CHAT)
    f.write('  snprintf(state.conversation_id,sizeof(state.conversation_id),"abcdefgh12345678");\n')
    f.write('  setenv("MCP_CHROME_URL","http://127.0.0.1:%d",1);\n' % CHROME_PORT)
    f.write('  if(curl_global_init(CURL_GLOBAL_DEFAULT)!=CURLE_OK) return 2;\n')
    f.write('  rc=send_check(&state);\n')
    f.write('  curl_global_cleanup();\n')
    f.write('  return rc==0?0:1;\n')
    f.write('}\n')
  compile_cmd=["gcc","-std=gnu89","-O2","-Wall","-Wextra","-Wshadow","-Wstrict-prototypes",
    "-Wmissing-prototypes","-Wconversion","-Wno-sign-conversion",source,"-lcjson","-lcurl","-o",binary]
  subprocess.check_call(compile_cmd)
  run=subprocess.run([binary],capture_output=True,text=True,timeout=10)
  check(run.returncode==0,"integrated watcher CDP check path failed: "+run.stdout+run.stderr)
  check("WATCH check sent" in run.stdout,"integrated watcher check success log missing")


def main():
  server=None
  drive_server=None
  drive_thread=None
  chrome_server=None
  chrome_thread=None
  try:
    drive_root=os.path.abspath("tmpdata/regression")
    os.makedirs(os.path.join(drive_root,"drive-stage"),exist_ok=True)
    drive_map=os.path.join(drive_root,"drive.map")
    googleauth_config=os.path.join(drive_root,"mymcp.conf")
    with open(drive_map,"w",encoding="utf-8") as f:
      f.write("docs root1 rw\nreadonly root1 ro\n")
    with open(googleauth_config,"w",encoding="utf-8") as f:
      f.write("googleauth_key=test-api-key\n")
    os.chmod(googleauth_config,0o600)
    drive_server,drive_thread=start_mock_drive()
    chrome_server,chrome_thread=start_mock_chrome()
    test_watch_check_path(drive_root)
    env=os.environ.copy()
    env["MYMCP_DRIVE_MAP"]=drive_map
    env["MYMCP_GOOGLEAUTH_CONFIG"]=googleauth_config
    env["MYMCP_GOOGLEAUTH_URL"]="http://127.0.0.1:%d/googleauth" % DRIVE_PORT
    env["MYMCP_DRIVE_STAGE"]=os.path.join(drive_root,"drive-stage")
    env["MYMCP_DRIVE_API"]="http://127.0.0.1:%d/drive/v3" % DRIVE_PORT
    env["MYMCP_DRIVE_UPLOAD_API"]="http://127.0.0.1:%d/upload/drive/v3" % DRIVE_PORT
    server=subprocess.Popen([os.environ.get("MYMCP_BIN","./mymcp"),"-p",str(PORT)],stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL,env=env)
    time.sleep(.3)
    discover=request(1,"server/discover")["result"]
    check(discover["supportedVersions"]==["2026-07-28"],"discover failed")
    check("chat" in discover["instructions"],"discover chat instruction missing")
    check("explicitly authorized" in discover["instructions"] and "do not use sleep-based" in discover["instructions"],"discover owner authorization rule missing")
    tools=request(2,"tools/list")["result"]["tools"]
    check([item["name"] for item in tools]==["hello","write_file","read_file","list_files","read_blob","write_blob","drive_list","drive_stat","drive_read_blob","drive_get_file","drive_put_file","drive_write_blob","drive_mkdir","drive_rename","drive_delete","run","start","status","tail","stop","jobs","agent_call"],"tools/list failed")
    for item in tools:
      required=item["inputSchema"].get("required",[])
      check(required and required[0]=="chat","chat is not first required field for "+item["name"])
    check(tool(3,"hello")["structuredContent"]["result"]=="hello from mymcp","hello failed")
    check(not tool(4,"write_file",{"path":"mymcp/testdata/test.txt","content":"alpha\nbeta\n"})["isError"],"write_file failed")
    check(tool(5,"read_file",{"path":"mymcp/testdata/test.txt"})["structuredContent"]["result"]=="alpha\nbeta\n","read_file failed")
    result=tool(6,"run",{"command":"printf 'OUT\\n'; printf 'ERR\\n' >&2; exit 7","cwd":"mymcp"})["structuredContent"]["result"]
    check("exit_code=7" in result and "OUT" in result and "ERR" in result,"run failed")
    started=tool(7,"start",{"command":"echo READY; sleep 2; echo DONE","cwd":"mymcp"})["structuredContent"]
    job_id=started["job_id"]
    check(started["chat"]==CHAT,"start chat missing")
    time.sleep(.3)
    status=tool(8,"status",{"job_id":job_id})["structuredContent"]
    check(status["state"]=="running" and status["chat"]==CHAT,"status running failed")
    check("READY" in tool(9,"tail",{"job_id":job_id,"lines":10,"stream":"both"})["structuredContent"]["stdout"],"tail failed")
    bad=request(10,"tools/call","status",{"chat":"other-chat","job_id":job_id})["result"]
    check(bad["isError"],"cross-chat status was accepted")
    time.sleep(2.2)
    status=tool(11,"status",{"job_id":job_id})["structuredContent"]
    check(status["state"]=="exited" and status["exit_code"]==0,"status exited failed")
    reload_started=tool(49,"start",{"command":"echo RELOAD_READY; sleep 10","cwd":"mymcp"})["structuredContent"]
    reload_job=reload_started["job_id"]
    time.sleep(.3)
    server_pid=server.pid
    os.kill(server_pid,signal.SIGHUP)
    time.sleep(.4)
    check(server.poll() is None and server.pid==server_pid,"server did not survive SIGHUP reload with the same PID")
    reload_status=tool(50,"status",{"job_id":reload_job})["structuredContent"]
    check(reload_status["state"]=="running","running job did not survive server reload")
    check("RELOAD_READY" in tool(51,"tail",{"job_id":reload_job,"lines":10})["structuredContent"]["stdout"],"reloaded server lost job output")
    check(tool(52,"stop",{"job_id":reload_job})["structuredContent"]["signal"]=="SIGTERM","reload test job stop failed")
    time.sleep(.3)
    started=tool(12,"start",{"command":"echo READY; while :; do sleep 10; done","cwd":"mymcp"})["structuredContent"]
    job_id=started["job_id"]
    job_dir=os.path.join("/home/tools/mcp/work/jobs",job_id)
    time.sleep(.3)
    old_job_time=time.time()-8*86400
    for name in ("meta.json","stdout.log","stderr.log","exit_code"):
      path=os.path.join(job_dir,name)
      if os.path.exists(path):
        os.utime(path,(old_job_time,old_job_time))
    os.utime(job_dir,(old_job_time,old_job_time))
    tool(47,"jobs",{"limit":20})
    check(os.path.isdir(job_dir),"old running job was removed by retention")
    check(tool(13,"stop",{"job_id":job_id,"force":False})["structuredContent"]["signal"]=="SIGTERM","stop failed")
    time.sleep(.3)
    check(tool(14,"status",{"job_id":job_id})["structuredContent"]["state"]=="exited","stop status failed")
    for name in ("meta.json","stdout.log","stderr.log","exit_code"):
      path=os.path.join(job_dir,name)
      if os.path.exists(path):
        os.utime(path,(old_job_time,old_job_time))
    os.utime(job_dir,(old_job_time,old_job_time))
    tool(48,"jobs",{"limit":20})
    check(not os.path.exists(job_dir),"expired exited job was not removed by retention")
    check(tool(15,"read_file",{"path":"../server.py"})["isError"],"read path escape was accepted")
    check(tool(16,"write_file",{"path":"../escape.txt","content":"bad"})["isError"],"write path escape was accepted")
    jobs=tool(17,"jobs",{"limit":20})["structuredContent"]
    check(isinstance(jobs,list),"jobs failed")
    check(all(item.get("chat")==CHAT for item in jobs),"jobs returned another chat")
    no_chat=request(18,"tools/call","hello",{})["result"]
    check(no_chat["isError"],"missing chat was accepted")
    files=tool(19,"list_files",{"path":"mymcp/testdata","recursive":True})["structuredContent"]
    match=[item for item in files if item.get("path")=="mymcp/testdata/test.txt"]
    check(match and match[0].get("type")=="file" and match[0].get("size")==11,"list_files failed")
    binary=b"\x00\x01abc\xff\n"
    encoded=base64.b64encode(binary).decode("ascii")
    written=tool(20,"write_blob",{"path":"mymcp/testdata/test.txt","offset":0,"data_base64":encoded,"truncate":True})["structuredContent"]
    check(written["length"]==len(binary) and written["size"]==len(binary),"write_blob failed")
    blob=tool(21,"read_blob",{"path":"mymcp/testdata/test.txt","offset":2,"length":3})["structuredContent"]
    check(base64.b64decode(blob["data_base64"])==binary[2:5] and not blob["eof"],"read_blob chunk failed")
    blob=tool(22,"read_blob",{"path":"mymcp/testdata/test.txt","offset":5,"length":100})["structuredContent"]
    check(base64.b64decode(blob["data_base64"])==binary[5:] and blob["eof"],"read_blob eof failed")
    check(tool(23,"read_blob",{"path":"../server.py"})["isError"],"read_blob path escape was accepted")
    check(tool(24,"write_blob",{"path":"../escape.bin","data_base64":"AA==","truncate":True})["isError"],"write_blob path escape was accepted")
    check(not tool(25,"write_file",{"path":"mymcp/testdata/test.txt","content":"alpha\nbeta\n"})["isError"],"test file restore failed")
    check(AGENT_CONFIG is not None and AGENT_DIR is not None,"agent test paths are required")
    check(agent_request("wait",token="wrong-token").get("status")=="error","bad agent token was accepted")
    old=time.time()-172800
    stale_queue=os.path.join(AGENT_DIR,"queue","req_"+"1"*16+".json")
    stale_done=os.path.join(AGENT_DIR,"done","req_"+"2"*16+".json")
    old_running=os.path.join(AGENT_DIR,"running","req_"+"3"*16+".json")
    for state_dir in (os.path.dirname(stale_queue),os.path.dirname(stale_done),os.path.dirname(old_running)):
      os.makedirs(state_dir,exist_ok=True)
    with open(stale_queue,"w",encoding="utf-8") as f:
      json.dump({"expires_epoch":time.time()-1},f)
    with open(stale_done,"w",encoding="utf-8") as f:
      f.write("{}\n")
    with open(old_running,"w",encoding="utf-8") as f:
      f.write("{}\n")
    os.utime(stale_done,(old,old))
    os.utime(old_running,(old,old))
    recent=os.path.join(AGENT_DIR,"done","req_"+"4"*16+".json")
    with open(recent,"w",encoding="utf-8") as f:
      f.write("{}\n")
    worker=threading.Thread(target=fake_agent_once)
    worker.start()
    time.sleep(.1)
    agent_result=tool(26,"agent_call",{"agent":"test-agent","module":"test","action":"echo","payload":{"x":123},"timeout":5})
    worker.join(timeout=5)
    check(not worker.is_alive(),"fake agent did not finish")
    check(not agent_result["isError"],"agent_call returned error")
    agent_data=agent_result["structuredContent"]
    check(agent_data.get("status")=="ok" and agent_data.get("result",{}).get("echo",{}).get("x")==123,"agent_call result mismatch")
    consumed=os.path.join(AGENT_DIR,"done",agent_data["request_id"]+".json")
    check(not os.path.exists(stale_queue),"expired queued agent request was not cleaned")
    check(not os.path.exists(stale_done),"stale orphaned agent result was not cleaned")
    check(os.path.exists(old_running),"old running agent request was cleaned automatically")
    check(os.path.exists(recent),"recent orphaned agent result was cleaned too early")
    check(not os.path.exists(consumed),"consumed agent result was retained")
    os.unlink(old_running)
    os.unlink(recent)
    cli_worker=threading.Thread(target=fake_edistribuzione_once)
    cli_worker.start()
    time.sleep(.1)
    cli=subprocess.run(["./agent_call","edistribuzione","load_profile.month","2026","09"],
      capture_output=True,text=True,env=env,timeout=10)
    cli_worker.join(timeout=5)
    check(not cli_worker.is_alive(),"fake edistribuzione agent did not finish")
    check(cli.returncode==0,"agent_call CLI failed: "+cli.stderr)
    check(cli.stdout=="mymcp/tmpdata/TESTPOD_2026_09.csv\n","agent_call CLI output mismatch")
    cli_csv=os.path.join("tmpdata","TESTPOD_2026_09.csv")
    with open(cli_csv,"r",encoding="utf-8") as f:
      cli_csv_text=f.read()
    check(cli_csv_text=="date,time,value\n2026-09-01,00:00,1.5\n2026-09-01,23:45,2.5\n",
      "agent_call CLI CSV mismatch")
    os.unlink(cli_csv)
    client_env=os.environ.copy()
    client_env["MCP_AGENT_URL"]=URL
    client_env["MCP_AGENT_ID"]="test-agent"
    client_env["MCP_TOKEN"]="outer-test"
    client_env["MCP_AGENT_TOKEN"]="test-token"
    client_env["MCP_CHROME_URL"]="http://127.0.0.1:%d" % CHROME_PORT
    MockChromeHandler.requests=0
    client=subprocess.Popen(["./mcp_agent"],env=client_env,stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True)
    time.sleep(.2)
    client_result=tool(60,"agent_call",{"agent":"test-agent","module":"test","action":"echo","payload":{"client":321},"timeout":5})
    client_data=client_result["structuredContent"]
    check(not client_result["isError"] and client_data.get("result",{}).get("client")==321,"unified mcp_agent result mismatch")
    check(client.poll() is None,"unified mcp_agent exited after agent request")
    check(MockChromeHandler.requests>0,"integrated watcher did not scan Chrome")
    client.terminate()
    client_out,client_err=client.communicate(timeout=5)
    check(" WATCH " in client_out,"mcp_agent watcher runtime log missing")
    check(" AGENT received " in client_out,"mcp_agent agent request runtime log missing")
    check(" AGENT result delivered " in client_out,"mcp_agent result runtime log missing")

    check(subprocess.run(["./mcp_agent","invalid"],env=client_env,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL).returncode==2,
      "unexpected mcp_agent argument was accepted")

    bad_env=client_env.copy()
    bad_env["MCP_AGENT_URL"]="http://127.0.0.1:19998/mcp"
    bad_env["MCP_CHROME_URL"]="http://127.0.0.1:19997"
    survivor=subprocess.Popen(["./mcp_agent"],env=bad_env,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
    time.sleep(3)
    check(survivor.poll() is None,"unified mcp_agent exited during network outage")
    survivor.terminate()
    survivor.wait(timeout=5)

    check(tool(27,"agent_call",{"module":"missing","action":"echo","timeout":1})["isError"],"missing agent module was accepted")
    drive_stat=tool(28,"drive_stat",{"path":"docs/doc.docx"})["structuredContent"]
    check(drive_stat["version"]=="1" and drive_stat["size"]==6 and drive_stat["mode"]=="rw","drive_stat failed")
    drive_list=tool(29,"drive_list",{"path":"docs","recursive":False})["structuredContent"]
    check(any(item.get("path")=="docs/doc.docx" for item in drive_list),"drive_list failed")
    drive_blob=tool(30,"drive_read_blob",{"path":"docs/doc.docx","offset":2,"length":3})["structuredContent"]
    check(base64.b64decode(drive_blob["data_base64"])==b"cde" and not drive_blob["eof"],"drive_read_blob failed")
    replacement=b"XYZ123"
    drive_write=tool(31,"drive_write_blob",{"path":"docs/doc.docx","offset":0,"data_base64":base64.b64encode(replacement).decode("ascii"),"truncate":True,"commit":True,"expected_version":"1"})
    check(not drive_write["isError"],"drive_write_blob failed")
    drive_written=drive_write["structuredContent"]
    check(drive_written.get("committed") is True and drive_written.get("version")!="1","drive_write_blob commit metadata failed")
    new_version=drive_written["version"]
    drive_blob=tool(32,"drive_read_blob",{"path":"docs/doc.docx","offset":0,"length":100})["structuredContent"]
    check(base64.b64decode(drive_blob["data_base64"])==replacement and drive_blob["eof"],"drive_write_blob content mismatch")
    direct_get=tool(42,"drive_get_file",{"path":"docs/doc.docx","local_path":"drive/direct.docx"})
    check(not direct_get["isError"],"drive_get_file failed")
    direct_data=direct_get["structuredContent"]
    direct_path=os.path.join("/home/tools/mcp/work",CHAT,"drive","direct.docx")
    check(direct_data.get("local_path")=="drive/direct.docx" and direct_data.get("version")==new_version,"drive_get_file metadata failed")
    check(open(direct_path,"rb").read()==replacement,"drive_get_file content mismatch")
    check(tool(43,"drive_get_file",{"path":"docs/doc.docx","local_path":"../escape.docx"})["isError"],"drive_get_file local path escape was accepted")
    with open(direct_path,"wb") as f:
      f.write(b"DIRECT")
    direct_put=tool(44,"drive_put_file",{"local_path":"drive/direct.docx","path":"docs/doc.docx","expected_version":new_version})
    check(not direct_put["isError"],"drive_put_file failed")
    direct_written=direct_put["structuredContent"]
    check(direct_written.get("local_path")=="drive/direct.docx" and direct_written.get("committed") is True,"drive_put_file metadata failed")
    new_version=direct_written["version"]
    check(MockDriveHandler.files["file1"]["content"]==b"DIRECT","drive_put_file content mismatch")
    check(tool(45,"drive_put_file",{"local_path":"../escape.docx","path":"docs/doc.docx"})["isError"],"drive_put_file local path escape was accepted")
    check(tool(46,"drive_put_file",{"local_path":"drive/direct.docx","path":"docs/doc.docx","expected_version":"999"})["isError"],"drive_put_file version conflict was accepted")
    renamed=tool(33,"drive_rename",{"path":"docs/doc.docx","new_name":"renamed.docx","expected_version":new_version})
    check(not renamed["isError"] and renamed["structuredContent"]["path"]=="docs/renamed.docx","drive_rename failed")
    rename_version=renamed["structuredContent"]["version"]
    made=tool(34,"drive_mkdir",{"path":"docs/sub"})
    check(not made["isError"] and made["structuredContent"]["type"]=="dir","drive_mkdir failed")
    first=tool(35,"drive_write_blob",{"path":"docs/new.docx","offset":0,"data_base64":base64.b64encode(b"abc").decode("ascii"),"truncate":True,"commit":False})
    check(not first["isError"] and first["structuredContent"].get("staged") is True,"drive staged write start failed")
    second=tool(36,"drive_write_blob",{"path":"docs/new.docx","offset":3,"data_base64":base64.b64encode(b"def").decode("ascii"),"commit":True})
    check(not second["isError"] and second["structuredContent"].get("committed") is True,"drive staged write commit failed")
    drive_blob=tool(37,"drive_read_blob",{"path":"docs/new.docx","offset":0,"length":100})["structuredContent"]
    check(base64.b64decode(drive_blob["data_base64"])==b"abcdef","drive staged write content mismatch")
    check(tool(38,"drive_mkdir",{"path":"readonly/nope"})["isError"],"read-only Drive alias accepted a write")
    check(tool(39,"drive_delete",{"path":"docs/renamed.docx","expected_version":"999"})["isError"],"Drive version conflict was accepted")
    deleted=tool(40,"drive_delete",{"path":"docs/renamed.docx","expected_version":rename_version})
    check(not deleted["isError"] and deleted["structuredContent"].get("trashed") is True,"drive_delete failed")
    check(tool(41,"drive_stat",{"path":"docs/renamed.docx"})["isError"],"trashed Drive file remained visible")
    print("===== OK: MYMCP TESTS PASSED =====")
    return 0
  finally:
    if server is not None:
      server.terminate()
      try:
        server.wait(timeout=2)
      except subprocess.TimeoutExpired:
        server.kill()
        server.wait()
    if drive_server is not None:
      drive_server.shutdown()
      drive_server.server_close()
    if drive_thread is not None:
      drive_thread.join(timeout=2)
    if chrome_server is not None:
      chrome_server.shutdown()
      chrome_server.server_close()
    if chrome_thread is not None:
      chrome_thread.join(timeout=2)


if __name__=="__main__":
  sys.exit(main())
