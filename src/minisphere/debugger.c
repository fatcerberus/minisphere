/**
 *  miniSphere JavaScript game engine
 *  Copyright (c) 2015-2017, Fat Cerberus
 *  All rights reserved.
 *
 *  Redistribution and use in source and binary forms, with or without
 *  modification, are permitted provided that the following conditions are met:
 *
 *  * Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 *
 *  * Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 *
 *  * Neither the name of miniSphere nor the names of its contributors may be
 *    used to endorse or promote products derived from this software without
 *    specific prior written permission.
 *
 *  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 *  AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 *  IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 *  ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 *  LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 *  CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 *  SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 *  INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 *  CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 *  ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 *  POSSIBILITY OF SUCH DAMAGE.
**/

#include "minisphere.h"
#include "debugger.h"

#include "dmessage.h"
#include "jsal.h"
#include "sockets.h"

static int const TCP_DEBUG_PORT = 1208;

struct source
{
	char*      name;
	lstring_t* text;
};

static bool      do_attach_debugger (void);
static void      do_detach_debugger (bool is_shutdown);
static js_step_t on_breakpoint_hit  (void);

static bool       s_is_attached = false;
static color_t    s_banner_color;
static lstring_t* s_banner_text;
static bool       s_have_source_map = false;
static server_t*  s_server;
static socket_t*  s_socket = NULL;
static vector_t*  s_sources;
static bool       s_want_attach;

void
debugger_init(bool want_attach, bool allow_remote)
{
	void*         data;
	size_t        data_size;
	const path_t* game_root;
	const char*   hostname;

	jsal_on_breakpoint(on_breakpoint_hit);
	
	s_banner_text = lstr_new("debug");
	s_banner_color = color_new(192, 192, 192, 255);
	s_sources = vector_new(sizeof(struct source));

	// load the source map, if one is available
	s_have_source_map = false;
	jsal_push_hidden_stash();
	jsal_del_prop_string(-1, "debugMap");
	game_root = game_path(g_game);
	if (data = game_read_file(g_game, "sources.json", &data_size)) {
		jsal_push_lstring(data, data_size);
		jsal_parse(-1);
		jsal_put_prop_string(-2, "debugMap");
		free(data);
		s_have_source_map = true;
	}
	else if (!path_is_file(game_root)) {
		jsal_push_new_object();
		jsal_push_string(path_cstr(game_root));
		jsal_put_prop_string(-2, "origin");
		jsal_put_prop_string(-2, "debugMap");
	}
	jsal_pop(1);

	// listen for SSj connection on TCP port 1208. the listening socket will remain active
	// for the duration of the session, allowing a debugger to be attached at any time.
	console_log(1, "listening for SSj on TCP port %d", TCP_DEBUG_PORT);
	hostname = allow_remote ? NULL : "127.0.0.1";
	s_server = server_new(hostname, TCP_DEBUG_PORT, 1024, 1, true);

	// if the engine was started in debug mode, wait for a debugger to connect before
	// beginning execution.
	s_want_attach = want_attach;
	if (s_want_attach && !do_attach_debugger())
		sphere_exit(true);
}

void
debugger_uninit()
{
	struct source* source;

	iter_t iter;

	do_detach_debugger(true);
	server_unref(s_server);
	
	if (s_sources != NULL) {
		iter = vector_enum(s_sources);
		while (iter_next(&iter)) {
			source = iter.ptr;
			lstr_free(source->text);
			free(source->name);
		}
		vector_free(s_sources);
	}
}

void
debugger_update(void)
{
	socket_t* client;
	char*     handshake;

	if (client = server_accept(s_server)) {
		if (s_socket != NULL) {
			console_log(2, "rejected SSj connection from %s, already attached",
				socket_hostname(client));
			socket_unref(client);
		}
		else {
			console_log(0, "connected to SSj at %s", socket_hostname(client));
			handshake = strnewf("2 20000 v2.1.1 %s %s\n", SPHERE_ENGINE_NAME, SPHERE_VERSION);
			socket_write(client, handshake, strlen(handshake));
			free(handshake);
			s_socket = client;
			jsal_debug_attach();
			s_is_attached = true;
		}
	}
}

bool
debugger_attached(void)
{
	return s_is_attached;
}

color_t
debugger_color(void)
{
	return s_banner_color;
}

const char*
debugger_name(void)
{
	return lstr_cstr(s_banner_text);
}

const char*
debugger_compiled_name(const char* source_name)
{
	// perform a reverse lookup on the source map to find the compiled name
	// of an asset based on its name in the source tree.  this is needed to
	// support SSj source code download, since SSj only knows the source names.

	static char retval[SPHERE_PATH_MAX];

	int         num_files;
	const char* this_source;

	int i;

	strncpy(retval, source_name, SPHERE_PATH_MAX - 1);
	retval[SPHERE_PATH_MAX - 1] = '\0';
	if (!s_have_source_map)
		return retval;
	jsal_push_hidden_stash();
	jsal_get_prop_string(-1, "debugMap");
	if (jsal_get_prop_string(-1, "fileMap")) {
		num_files = jsal_get_length(-1);
		for (i = 0; i < num_files; ++i) {
			jsal_get_prop_index(-1, i);
			this_source = jsal_get_string(-1);
			if (strcmp(this_source, source_name) == 0)
				strncpy(retval, jsal_get_string(-2), SPHERE_PATH_MAX - 1);
			jsal_pop(1);
		}
		jsal_pop(3);
	}
	else {
		jsal_pop(3);
	}
	return retval;
}

const char*
debugger_source_name(const char* compiled_name)
{
	// note: pathname must be canonicalized using game_full_path() otherwise
	//       the source map lookup will fail.

	static char retval[SPHERE_PATH_MAX];

	strncpy(retval, compiled_name, SPHERE_PATH_MAX - 1);
	retval[SPHERE_PATH_MAX - 1] = '\0';
	if (!s_have_source_map)
		return retval;
	jsal_push_hidden_stash();
	jsal_get_prop_string(-1, "debugMap");
	if (!jsal_get_prop_string(-1, "fileMap"))
		jsal_pop(3);
	else {
		jsal_get_prop_string(-1, compiled_name);
		if (jsal_is_string(-1))
			strncpy(retval, jsal_get_string(-1), SPHERE_PATH_MAX - 1);
		jsal_pop(4);
	}
	return retval;
}

void
debugger_cache_source(const char* name, const lstring_t* text)
{
	struct source cache_entry;

	iter_t iter;
	struct source* p_source;

	if (s_sources == NULL)
		return;

	iter = vector_enum(s_sources);
	while (p_source = iter_next(&iter)) {
		if (strcmp(name, p_source->name) == 0) {
			lstr_free(p_source->text);
			p_source->text = lstr_dup(text);
			return;
		}
	}

	cache_entry.name = strdup(name);
	cache_entry.text = lstr_dup(text);
	vector_push(s_sources, &cache_entry);
}

void
debugger_log(const char* text, print_op_t op, bool use_console)
{
	const char* heading;

	if (use_console) {
		heading = op == PRINT_ASSERT ? "ASSERT"
			: op == PRINT_DEBUG ? "debug"
			: op == PRINT_ERROR ? "ERROR"
			: op == PRINT_INFO ? "info"
			: op == PRINT_TRACE ? "trace"
			: op == PRINT_WARN ? "WARN"
			: "log";
		console_log(0, "%s: %s", heading, text);
	}
}

static bool
do_attach_debugger(void)
{
	double timeout;

	printf("waiting for SSj client to connect...\n");
	fflush(stdout);
	timeout = al_get_time() + 30.0;
	while (s_socket == NULL && al_get_time() < timeout) {
		debugger_update();
		sphere_sleep(0.05);
	}
	if (s_socket == NULL)  // did we time out?
		printf("timed out waiting for SSj\n");
	return s_socket != NULL;
}

static void
do_detach_debugger(bool is_shutdown)
{
	if (!s_is_attached)
		return;

	// detach the debugger
	console_log(1, "detaching SSj debug session");
	s_is_attached = false;
	jsal_debug_detach();
	if (s_socket != NULL) {
		socket_close(s_socket);
		while (socket_connected(s_socket))
			sphere_sleep(0.05);
	}
	socket_unref(s_socket);
	s_socket = NULL;
	if (s_want_attach && !is_shutdown)
		sphere_exit(true);  // clean detach, exit
}

static js_step_t
on_breakpoint_hit(void)
{
	int         column;
	const char* filename;
	int         line;
	dmessage_t* message;

	filename = jsal_get_string(0);
	line = jsal_get_int(1) + 1;
	column = jsal_get_int(2) + 1;

	message = dmessage_new(DMESSAGE_NFY);
	dmessage_add_int(message, NFY_STATUS);
	dmessage_add_int(message, 0);
	dmessage_add_string(message, filename);
	dmessage_add_int(message, line);
	dmessage_add_int(message, column);
	dmessage_send(message, s_socket);
	dmessage_free(message);

	message = dmessage_new(DMESSAGE_NFY);
	dmessage_add_int(message, NFY_STATUS);
	dmessage_add_int(message, 1);
	dmessage_add_string(message, filename);
	dmessage_add_int(message, line);
	dmessage_add_int(message, column);
	dmessage_send(message, s_socket);
	dmessage_free(message);

	return JS_STEP_CONTINUE;
}
