#include "ssj.h"
#include "session.h"

#include "help.h"
#include "inferior.h"
#include "parser.h"

struct session
{
	int         frame;
	inferior_t* inferior;
};

const char* const
command_db[] =
{
	"backtrace",  "bt", "",
	"breakpoint", "bp", "~f",
	"clearbp",    "cb", "n",
	"continue",   "c",  "",
	"down",       "d",  "~n",
	"eval",       "e",  "s",
	"examine",    "x",  "s",
	"frame",      "f",  "~n",
	"list",       "l",  "~nf",
	"stepover",   "s",  "",
	"stepin",     "si", "",
	"stepout",    "so", "",
	"up",         "u",  "~n",
	"vars",       "v",  "",
	"where",      "w",  "",
	"quit",       "q",  "",
	"help",       "h",  "~s",
};

static void        autoselect_frame (session_t* obj);
static void        do_command_line  (session_t* obj);
static const char* find_verb        (command_t* cmd);
static void        handle_backtrace (session_t* obj, command_t* cmd);
static void        handle_eval      (session_t* obj, command_t* cmd, bool is_verbose);
static void        handle_frame     (session_t* obj, command_t* cmd);
static void        handle_help      (session_t* obj, command_t* cmd);
static void        handle_list      (session_t* obj, command_t* cmd);
static void        handle_resume    (session_t* obj, command_t* cmd, resume_op_t op);
static void        handle_up_down   (session_t* obj, command_t* cmd, int direction);
static void        handle_vars      (session_t* obj, command_t* cmd);
static void        handle_where     (session_t* obj, command_t* cmd);
static void        handle_quit      (session_t* obj, command_t* cmd);
static void        preview_frame    (session_t* obj, int frame);
static bool        validate_args    (const command_t* this, const char* verb_name, const char* pattern);

session_t*
session_new(inferior_t* inferior)
{
	session_t* obj;

	obj = calloc(1, sizeof(session_t));
	obj->inferior = inferior;
	return obj;
}

void
session_free(session_t* obj)
{
	free(obj);
}

void
session_run(session_t* obj, bool run_now)
{
	if (run_now) {
		inferior_resume(obj->inferior, OP_RESUME);
		printf("\n");
	}
	autoselect_frame(obj);
	preview_frame(obj, obj->frame);
	
	while (inferior_is_attached(obj->inferior))
		do_command_line(obj);
	printf("SSJ session terminated.\n");
}

static void
autoselect_frame(session_t* obj)
{
	const backtrace_t* calls;

	calls = inferior_get_calls(obj->inferior);
	obj->frame = 0;
	while (backtrace_get_linenum(calls, obj->frame) == 0)
		++obj->frame;
}

static void
do_command_line(session_t* obj)
{
	char               buffer[4096];
	const backtrace_t* calls;
	char               ch;
	command_t*         command;
	const char*        filename;
	const char*        function_name;
	int                line_no;
	const char*        verb;

	int idx;

	calls = inferior_get_calls(obj->inferior);
	function_name = backtrace_get_call_name(calls, obj->frame);
	filename = backtrace_get_filename(calls, obj->frame);
	line_no = backtrace_get_linenum(calls, obj->frame);
	if (line_no != 0)
		printf("\n\33[36;1m%s:%d %s\33[m\n\33[33;1mssj:\33[m ", filename, line_no, function_name);
	else
		printf("\n\33[36;1msyscall %s\33[m\n\33[33;1mssj:\33[m ", function_name);
	idx = 0;
	ch = getchar();
	while (ch != '\n') {
		if (idx >= 4095) {
			printf("string is too long to parse.\n");
			buffer[0] = '\0';
			break;
		}
		buffer[idx++] = ch;
		ch = getchar();
	}
	buffer[idx] = '\0';
	command = command_parse(buffer);
	verb = find_verb(command);

	// figure out which handler to run based on the command name. this could
	// probably be generalized to factor out the massive if/elseif tower, but for
	// now it serves its purpose.
	if (strcmp(verb, "quit") == 0)
		handle_quit(obj, command);
	else if (strcmp(verb, "help") == 0)
		handle_help(obj, command);
	else if (strcmp(verb, "backtrace") == 0)
		handle_backtrace(obj, command);
	else if (strcmp(verb, "up") == 0)
		handle_up_down(obj, command, +1);
	else if (strcmp(verb, "down") == 0)
		handle_up_down(obj, command, -1);
	else if (strcmp(verb, "continue") == 0)
		handle_resume(obj, command, OP_RESUME);
	else if (strcmp(verb, "eval") == 0)
		handle_eval(obj, command, false);
	else if (strcmp(verb, "examine") == 0)
		handle_eval(obj, command, true);
	else if (strcmp(verb, "frame") == 0)
		handle_frame(obj, command);
	else if (strcmp(verb, "list") == 0)
		handle_list(obj, command);
	else if (strcmp(verb, "stepover") == 0)
		handle_resume(obj, command, OP_STEP_OVER);
	else if (strcmp(verb, "stepin") == 0)
		handle_resume(obj, command, OP_STEP_IN);
	else if (strcmp(verb, "stepout") == 0)
		handle_resume(obj, command, OP_STEP_OUT);
	else if (strcmp(verb, "vars") == 0)
		handle_vars(obj, command);
	else if (strcmp(verb, "where") == 0)
		handle_where(obj, command);
	else
		printf("'%s': not implemented.\n", verb);
	command_free(command);
}

static const char*
find_verb(command_t* command)
{
	const char* full_name;
	const char* matches[100];
	int         num_commands;
	int         num_matches = 0;
	const char* pattern;
	const char* short_name;
	const char* verb;

	int i;

	if (command_len(command) < 1)
		return NULL;

	num_commands = sizeof(command_db) / sizeof(command_db[0]) / 3;
	verb = command_get_string(command, 0);
	for (i = 0; i < num_commands; ++i) {
		full_name = command_db[0 + i * 3];
		short_name = command_db[1 + i * 3];
		if (strcmp(verb, short_name) == 0) {
			matches[0] = full_name;
			pattern = command_db[2 + i * 3];
			num_matches = 1;  // canonical short name is never ambiguous
			break;
		}
		if (strstr(full_name, verb) == full_name) {
			matches[num_matches] = full_name;
			if (num_matches == 0)
				pattern = command_db[2 + i * 3];
			++num_matches;
		}
	}

	if (num_matches == 1)
		return validate_args(command, matches[0], pattern) ? matches[0] : NULL;
	else if (num_matches > 1) {
		printf("'%s': abbreviated name is ambiguous between:\n", verb);
		for (i = 0; i < num_matches; ++i)
			printf("    * %s\n", matches[i]);
		return NULL;
	}
	else {
		printf("'%s': unrecognized command name.\n", verb);
		return NULL;
	}
}

static void
handle_backtrace(session_t* obj, command_t* cmd)
{
	const backtrace_t* calls;

	if (!(calls = inferior_get_calls(obj->inferior)))
		return;
	backtrace_print(calls, obj->frame, true);
}

static void
handle_resume(session_t* obj, command_t* cmd, resume_op_t op)
{
	inferior_resume(obj->inferior, op);
	if (inferior_is_attached(obj->inferior))
		autoselect_frame(obj);
	if (op == OP_RESUME)
		printf("\n");
	preview_frame(obj, obj->frame);
}

static void
handle_eval(session_t* obj, command_t* cmd, bool is_verbose)
{
	const char*     expr;
	const dvalue_t* getter;
	remote_ptr_t    heapptr;
	bool            is_accessor;
	bool            is_error;
	objview_t*      object;
	unsigned int    prop_flags;
	const char*     prop_key;
	const dvalue_t* setter;
	dvalue_t*       result;

	int i = 0;

	expr = command_get_string(cmd, 1);
	result = inferior_eval(obj->inferior, expr, obj->frame, &is_error);
	printf(is_error ? "error: " : "= ");
	if (dvalue_tag(result) != DVALUE_OBJ)
		dvalue_print(result, is_verbose);
	else {
		heapptr = dvalue_as_ptr(result);
		if (!(object = inferior_get_object(obj->inferior, heapptr, is_verbose)))
			return;
		printf("{\n");
		for (i = 0; i < objview_len(object); ++i) {
			is_accessor = objview_get_tag(object, i) == PROP_ACCESSOR;
			prop_key = objview_get_key(object, i);
			prop_flags = objview_get_flags(object, i);
			if (!(prop_flags & PROP_ENUMERABLE) && !is_verbose)
				continue;
			printf("    %s%s%s  \"%s\" : ",
				prop_flags & PROP_WRITABLE ? "w" : "-",
				prop_flags & PROP_ENUMERABLE ? "e" : "-",
				prop_flags & PROP_CONFIGURABLE ? "c" : "-",
				prop_key);
			if (!is_accessor)
				dvalue_print(objview_get_value(object, i), is_verbose);
			else {
				getter = objview_get_getter(object, i);
				setter = objview_get_setter(object, i);
				printf("{ get: ");
				dvalue_print(getter, is_verbose);
				printf(", set: ");
				dvalue_print(setter, is_verbose);
				printf(" }");
			}
			printf("\n");
		}
		printf("}");
		objview_free(object);
	}
	printf("\n");
	dvalue_free(result);
}

static void
handle_frame(session_t* obj, command_t* cmd)
{
	const backtrace_t* calls;
	int                frame;

	if (!(calls = inferior_get_calls(obj->inferior)))
		return;
	frame = obj->frame;
	if (command_len(cmd) >= 2)
		frame = command_get_int(cmd, 1);
	if (frame < 0 || frame >= backtrace_len(calls))
		printf("stack frame #%2d doesn't exist.\n", frame);
	else {
		obj->frame = frame;
		preview_frame(obj, obj->frame);
	}
}

static void
handle_help(session_t* obj, command_t* cmd)
{
	help_print(command_len(cmd) > 0 ? command_get_string(cmd, 1) : NULL);
}

static void
handle_list(session_t* obj, command_t* cmd)
{
	const char*        active_filename;
	int                active_lineno = 0;
	const backtrace_t* calls;
	const char*        filename;
	int                lineno;
	int                num_lines = 10;
	const source_t*    source;

	calls = inferior_get_calls(obj->inferior);
	active_filename = backtrace_get_filename(calls, obj->frame);
	active_lineno = backtrace_get_linenum(calls, obj->frame);
	filename = active_filename;
	lineno = active_lineno;
	if (command_len(cmd) >= 2)
		num_lines = command_get_int(cmd, 1);
	if (command_len(cmd) >= 3) {
		filename = command_get_string(cmd, 2);
		lineno = command_get_int(cmd, 2);
	}
	if (!(source = inferior_get_source(obj->inferior, filename)))
		printf("source unavailable for %s.\n", filename);
	else {
		if (strcmp(filename, active_filename) != 0)
			active_lineno = 0;
		source_print(source, lineno, num_lines, active_lineno);
	}
}

static void
handle_up_down(session_t* obj, command_t* cmd, int direction)
{
	const backtrace_t* calls;
	int                new_frame;
	int                num_steps;

	if (!(calls = inferior_get_calls(obj->inferior)))
		return;
	new_frame = obj->frame + direction;
	if (new_frame >= backtrace_len(calls))
		printf("innermost frame: can't go up any further.\n");
	else if (new_frame < 0)
		printf("outermost frame: can't go down any further.\n");
	else {
		num_steps = command_len(cmd) >= 2 ? command_get_int(cmd, 1) : 1;
		new_frame = obj->frame + num_steps * direction;
		obj->frame = new_frame < 0 ? 0
			: new_frame >= backtrace_len(calls) ? backtrace_len(calls) - 1
			: new_frame;
		preview_frame(obj, obj->frame);
	}
}

static void
handle_vars(session_t* obj, command_t* cmd)
{
	const backtrace_t* calls;
	const char*        call_name;
	const dvalue_t*    value;
	const objview_t*   vars;
	const char*        var_name;

	int i;

	if (!(calls = inferior_get_calls(obj->inferior)))
		return;
	if (!(vars = inferior_get_vars(obj->inferior, obj->frame)))
		return;
	if (objview_len(vars) == 0) {
		call_name = backtrace_get_call_name(calls, obj->frame);
		printf("%s has no local variables.\n", call_name);
	}
	for (i = 0; i < objview_len(vars); ++i) {
		var_name = objview_get_key(vars, i);
		value = objview_get_value(vars, i);
		printf("var %s = ", var_name);
		dvalue_print(value, false);
		printf("\n");
	}
}

static void
handle_where(session_t* obj, command_t* cmd)
{
	int                frame = 0;
	const backtrace_t* calls;

	if (!(calls = inferior_get_calls(obj->inferior)))
		return;
	while (backtrace_get_linenum(calls, frame) <= 0)
		++frame;
	preview_frame(obj, obj->frame);
}

static void
handle_quit(session_t* obj, command_t* cmd)
{
	inferior_detach(obj->inferior);
}

static void
preview_frame(session_t* obj, int frame)
{
	const backtrace_t* calls;
	const char*        filename;
	int                lineno;
	const source_t*    source;

	if (!(calls = inferior_get_calls(obj->inferior)))
		return;
	backtrace_print(calls, frame, false);
	filename = backtrace_get_filename(calls, frame);
	lineno = backtrace_get_linenum(calls, frame);
	if (lineno == 0)
		printf("system call - no source provided\n");
	else {
		if (!(source = inferior_get_source(obj->inferior, filename)))
			printf("source unavailable for %s.\n", filename);
		else
			source_print(source, lineno, 1, lineno);
	}
}

static bool
validate_args(const command_t* this, const char* verb_name, const char* pattern)
{
	int         index = 0;
	int         want_num_args;
	token_tag_t want_tag;
	const char* want_type;
	const char  *p_type;

	if (strchr(pattern, '~'))
		want_num_args = (int)(strchr(pattern, '~') - pattern);
	else
		want_num_args = (int)strlen(pattern);
	if (command_len(this) - 1 < want_num_args) {
		printf("'%s': expected at least %d arguments.\n", verb_name, want_num_args);
		return false;
	}
	p_type = pattern;
	while (index < command_len(this) - 1) {
		if (*p_type == '~') ++p_type;
		if (*p_type == '\0') break;
		switch (*p_type) {
		case 's':
			want_tag = TOK_STRING;
			want_type = "string";
			break;
		case 'n':
			want_tag = TOK_NUMBER;
			want_type = "number";
			break;
		case 'f':
			want_tag = TOK_FILE_LINE;
			want_type = "file:line";
			break;
		}
		if (command_get_tag(this, index + 1) != want_tag)
			goto wrong_type;
		++p_type;
		++index;
	}
	return true;

wrong_type:
	printf("'%s': expected a %s for argument %d.\n", verb_name, want_type, index + 1);
	return false;
}
