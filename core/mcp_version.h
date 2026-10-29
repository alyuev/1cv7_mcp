// mcp_version.h - the only place with the 1cmcp version. Used by the core (MCP serverInfo/status),
// the component (Версия(), options page), the .rc (file version) and docs/make_als.py.
// The resource compiler can't build strings from numbers, so change all three lines together.
#ifndef MCP_VERSION_H
#define MCP_VERSION_H

#define MCP_VERSION "0.8.1"
#define MCP_VERSION_FULL "0.8.1.1"
#define MCP_VERSION_RC 0, 8, 1, 1

#endif
