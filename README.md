# mymcp

## Project status

Current development version: **1.10**

`mymcp` is a small MCP server written in C and designed to replace the previous Python MCP server that used the official Python MCP SDK.

The active production service is expected to run:

```text
/home/tools/mcp/mymcp
```

The development sources of truth are:

```text
/home/tools/mcp/work/mymcp/mymcp.c
/home/tools/mcp/work/mymcp/mcp_drive.c
/home/tools/mcp/work/mymcp/mcp_drive.h
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

### Temporary bulk data convention

A project that needs large amounts of temporary, generated or easily reproducible data must use:

```text
/home/tools/mcp/work/<project>/tmpdata/
```

`tmpdata/` is intentionally non-persistent and is excluded from normal backups. It may be deleted at any time without affecting the ability to preserve or resume the project.

Use `tmpdata/` only for items such as generated datasets, caches, temporary copies, intermediate computation output and other bulky data that can be recreated.

Never store in `tmpdata/` source code, configuration files, documentation, final results, irreplaceable measurements, required checkpoints or any other data needed to reproduce or resume the work.

Chats working on MCP projects should follow this convention whenever they create substantial temporary data.

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
- Never create or use hidden files or directories (a basename starting with `.`) for source, configuration, cache, state, temporary data or backups. Use explicit visible names instead; project temporary/generated data belongs under `tmpdata/`.
- Source header format is:

```c
// Gianluca Mazzini @2026- Version 1.16
```

The initial development year and major version are user-controlled. Version 1.16 is the current development version. Each functional change gets its own release number; unrelated functional changes are not grouped into one release.


## Dependencies

Build environment verified on Debian with GCC.

Main server dependencies:

```text
libc
libcjson.so.1
libcurl
```

`mcp_agent` also uses libcurl.

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

Expected build outputs:

```text
/home/tools/mcp/work/mymcp/mymcp
/home/tools/mcp/work/mymcp/mcp_agent
/home/tools/mcp/work/mymcp/agent_call -> mymcp
```

agent_call is a visible symlink to the same mymcp executable. The binary selects CLI mode from argv[0], so the command-line client and the MCP server share the same agent transport implementation.

The build should finish with zero compiler warnings before deployment.

### macOS client build

The macOS side needs three project files: `mcp_agent.c`, `gmcp.c` and `Makefile.mac`. `mcp_agent` is the unified always-running agent/watcher; `gmcp` remains the daily command-line client. The old standalone Chrome/check helper and watcher are no longer part of the macOS side.

On the Mac, from the local MCP directory:

```sh
cd /Users/gmazzini/mcp
scp -P 56789 root@s018.mazzini.org:/home/tools/mcp/work/mymcp/{mcp_agent.c,gmcp.c,Makefile.mac} .
make -f Makefile.mac
```

`Makefile.mac` discovers the Homebrew prefixes for `curl` and `cjson`, builds with the same strict C89 warning set used by the project, and produces:

```text
mcp_agent
gmcp
```

It assumes Homebrew `curl`, `cjson` and `pkg-config` are already installed. Normal background operation requires only `./mcp_agent`; `gmcp` remains available for the daily command-line operations. There are no separate watcher or Chrome-helper processes to compile or start.


## Tests

`test_mymcp.py` is the permanent regression test for the server and unified macOS client. It runs a separate local server on port 18080 and exercises discovery, all local file/blob tools, Google Drive tools through an isolated mock Drive API, synchronous and asynchronous jobs, ownership isolation, path protection, the generic agent transport, the local `agent_call` CLI, the unified `mcp_agent` running agent and watcher together through a mock Chrome endpoint, rejection of obsolete operating modes, and survival of temporary MCP/Chrome unavailability.

The agent/client part requires an isolated test configuration. From the project directory:

```sh
rm -rf tmpdata/regression
mkdir -p tmpdata/regression/agent
printf '%s\n' 'test-agent test-token test,edistribuzione' > tmpdata/regression/agent.conf
MYMCP_AGENT_CONFIG="$PWD/tmpdata/regression/agent.conf" \
MYMCP_AGENT_DIR="$PWD/tmpdata/regression/agent" \
python3 test_mymcp.py
rm -rf tmpdata/regression testdata
```

Run the regression test after changes to protocol, tools, jobs or agent transport.


## Deployment

The systemd service is:

```text
/etc/systemd/system/mcp.service
```

Its production command and reload action are:

```text
ExecStart=/home/tools/mcp/mymcp
ExecReload=/bin/kill -HUP $MAINPID
```

`KillMode=control-group` remains intentional. A normal `stop` or `restart` is the hard-cleanup path and terminates the server together with every asynchronous job still in the service cgroup. `reload` is the non-destructive update path: Version 1.15 handles `SIGHUP` in the main server process, closes the listening socket and `exec()`s the executable path that was resolved at startup. The main PID is preserved and existing job supervisors/process groups continue running.

The service runs as:

```text
User=mcp
Group=mcp
```

Do not deploy automatically unless explicitly requested by the user.

The user normally copies the tested binary into production.

Because an executable may already be running, prefer atomic replacement. The first activation of Version 1.15 from an older server requires one final normal restart, because older binaries do not handle `SIGHUP`. Install the binary, add the `ExecReload` line above to the unit, reload the systemd unit definition and restart once:

```sh
sudo install -m 755 /home/tools/mcp/work/mymcp/mymcp /home/tools/mcp/mymcp.new
sudo mv /home/tools/mcp/mymcp.new /home/tools/mcp/mymcp
sudo systemctl daemon-reload
sudo systemctl restart mcp.service
```

For later releases that preserve the Version 1.15 reload contract, use atomic replacement followed by reload instead:

```sh
sudo install -m 755 /home/tools/mcp/work/mymcp/mymcp /home/tools/mcp/mymcp.new
sudo mv /home/tools/mcp/mymcp.new /home/tools/mcp/mymcp
sudo ln -sf mymcp /home/tools/mcp/agent_call
sudo systemctl reload mcp.service
```

The agent_call symlink is only the local CLI entry point; it does not add another daemon or service.

A reload preserves running jobs. A restart or stop deliberately terminates them. A machine reboot necessarily terminates them as well; generic automatic replay after boot is intentionally not implemented because arbitrary jobs may have external side effects.

Then verify:

```sh
sudo systemctl status mcp.service --no-pager
```

The installed binary can be checked for the expected version, for example:

```sh
strings /home/tools/mcp/mymcp | grep '^1\.09$'
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

The development server exposes 22 tools:

```text
hello
write_file
read_file
list_files
read_blob
write_blob
drive_list
drive_stat
drive_read_blob
drive_get_file
drive_put_file
drive_write_blob
drive_mkdir
drive_rename
drive_delete
run
start
status
tail
stop
jobs
agent_call
```

A more user-oriented command manual is stored at:

```text
/home/tools/mcp/work/GMmcp2.txt
```

That manual is maintained with the current runtime behavior and should be kept synchronized whenever tools or logging change.


## Mandatory `chat` parameter

Every tool call requires a `chat` argument to identify which conversation is using the server and to keep logs and job ownership understandable across clients or chats.

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


### list_files

Lists files and directories below a path inside the work root, optionally recursively. Paths outside the work root are rejected.


### read_blob

Reads a binary-safe file chunk below the work root and returns it as base64 together with offset/length metadata.


### write_blob

Writes a base64-supplied binary chunk below the work root, with explicit offset and optional truncation. Path protection is the same as for the text file tools.


### Google Drive tools

Google Drive is accessed directly through the Google Drive API. It is not mounted as a filesystem.

Authorized roots are configured outside the MCP work tree in:

```text
/home/tools/mcp/drive.map
```

The map is read and fully validated on every Drive tool call, so authorization changes take effect without restarting `mymcp`. Every non-comment line has exactly three fields:

```text
<alias> <google-folder-id> <ro|rw>
```

Example:

```text
garr 1AbCdEf... rw
ari  1XyZ...    rw
archive 1Qwerty... ro
```

Only paths below the configured folder IDs can be reached through Drive tools. The alias is the first path component, for example `garr/NIS2/relazione.docx`. Duplicate names inside one Drive folder make a path ambiguous and are rejected rather than guessed.

Google access tokens are obtained on demand from the central authentication service:

```text
POST https://google.mazzini.org/googleauth
Content-Type: application/x-www-form-urlencoded

action=token&channel=mymcp&key=<API_KEY>
```

The HTTP 200 response body is the Google access token in `text/plain`; `mymcp` uses it only to build the existing `Authorization: Bearer <token>` header for Google Drive requests. OAuth login, refresh tokens and access-token renewal remain entirely the responsibility of `googleauth`. `mymcp` never reads `/home/www/data/google_access_token`.

The channel API key is runtime-only configuration in:

```text
/home/tools/mcp/mymcp.conf
```

with:

```text
googleauth_key=<API_KEY>
```

The file should be readable by the `mcp` service account and not by unrelated users; the recommended ownership/mode is `root:mcp` and `0640`. The API key and returned access token are kept in memory only and are never returned to MCP clients. The `mymcp` channel name is fixed and non-secret.

Binary write sessions use private staging outside the normal work tree:

```text
/home/tools/mcp/drive-stage
```

Production setup must create that directory as `mcp:mcp` mode `0700`. The staging area is not exposed by the normal work-directory tools.

`drive_list(path,recursive=false)` lists an authorized folder. `drive_stat(path)` returns item metadata including the Drive `version`, which can be used for optimistic concurrency checks. `drive_read_blob(path,offset,length)` reads ordinary binary Drive files in base64 chunks. Google-native Docs/Sheets/Slides are deliberately not exported by this interface; the current target is ordinary files such as `.pptx`, `.docx` and `.pdf` stored in Drive.

`drive_get_file(path,local_path)` downloads an ordinary Drive file directly into `/home/tools/mcp/work/<chat>/<local_path>` without returning its bytes or base64 through the MCP client. The destination is restricted to the calling chat workspace; traversal and symlink escapes are rejected. Download uses a temporary file in the destination directory followed by an atomic rename, so an interrupted transfer does not leave a partial target. The result includes Drive metadata plus `id` and `local_path`.

`drive_put_file(local_path,path,expected_version?)` uploads a regular file directly from `/home/tools/mcp/work/<chat>/<local_path>` to an authorized `rw` Drive destination without sending file bytes or base64 through the MCP client. The local source is restricted to the calling chat workspace. Existing Drive files retain the version check semantics used by the Drive write backend; `expected_version` can reject a stale replacement. The result includes Drive metadata plus `id`, `local_path` and `committed=true`.

`drive_write_blob` replaces or creates an ordinary Drive file using chunks of at most 1 MiB. A write session starts with `truncate=true` and `offset=0`. For a one-chunk file, use `commit=true`. For a multi-chunk file, use `commit=false` for every non-final chunk and `commit=true` on the final chunk. The server captures the target Drive version when staging begins and checks it again before commit. If another writer changed or created the target in the meantime, commit fails and leaves the staged data available for a deliberate retry instead of overwriting the newer Drive file. Optional `expected_version` can enforce the version already observed by the caller before staging begins.

`drive_mkdir(path)` creates a folder under an `rw` alias. `drive_rename(path,new_name,expected_version?)` changes only the item's name, not its parent. `drive_delete(path,expected_version?)` moves the item to Google Drive trash; it does not permanently delete it. Rename and delete of a configured root alias are prohibited. All modifying operations are rejected on `ro` aliases.

For tests, the Drive map, googleauth configuration path, googleauth endpoint, staging directory and Drive API endpoints can be overridden with `MYMCP_DRIVE_MAP`, `MYMCP_GOOGLEAUTH_CONFIG`, `MYMCP_GOOGLEAUTH_URL`, `MYMCP_GOOGLEAUTH_CHANNEL`, `MYMCP_DRIVE_STAGE`, `MYMCP_DRIVE_API` and `MYMCP_DRIVE_UPLOAD_API`. The regression suite uses these overrides only against its local mock servers.


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

`meta.json` contains the owning chat together with process and command metadata. Do not assume that job PID and job ID are interchangeable.

Version 1.14 treats job storage as transient runtime state rather than a permanent archive. Running jobs are never removed by age. A job that is no longer running is removed, together with its metadata and stdout/stderr files, after 7 days without job-file activity. The cleanup also covers old orphaned jobs that have no `exit_code`; before removal the stored leader PID/start-time and process group are checked so a genuinely active job is preserved. Cleanup runs opportunistically when jobs are started or listed.


## Client identity

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


## gmcp client

Current `gmcp` version: **1.02**.

`gmcp` is the command-line client for the MCP service. Its `github` command reads `/home/tools/mcp/work/github.map` and treats that mapping as the complete authoritative file list for every repository named in the map.

For each mapped repository, `gmcp github` clones the current repository into its disposable local clone, compares the tracked Git paths with the destinations listed in `github.map`, stages deletion of every tracked GitHub file that is not mapped, then creates or updates the mapped files from MCP. One commit and push is produced only when the repository has changes. Deleted paths are printed as `DELETE owner/repo/path`.

The deletion step operates only inside the temporary Git clone and therefore only affects the Git repository after the resulting commit is pushed. It never removes a source file from `/home/tools/mcp/work`, never removes `github.map`, and never removes runtime data from the MCP server. Only paths returned by `git ls-files` are candidates for GitHub deletion; `.git` metadata and unrelated local files are not candidates.

Consequently, any file that must remain in a mapped GitHub repository, including documentation, examples or repository metadata files, must itself appear as a destination in `github.map`.


## Remote agents

`mymcp` provides a generic request/response transport to remote local agents. The current Mac agent can use a dedicated Chrome instance through CDP on `127.0.0.1:9222`, while the transport itself remains independent of browser-specific modules.

The public HTTP endpoint remains exactly one:

```text
/mcp
```

No additional endpoint and no Apache configuration change are required. Normal ChatGPT/MCP traffic follows the existing MCP protocol. Agent traffic is recognized before MCP parsing by the dedicated HTTP headers `X-MCP-Agent`, `X-MCP-Agent-Action` and `X-MCP-Agent-Token`.

The server side exposed to MCP is the general tool:

```text
agent_call(chat, agent?, module, action, payload?, timeout?)
```

Version 1.16 also provides the local CLI command:

```sh
agent_call edistribuzione load_profile.month YYYY MM [MAGNITUDE]
```

For example:

```sh
agent_call edistribuzione load_profile.month 2026 09
```

The year must contain four digits and the month exactly two digits from 01 to 12. The optional magnitude accepts A+, A-, RI+, RC+, RI- or RC-; when omitted the agent default A+ is used. The CLI uses the same queue, timeout handling and result processing as the MCP tool. On success it writes the normal CSV under mymcp/tmpdata and prints only that relative CSV path. Failures return a non-zero exit status. It is intended to run locally as root or mcp because the agent spool is private to the mcp service account.

`module` and `action` are small validated names. `payload` and the returned `result` are arbitrary JSON, so adding a new local capability does not require changing the transport protocol. The optional `agent` selects a specific machine; when omitted, any configured agent providing that module may claim the request. `mymcp` transports requests and results but does not know how a module is implemented. In particular, it does not receive browser cookies or authentication state from a local browser.

The local agent uses long polling. It repeatedly sends an authenticated `wait` request to `/mcp`; `mymcp` holds the connection for up to 25 seconds and returns immediately when suitable work appears. After executing the request, the agent sends a `result` request to the same `/mcp` endpoint. The agent therefore requires no inbound port and works behind NAT or firewalls as long as it can reach the existing HTTPS MCP service.

Agent configuration is read dynamically from:

```text
/home/tools/mcp/agent.conf
```

Each non-comment line has three whitespace-separated fields:

```text
<agent-id> <token> <module1,module2,...>
```

For example:

```text
mac1 <token> test,browser,qrz,edistribuzione
```

The configuration file is read when an agent authenticates and when `agent_call` checks module availability. Adding an agent, changing its token or changing its module list therefore does **not** require restarting `mymcp`. Tokens are never returned to MCP clients. The existing HTTPS/Apache protection remains the outer transport protection; the per-agent token is an additional application-level identity check.

Runtime requests use a filesystem spool, preserving the existing fork-per-connection architecture without threads, shared memory, databases, Redis or WebSockets:

```text
/home/tools/mcp/agent/queue/
/home/tools/mcp/agent/running/
/home/tools/mcp/agent/done/
```

A queued request is claimed with an atomic `rename()` into `running`. Results are written into `done`. Request IDs use the `req_<hex>` form. A request already assigned to an agent is not automatically reassigned on timeout, because actions may have side effects and automatic replay could execute them twice. If a request times out before any agent claims it, it is cancelled from the queue.

Version 1.13 keeps this spool transient without imposing an arbitrary execution limit on an agent. Each queued request stores an `expires_epoch` derived from that specific `agent_call` timeout and cannot be claimed after it expires. A request already claimed into `running` is never automatically replayed or time-expired, because the server cannot know whether an external side effect is still in progress. When `agent_call` receives a completed result, its `done` file is consumed and removed immediately. Orphaned `done` files, for example results that arrived after the caller timed out, are removed opportunistically after 1 hour.

The agent runtime directory is operational state and is not intended for backup or historical retention.

### mcp_agent client

Production `mcp_agent` is Version **1.33**; development Version **1.37** is the unified macOS client.

`mcp_agent` is the single macOS client. Version 1.37 combines the remote-agent transport and MCP job watcher in one executable and uses the same Chrome CDP endpoint on `127.0.0.1:9222`. The client provides `test/echo`, `browser/read_tab`, `edistribuzione/load_profile.month` and `qrz/webcontact.add`, and also watches ChatGPT conversations for completed MCP jobs.

`edistribuzione/load_profile.month` retrieves one complete monthly quarter-hour load profile from an already authenticated E-Distribuzione PortaleClienti Chrome session. Payload: `year`, `month`, and optional `magnitude`; the default magnitude is `A+`. Supported magnitudes are `A+`, `A-`, `RI+`, `RC+`, `RI-` and `RC-`. The agent automatically opens the `Curve di carico` page when the authenticated browser is elsewhere in PortaleClienti, waits for the page controls, resolves the actual month/year option values exposed by the current portal UI (including zero-padded month values), synchronizes the four Aura date controls, activates `Modifica periodo` once, observes the Aura `QueryLoadProfile`/continuation response through CDP, and returns the normalized `MappaDailyLoadProfile` as days containing quarter-hour samples. The result also includes the POD read from the already loaded Aura component. A normal 31-day month therefore contains 2976 samples. When `mymcp` receives a successful result for this action, it writes the normalized samples to `mymcp/tmpdata/<POD>_YYYY_MM.csv` with columns `date,time,value`; the first sample is `00:00` and the 96th is `23:45`. The returned result includes `csv_file` when the file is written successfully. No automatic action retry is performed. If the authenticated portal context is unavailable or expired, the action fails instead of replaying the request. Result delivery itself is retried after transient network/server failures because retransmitting a completed result does not repeat the external action. Browser cookies and authentication material remain local to Chrome/the Mac and are never returned to the server.

`qrz/webcontact.add` implements the authenticated QRZ operation required by qrzweb. Its payload is `callsign` plus `mycall`. The operation runs entirely inside an already authenticated QRZ Chrome context, performs an initial Web Contacts presence check, submits the QRZ add action only when needed, then performs a fresh final verification. It returns structured codes including `ALREADY_PRESENT`, `ADDED`, `NO_BROWSER`, `NO_QRZ_CONTEXT`, `NOT_AUTHENTICATED`, `PROFILE_UNAVAILABLE`, `WEB_CONTACTS_UNAVAILABLE`, `ACTION_FAILED`, `NOT_CONFIRMED`, `TEMPORARY_ERROR` and `INVALID_REQUEST`. Cookies, QRZ session identifiers, hidden form values and authentication material remain local to Chrome/the Mac and are never returned to the server. Because `mcp_agent` executes requests serially, authenticated QRZ actions are serialized by design.

The QRZ operation has been validated end-to-end against a real authenticated QRZ session for the three essential cases: an existing relationship returns `ALREADY_PRESENT` without submitting an add action; a profile with historical Web Contacts but no usable authenticated action returns `WEB_CONTACTS_UNAVAILABLE`; and a real new relationship returns `ADDED` only after a fresh positive verification. Repeating the successful add returns `ALREADY_PRESENT`, confirming real idempotency after a completed action.

By default it uses `https://www.mazzini.org/mcp`, agent ID `mac1`, `~/mcp/token.txt` for the existing outer MCP/Apache authorization and `~/mcp/agent.token` for the per-agent authentication. The endpoint, agent ID and both tokens may also be supplied through environment variables. `MCP_CHROME_URL` can override the default Chrome CDP base URL for diagnostics/tests; normal macOS operation uses `http://127.0.0.1:9222`.

`mcp_agent` has one operating mode only: started without arguments, it continuously runs both the remote agent and the job watcher. `-h` and `--help` only print usage information.

Version 1.37 keeps the foreground process visibly alive through timestamped runtime output on stdout/stderr only; no persistent request log is created. Startup is reported immediately. Each successful agent long-poll with no work prints an `AGENT idle` heartbeat. Agent requests print arrival time, request ID, module/action, worker start, result delivery, completion and worker exit; network/result retries are reported as `ERROR`. Every watcher cycle prints a `WATCH scan` heartbeat, each selected chat, a chat job summary, every running job with elapsed time and command when available, each newly completed job with exit code, and successful or failed `check` delivery.

Version 1.37 also restores the original watcher safety rule: the Chrome target is rediscovered immediately before sending `check`. Its CDP reply handling accepts commands where the caller intentionally does not retain the JSON reply; this prevents the null-reply dereference that could terminate the unified client when a job completion triggered `Input.insertText` or `Input.dispatchKeyEvent`. The permanent regression suite executes a complete mocked Chrome WebSocket/CDP `check` sequence.

The current Version 1.16 development server passes the full regression suite on a separate local port, including the end-to-end fake-agent exchange (`agent_call -> wait -> result`), the local E-Distribuzione CLI path through the same spool, wrong agent authentication, unavailable modules, per-request queue expiry, immediate consumption of delivered results, preservation of already claimed `running` requests, seven-day cleanup of completed/orphaned jobs while preserving active jobs, and non-destructive `SIGHUP` reload while an asynchronous job remains running.


### Integrated watcher

Normal mcp_agent mode runs both functions continuously: remote-agent long polling and MCP job watching. Only one program has to be started on the Mac.

The client reads the MCP token and agent token once at startup. The parent keeps reusable libcurl handles for the agent long-poll connection and MCP watcher queries, allowing HTTP/TLS connections to be reused across cycles. Chrome discovery uses the same 127.0.0.1:9222 instance used by the action modules.

When no agent action is running there is one process. When a request arrives, mcp_agent forks one temporary worker to execute and return that request while the parent continues the watcher. The parent does not accept another agent action until the worker finishes, so external actions remain serialized. The worker exits when the request is complete.

Transient network loss, Wi-Fi changes, Mac sleep/wake, temporary MCP unavailability and stale HTTP connections are non-fatal. Long polling reconnects automatically. A completed action whose result cannot temporarily be delivered keeps retrying only the result transmission with bounded backoff; the action itself is never replayed automatically. Chrome WebSocket connections are opened when needed and rediscovered on later operations rather than assumed to survive sleep/wake.

The watcher scans every 60 seconds. Browser tab titles must be either:

~~~text
<chat>
<chat> <number>
~~~

The first word is the MCP chat name and the optional second word is a non-negative decimal conversation number. If several tabs have the same chat name, only the highest-numbered tab is monitored. An unnumbered tab has number 0. Two different tabs with the same highest number are ambiguous, so monitoring for that chat is suspended.

Watcher state is kept only in memory. On the first observation of a conversation, already exited jobs are treated as historical and running jobs form the baseline. A later transition from running to exited, or a newly observed completion between scans, causes one check to be sent through that conversation's current Chrome CDP target. The state is then marked notified so the same completion is not sent twice.

If the selected conversation changes or disappears, its old in-memory state is discarded. Work completed while a conversation was not being watched is therefore not reported retroactively. Long-running work must use MCP start because only MCP jobs have the job_id required for tracking.

There are no alternate watcher-only or agent-only operating modes. The executable always runs both functions together.

There is intentionally no reboot/login autostart configuration in this project. The user starts mcp_agent manually after a reboot. Once started, it is expected to remain alive across normal network interruptions and Mac sleep/wake until explicitly terminated or the machine is rebooted.


## Agent strategy published by server/discover

MCP discovery publishes concise operational guidance so a new chat can work safely without relying on previous conversation history. Normal strategy for long-running work:

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

Google Drive tools are a separate backend. They can reach only the folder roots named in `/home/tools/mcp/drive.map`; that map is outside `work` and cannot be changed through the normal file tools. The private Drive staging directory is also outside `work`.

`run` and `start` restrict `cwd` to the work root, but commands execute with normal Linux permissions of the `mcp` account.

Therefore system/user file protection should also rely on standard Unix ownership and mode bits where appropriate.

A stricter sandbox for `run/start` was discussed but intentionally not implemented yet.


## Known operational behavior

- ChatGPT sessions may cache the old tool schema.
- A server restart alone may not make a current chat rediscover new tools immediately.
- Refresh can help, but sometimes opening a new chat is required.
- Always distinguish client schema-cache problems from server runtime problems.
- The server can be queried directly over localhost to verify `server/discover` and `tools/list` independently of the ChatGPT session.


## Current production state

At the time this document was written:

- The C implementation is stable and in production.
- Production `mymcp` is Version 1.15. Development Version 1.16 adds the local `agent_call` CLI entry point while preserving the existing MCP agent transport and non-destructive `SIGHUP` reload contract. Production `mcp_agent` on the remote Mac is Version 1.33; development Version 1.37 unifies the remote agent and job watcher into the single `mcp_agent` executable, keeps agent actions serialized through temporary workers, reuses parent MCP connections, and automatically recovers from transient network loss and sleep/wake. The E-Distribuzione result includes the POD, and `mymcp` stores successful monthly profiles in `mymcp/tmpdata/<POD>_YYYY_MM.csv`.
- The production service executes `/home/tools/mcp/mymcp`.
- The Python MCP virtualenv, Python SDK checkout, old `server.py` and related MCP Python runtime files were removed.
- The C binary uses cJSON, libcurl and libc.
- Production Version 1.15 and development Version 1.16 expose 22 MCP tools, including nine native Drive tools and `agent_call`; `drive_get_file` and `drive_put_file` transfer ordinary files directly between Drive and the calling chat workspace without base64 through the client.
- `chat` is mandatory.
- Job ownership by chat works.
- The development binary/source remain under `/home/tools/mcp/work/mymcp`.
- The command manual is `/home/tools/mcp/work/GMmcp2.txt`.


## Rules for a future assistant/chat continuing development

1. Treat `/home/tools/mcp/work/mymcp/mymcp.c` together with `mcp_drive.c` / `mcp_drive.h` as the development source of truth for the server and Drive backend.
2. Read the actual current source before changing behavior.
3. Keep all mymcp development files inside `/home/tools/mcp/work/mymcp`.
4. Keep unrelated projects in their own subdirectories under `/home/tools/mcp/work`.
5. Follow the C coding style documented above.
6. Increment the source version for every functional change and keep one functional modification per release.
7. Build with zero warnings before testing.
8. Run the regression test after protocol/tool/job changes.
9. Test new server versions on a separate port before production deployment when practical.
10. Do not overwrite `/home/tools/mcp/mymcp` or restart `mcp.service` unless the user asks for deployment.
11. Prefer atomic installation with `.new` + `mv` for a running executable.
12. Keep `GMmcp2.txt` synchronized when tool parameters or behavior change.
13. Preserve the mandatory `chat` semantics unless the user explicitly changes the design.
14. Preserve job ownership isolation between chats.
15. Do not reintroduce persistent request logging unless the user explicitly asks for it.
16. Remember that `run/start` are not currently filesystem-sandboxed beyond Unix permissions and constrained working directory.
17. Do not remove or alter unrelated project files under `work/`.
18. If ChatGPT shows stale tools, verify the server directly before modifying code.
19. For this conversation/project, use `chat="mymcp"` when calling tools.
20. Never create or use hidden files or directories (names beginning with `.`); use visible names instead, including for caches, state, temporary files and backups.
