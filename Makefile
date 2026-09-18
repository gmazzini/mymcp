CC=gcc
CFLAGS=-std=gnu89 -O2 -Wall -Wextra -Wshadow -Wstrict-prototypes -Wmissing-prototypes -Wconversion -Wno-sign-conversion
LDLIBS=-lcjson -lcurl
AGENT_LDLIBS=-lcjson -lcurl

all: mymcp mcp_agent agent_call

mymcp: mymcp.c mcp_drive.c mcp_drive.h
	$(CC) $(CFLAGS) mymcp.c mcp_drive.c $(LDLIBS) -o mymcp

mcp_agent: mcp_agent.c
	$(CC) $(CFLAGS) mcp_agent.c $(AGENT_LDLIBS) -o mcp_agent

agent_call: mymcp
	ln -sf mymcp agent_call

clean:
	rm -f mymcp mcp_agent agent_call mymcp_test.log mymcp_test.pid
