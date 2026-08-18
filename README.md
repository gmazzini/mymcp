# mymcp

## Project status

Current version: **1.04**

`mymcp` is a small MCP server written in C and designed to replace the previous Python MCP server that used the official Python MCP SDK.

The active production service is expected to run:

```text
/home/tools/mcp/mymcp
```

The development source of truth is:

```text
/home/tools/mcp/work/mymcp/mymcp.c
```

The project directory is:

```text
/home/tools/mcp/work/mymcp/
```

The MCP work root exposed to tools is:

```text
/home/tools/mcp/work
```

Do not scatter project files directly in `/home/tools/mcp/work`. Create a subdirectory for each project.

The old Python MCP runtime was removed after the C implementation became stable. Python scripts that belong to individual projects under `work/` are unrelated and may still exist.


## Why this project exists

The original MCP server was written in Python using the MCP Python SDK. It provided basic file access and synchronous shell execution.

It was extended with asynchronous process management, then reimplemented in C for a smaller, simpler and more self-contained runtime.

The C server currently provides the same operational functions plus persistent job management and request logging.

The design goal is a small server that performs control-plane work only. Heavy computation is executed by external programs, often written in C.


## Language and coding style

The source follows the user's C style rules.

- C89-style code.
- Build uses `-std=gnu89` because `//` comments are required.
- Variables are declared at the beginning of functions or blocks.
- Variables of the same type are grouped where readable and semantically appropriate.
- Variables are initialized after declaration.
- Prefer `for` over `while` where practical.
- Comments are English only and use `//`.
- Prefer standard library/system functions over unnecessary custom replacements.
- Optimize for performance and avoid unnecessary allocations or variables.
- Do not leave dead code or unused variables.
- Opening braces are on the same line as the statement, with one space before `{`.
- Source header format is:

```c
// Gianluca Mazzini @2026- Version 1.04
```

The initial development year and major version are user-controlled. Version 1.04 is the current implemented and tested development version.


## Dependencies

Build environment verified on Debian with GCC.

Main dependencies:

```text
libc
libcjson.so.1
```

The system already provides cJSON headers at:

```text
/usr/include/cjson/cJSON.h
```

No Python runtime is needed by the production MCP service.


## Build

From:

```text
/home/tools/mcp/work/mymcp
```

run:

```sh
make clean
make
```

The Makefile currently builds with strict warnings and optimization.

Expected output binary:

```text
/home/tools/mcp/work/mymcp/mymcp
```

The build should finish with zero compiler warnings before deployment.


## Tests

A regression test exists at:

```text
/home/tools/mcp/work/mymcp/test_mymcp.py
```

It is a development/test helper only. It is not part of the production MCP runtime.

Run it from the project directory:

```sh
python3 test_mymcp.py
```

The test exercises discovery, tool listing, synchronous commands, asynchronous jobs, tail/status/stop, path protection and job listing.

When changing MCP schemas, job semantics or logging, update and rerun this test.


## Deployment

The systemd service is:

```text
/etc/systemd/system/mcp.service
```

Its production command is:

```text
ExecStart=/home/tools/mcp/mymcp
```

The service runs as:

```text
User=mcp
Group=mcp
```

Do not deploy automatically unless explicitly requested by the user.

The user normally copies the tested binary into production.

Because an executable may already be running, prefer atomic replacement:

```sh
sudo install -m 755 /home/tools/mcp/work/mymcp/mymcp /home/tools/mcp/mymcp.new
sudo mv /home/tools/mcp/mymcp.new /home/tools/mcp/mymcp
sudo systemctl restart mcp.service
```

Then verify:

```sh
sudo systemctl status mcp.service --no-pager
```

The installed binary can be checked for the expected version, for example:

```sh
strings /home/tools/mcp/mymcp | grep '^1\.01$'
```

The user may need to refresh the ChatGPT MCP/plugin interface after tool schemas change. Existing chats sometimes retain a stale tool schema even after refresh; a new chat may be required. This is a client/session cache issue, not necessarily a server issue.


## MCP protocol

The server implements the MCP protocol version:

```text
2026-07-28
```

Transport:

```text
Streamable HTTP
```

Default endpoint:

```text
http://127.0.0.1:8000/mcp
```

The implementation was tested against the MCP JSON schema present in the former Python SDK checkout and against the working Python server's wire behavior.

The server handles at least:

```text
server/discover
tools/list
tools/call
ping
```

It validates request metadata and relevant MCP headers.

The response form mirrors the prior working server and includes both text `content` and `structuredContent` for tool results.


## Available tools

The server exposes 9 tools:

```text
hello
write_file
read_file
run
start
status
tail
stop
jobs
```

A more user-oriented command manual is stored at:

```text
/home/tools/mcp/work/GMmcp2.txt
```

That manual is maintained with the current runtime behavior and should be kept synchronized whenever tools or logging change.


## Mandatory `chat` parameter

Version 1.01 introduced a mandatory `chat` argument for every tool call.

The purpose is to identify which conversation is using the MCP server and make logs/job ownership understandable across multiple clients or chats.

Rules:

```text
length: 1-64 characters
allowed: A-Z a-z 0-9 _ - .
```

Examples:

```text
mymcp
chess
dts2-step185
chess-chatgpt
```

If the chat name is not already known, the assistant should ask the user to choose one before the first MCP tool call, then reuse the same value throughout that conversation.

For the chat in which this project was developed, the agreed value is:

```text
chat=mymcp
```

Conceptual examples:

```text
hello(chat="mymcp")
read_file(chat="mymcp",path="mymcp/README.md")
run(chat="mymcp",command="make",cwd="mymcp")
```

JSON object property order is not semantically significant, but `chat` is intentionally published as the first property in the tool schema for clarity.


## Tool behavior

### hello

Connectivity test.

Conceptual call:

```text
hello(chat="mymcp")
```

Expected response contains:

```text
hello from mymcp
```


### write_file

Writes/replaces a text file under `/home/tools/mcp/work`.

Conceptual call:

```text
write_file(chat="mymcp",path="project/file.txt",content="text")
```

The implementation rejects path traversal/outside-work paths and rejects unsafe symlink traversal cases.

Parent project directories may be created as needed.


### read_file

Reads a regular text file under `/home/tools/mcp/work`.

Conceptual call:

```text
read_file(chat="mymcp",path="project/file.txt")
```

Paths outside the work root are rejected.


### run

Runs a shell command synchronously as the Unix user `mcp`.

Conceptual call:

```text
run(chat="mymcp",command="gcc prova.c -o prova",cwd="test")
```

Default `cwd` is `.` relative to `/home/tools/mcp/work`.

Current timeout:

```text
60 seconds
```

Returns exit code, stdout and stderr.

The command itself is executed through `/bin/sh -c` and therefore has the normal filesystem/process permissions of Unix user `mcp`.

Important security note: only the working directory is constrained to the MCP work tree. The shell command is not a filesystem sandbox. It may access anything the Unix account `mcp` is permitted to access by normal Linux permissions.

A possible future Landlock/container-style confinement was discussed but is NOT implemented.


### start

Starts a long-running shell command asynchronously.

Conceptual call:

```text
start(chat="mymcp",command="./program",cwd="project")
```

Returns a stable MCP job identifier such as:

```text
job_a1b2c3d4e5f6
```

The job identifier is independent of the operating-system PID because PIDs can be reused.

Each job runs in its own process group/session structure so it can be monitored and stopped as a unit.

stdout/stderr are stored persistently under the job directory.


### status

Returns the state and resource usage of a job owned by the same chat.

Conceptual call:

```text
status(chat="mymcp",job_id="job_a1b2c3d4e5f6")
```

Returned information includes items such as:

```text
job_id
chat
state
pid
pgid
active pids
elapsed time
exit code
CPU usage
RSS bytes
command
cwd
start time
stdout path
stderr path
```

A chat cannot use `status` on a job owned by another chat.


### tail

Reads the last lines of a job's output.

Conceptual call:

```text
tail(chat="mymcp",job_id="job_a1b2c3d4e5f6",lines=50,stream="stdout")
```

Allowed streams:

```text
stdout
stderr
both
```

Default lines:

```text
50
```

Maximum lines:

```text
1000
```

A chat cannot tail another chat's job.


### stop

Stops an asynchronous job owned by the same chat.

Normal operation:

```text
stop(chat="mymcp",job_id="job_a1b2c3d4e5f6",force=false)
```

This sends `SIGTERM` to the job process group.

Forced operation:

```text
stop(chat="mymcp",job_id="job_a1b2c3d4e5f6",force=true)
```

This sends `SIGKILL`.

The server verifies process metadata to reduce the risk of signaling an unrelated process after PID reuse.

A chat cannot stop another chat's job.


### jobs

Lists jobs owned by the requesting chat, newest first.

Conceptual call:

```text
jobs(chat="mymcp",limit=100)
```

Default limit:

```text
100
```

Maximum limit:

```text
1000
```

Old jobs created before chat ownership existed may internally be marked:

```text
chat=legacy
```


## Version 1.04 visible job storage

Version 1.04 changes persistent job storage from the hidden Unix directory:

```text
/home/tools/mcp/work/.jobs/
```

to the visible directory:

```text
/home/tools/mcp/work/jobs/
```

This is a visibility and usability change only. Job semantics are unchanged. Each `job_...` directory still contains `meta.json`, `stdout.log`, `stderr.log` and, after completion, `exit_code`. `meta.json` remains the authoritative mapping from a job ID to its owning chat, command, cwd, PID/PGID and timestamps.

Production migration is performed explicitly while no jobs are active by renaming `.jobs` to `jobs`.

## Job storage

Persistent asynchronous job state is stored under:

```text
/home/tools/mcp/work/jobs/
```

A typical job directory contains:

```text
jobs/job_a1b2c3d4e5f6/meta.json
jobs/job_a1b2c3d4e5f6/stdout.log
jobs/job_a1b2c3d4e5f6/stderr.log
jobs/job_a1b2c3d4e5f6/exit_code
```

`meta.json` contains the chat owner in version 1.01.

Do not assume that job PID and job ID are interchangeable.


## Request logging

Version 1.01 added MCP request logging. Version 1.02 turns it into a more useful audit trail by recording the operational arguments of tool calls while still avoiding file contents and command output.

Production log file:

```text
/home/tools/mcp/mcp.log
```

It must be writable by Unix user `mcp`.

Typical setup:

```sh
sudo touch /home/tools/mcp/mcp.log
sudo chown mcp:mcp /home/tools/mcp/mcp.log
sudo chmod 644 /home/tools/mcp/mcp.log
```

The server checks log availability at startup and is intended not to silently operate without its audit log.

Examples:

```text
2026-08-12T11:20:10+0200 chat=chess client=chatgpt method=tools/call name=read_file path="chess/dtschess.c" tool_error=false rc=200 elapsed_ms=1
2026-08-12T11:20:22+0200 chat=chess client=chatgpt method=tools/call name=write_file path="chess/step185_summary.txt" bytes=1842 tool_error=false rc=200 elapsed_ms=2
2026-08-12T11:20:40+0200 chat=chess client=chatgpt method=tools/call name=run cwd="chess" command="gcc dtschess.c -O2 -o dtschess" exit_code=0 tool_error=false rc=200 elapsed_ms=481
2026-08-12T11:21:03+0200 chat=chess client=chatgpt method=tools/call name=start cwd="chess" command="./dtschess --test step185" job_id=job_a1b2c3d4e5f6 tool_error=false rc=200 elapsed_ms=4
```

Tool-specific audit fields are:

```text
hello       tool_error
read_file   path, tool_error
write_file  path, bytes, tool_error
run         cwd, command, exit_code when available, tool_error
start       cwd, command, job_id when available, tool_error
status      job_id, tool_error
tail        job_id, stream, lines, tool_error
stop        job_id, force, tool_error
jobs        limit, tool_error
```

`tool_error=true` distinguishes an MCP tool-level failure from the HTTP result code. A tool can legitimately return HTTP `rc=200` while reporting a logical tool error.

Automatic MCP operations that do not belong to a specific tool chat use `chat=-` and do not have tool arguments.

The audit logger deliberately does **not** record:

- `write_file` content
- data returned by `read_file`
- stdout/stderr returned by `run`
- stdout/stderr data returned by `tail`
- complete JSON request/response payloads

For `run` and `start`, the shell command itself is intentionally logged because it is the most important information for auditing what an autonomous client asked the server to execute. Logged quoted values are normalized to one line and limited to 2048 characters each. Quotes and backslashes are escaped.

A command may itself contain a secret such as a token or password; if so, that secret can appear in the audit log. Avoid putting credentials directly on command lines. Automatic credential redaction is not implemented in Version 1.02.

Useful inspection:

```sh
tail -f /home/tools/mcp/mcp.log
```


## Client identity

The server can log MCP client information when supplied by the client in request metadata.

The `chat` field is our own required tool argument and is not the JSON-RPC request ID.

Do not rename it to `id`; JSON-RPC already has an `id` field with a different purpose.

There is no portable MCP standard field that reliably gives the human-visible ChatGPT conversation title/thread name, therefore the chat label is explicitly supplied by the caller.


## Typical synchronous workflow

For a small C program under a project directory:

```text
write_file(chat="project",path="project/prova.c",content=source)
run(chat="project",command="gcc prova.c -o prova",cwd="project")
run(chat="project",command="./prova",cwd="project")
```

Inspect stdout/stderr after each step and rewrite the file only if necessary.


## Typical asynchronous workflow

For a long calculation:

```text
start(chat="project",command="./long_job",cwd="project")
```

Store the returned `job_id`, then use:

```text
status(chat="project",job_id="job_...")
tail(chat="project",job_id="job_...",lines=50,stream="both")
```

Repeat as needed.

If cancellation is required:

```text
stop(chat="project",job_id="job_...",force=false)
```

Use `force=true` only when necessary.


## Agent strategy published by server/discover

Version 1.03 publishes concise operational guidance directly in MCP discovery instructions so a new chat can work safely without relying on previous conversation history.

Normal strategy for long-running work:

```text
start(...) -> status()/tail()/read_file() -> validate -> next action
```

A synchronous `run()` that contains `sleep`, delayed wait-then-observe commands, or polling loops is technically possible and can provide temporal continuity, but real use showed that this pattern may consume Work/Codex usage heavily and can exhaust the short usage window.

Therefore the discovery guidance is intentionally conservative:

- use `start()` for long-running work;
- use `status()`, `tail()` or `read_file()` for observations;
- do not use sleep-based/delayed polling through `run()` by default;
- use that technique only when the owner has explicitly authorized it for the current task.

The delayed-wait technique remains documented because it is a useful capability, but it is not the default agent strategy.


## Filesystem and Unix security model

The MCP service runs as Unix user/group `mcp:mcp`.

`read_file` and `write_file` explicitly restrict paths to `/home/tools/mcp/work`.

`run` and `start` restrict `cwd` to the work root, but commands execute with normal Linux permissions of the `mcp` account.

Therefore system/user file protection should also rely on standard Unix ownership and mode bits where appropriate.

A stricter sandbox for `run/start` was discussed but intentionally not implemented yet.


## Tool annotations and confirmation prompts

ChatGPT may ask the user to confirm some write/action calls, for example `write_file`.

A future version may add MCP Tool Annotations such as:

```text
readOnlyHint
destructiveHint
idempotentHint
openWorldHint
```

These annotations are NOT currently the main security boundary and do not automatically eliminate user confirmations.

Potential classification discussed for a future version:

```text
hello      read-only
read_file  read-only
status     read-only
tail       read-only
jobs       read-only
write_file write/destructive-capable
run        powerful shell action
start      powerful shell action
stop       process-control action
```

Do not claim this future annotation work is implemented unless the source has actually been changed and tested.


## Mobile ChatGPT note

Conversation history can continue across ChatGPT web/mobile for the same account, but custom MCP app availability may differ by client. If MCP tools are unavailable in a mobile session, continue conversational work there and resume MCP operations from a supported client.

This is a client capability issue, not a mymcp server failure.


## Known operational behavior

- ChatGPT sessions may cache the old tool schema.
- A server restart alone may not make a current chat rediscover new tools immediately.
- Refresh can help, but sometimes opening a new chat is required.
- Always distinguish client schema-cache problems from server runtime problems.
- The server can be queried directly over localhost to verify `server/discover` and `tools/list` independently of the ChatGPT session.


## Current production state

At the time this document was written:

- The C implementation is stable and in production.
- Development version 1.02 has been built and regression-tested; production remains whatever binary is currently installed until the user deploys it.
- The production service executes `/home/tools/mcp/mymcp`.
- The Python MCP virtualenv, Python SDK checkout, old `server.py` and related MCP Python runtime files were removed.
- The C binary uses cJSON and libc.
- All 9 tools work.
- `chat` is mandatory.
- Job ownership by chat works.
- Request logging is enabled at `/home/tools/mcp/mcp.log`; Version 1.02 adds operational audit fields without logging file contents or command output.
- The development binary/source remain under `/home/tools/mcp/work/mymcp`.
- The command manual is `/home/tools/mcp/work/GMmcp2.txt`.


## Rules for a future assistant/chat continuing development

1. Treat `/home/tools/mcp/work/mymcp/mymcp.c` as the development source of truth.
2. Read the actual current source before changing behavior.
3. Keep all mymcp development files inside `/home/tools/mcp/work/mymcp`.
4. Keep unrelated projects in their own subdirectories under `/home/tools/mcp/work`.
5. Follow the C coding style documented above.
6. Increment the source version when implementing a real released change; do not invent a version without considering the user's versioning rule.
7. Build with zero warnings before testing.
8. Run the regression test after protocol/tool/job changes.
9. Test new server versions on a separate port before production deployment when practical.
10. Do not overwrite `/home/tools/mcp/mymcp` or restart `mcp.service` unless the user asks for deployment.
11. Prefer atomic installation with `.new` + `mv` for a running executable.
12. Keep `GMmcp2.txt` synchronized when tool parameters or behavior change.
13. Preserve the mandatory `chat` semantics unless the user explicitly changes the design.
14. Preserve job ownership isolation between chats.
15. Preserve request logging unless the user explicitly changes it.
16. Preserve the Version 1.02 audit policy: log operational paths/parameters and the `run/start` command (up to 2048 characters), but never log file contents, stdout/stderr or complete JSON payloads by default.
17. Remember that `run/start` are not currently filesystem-sandboxed beyond Unix permissions and constrained working directory.
18. Do not remove or alter unrelated project files under `work/`.
19. If ChatGPT shows stale tools, verify the server directly before modifying code.
20. For this conversation/project, use `chat="mymcp"` when calling tools.


## Possible future work

These ideas were discussed but are not implemented unless the source says otherwise:

- MCP Tool Annotations to better describe read-only/destructive/idempotent/open-world behavior.
- Stronger confinement of `run` and `start` using Landlock or another sandbox mechanism.
- Additional log management/rotation if `mcp.log` grows significantly.
- Additional client interoperability testing with Gemini CLI/API or other MCP clients.

Do not implement these automatically. Re-evaluate requirements with the user first.
