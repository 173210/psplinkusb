/*
 * PSPLINK
 * -----------------------------------------------------------------------
 * Licensed under the BSD license, see LICENSE in PSPLINK root for details.
 *
 * pspsh.c - PSPLINK pc terminal
 *
 * Copyright (c) 2006 James F <tyranid@gmail.com>
 *
 * $HeadURL$
 * $Id$
 */
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <netdb.h>
#include <limits.h>
#include <readline/readline.h>
#include <readline/history.h>
#include <errno.h>
#include <signal.h>
#include <shellcmd.h>
#include "parse_args.h"

#ifndef SOL_TCP
#define SOL_TCP 6
#endif

#define DEFAULT_PORT 10000
#define HISTORY_FILE ".pspsh.hist"
#define DEFAULT_IP     "localhost"

struct Args
{
	const char *ip;
	const char *hist;
	unsigned short port;
	/* Indicates we are in script mode */
	int script;
	/* Holds the current execution line */
	char exec[1024];
	/* Script arguments */
	int sargc;
	char **sargv;
	int notty;
};

struct GlobalContext
{
	struct Args args;
	int exit;
	int conn_sanity;
	fd_set readsave;
	int sock;
	int outsock;
	int errsock;
	char history_file[PATH_MAX];
	char currpath[PATH_MAX];
	char currcmd[PATH_MAX];
	FILE *fredir;
	FILE *fscript;
	char **sargv;
	int  sargc;
	int  lasterr;
	FILE *fstdout;
	FILE *fstderr;
};

struct GlobalContext g_context;
extern char **environ;

int help_cmd(int argc, char **argv);
int close_cmd(int argc, char **argv);
int exit_cmd(int argc, char **argv);
int env_cmd(int argc, char **argv);
int set_cmd(int argc, char **argv);
int unset_cmd(int argc, char **argv);
int echo_cmd(int argc, char **argv);
void cli_handler(char *buf);
struct TabEntry* read_tab_completion(void);
struct TabEntry
{
	struct TabEntry *pNext;
	char *name;
};

struct sh_command g_commands[] = { SHELL_COMMANDS };

const char *g_drive_prefix[] = { 
	"ms0:/",
	"flash0:/",
	"flash1:/",
	"disc0:/",
	"host0:/",
	"host1:/",
	"host2:/",
	"host3:/",
	"host4:/",
	"host5:/",
	"host6:/",
	"host7:/",
	NULL
};

/* Find a command from the command list */
const struct sh_command* find_command(const char *cmd)
{
	const struct sh_command* found_cmd = NULL;
	int cmd_loop;

	for(cmd_loop = 0; g_commands[cmd_loop].name != NULL; cmd_loop++)
	{
		if(strcmp(cmd, g_commands[cmd_loop].name) == 0)
		{
			found_cmd = &g_commands[cmd_loop];
			break;
		}

		if(g_commands[cmd_loop].syn)
		{
			if(strcmp(cmd, g_commands[cmd_loop].syn) == 0)
			{
				found_cmd = &g_commands[cmd_loop];
				break;
			}
		}
	}

	return found_cmd;
}


int fixed_write(int s, const void *buf, int len)
{
	int written = 0;

	while(written < len)
	{
		int ret;

		ret = write(s, buf+written, len-written);
		if(ret < 0)
		{
			if(errno != EINTR)
			{
				perror("write");
				written = -1;
				break;
			}
		}
		else
		{
			written += ret;
		}
	}

	return written;
}

int execute_line(const char *buf)
{
	char args[4096];
	char *argv[16];
	int  argc;
	int len;
	int ret = 0;

	len = strlen(buf);

	if(len > 0)
	{
		char redir[PATH_MAX];
		int  type;
		int binlen = parse_cli(buf, args, &argc, argv, 16, g_context.sargc, g_context.sargv, &type, redir);
		if((binlen > 0) && (args[0] != '#'))
		{
			if(strchr(argv[0], '.') || strchr(argv[0], '/'))
			{
				int ldlen = strlen("ld")+1;
				/* If this looks to be a path then prefix ld and send */
				memmove(args+ldlen, args, binlen);
				memcpy(args, "ld", ldlen);
				binlen += ldlen;
			}
			else
			{
				const struct sh_command *cmd = find_command(argv[0]);
				if((cmd) && (cmd->func))
				{
					if(!cmd->func(argc-1, argv+1))
					{
						/* If it returns 0 then dont continue with the output */
						return 0;
					}
				}
			}

			if(!g_context.args.script)
			{
				/* Remove the handler and prompt */
				rl_callback_handler_remove();
				rl_callback_handler_install("", cli_handler);
			}

			if(type != REDIR_TYPE_NONE)
			{
				if(type == REDIR_TYPE_NEW)
				{
					g_context.fredir = fopen(redir, "w");
				}
				else
				{
					g_context.fredir = fopen(redir, "a");
				}

				if(g_context.fredir == NULL)
				{
					fprintf(stderr, "Warning: Could not open file %s\n", redir);
				}
			}

			len = fixed_write(g_context.sock, args, binlen);
			if(len < 0)
			{
				close(g_context.sock);
				g_context.sock = -1;
				return -1;
			}
			strcpy(g_context.currcmd, args);
			ret = 1;
		}
	}

	return ret;
}

void close_script(void)
{
	if(g_context.fscript)
	{
		if(g_context.fscript != stdin)
		{
			fclose(g_context.fscript);
		}
		g_context.fscript = NULL;
	}

	if(g_context.sargv)
	{
		int i;

		for(i = 0; i < g_context.sargc; i++)
		{
			if(g_context.sargv[i])
			{
				free(g_context.sargv[i]);
			}
		}
		free(g_context.sargv);
		g_context.sargv = NULL;
	}

	g_context.sargc = 0;

	if(g_context.args.script)
	{
		g_context.exit = 1;
	}
}

int open_script(const char *file, int sargc, char **sargv)
{
	int ret = 0;
	int i;

	do
	{
		/* Ensure script is closed */
		close_script();

		if(strcmp(file, "-") == 0)
		{
			g_context.fscript = stdin;
		}
		else
		{
			g_context.fscript = fopen(file, "r");
			if(g_context.fscript == NULL)
			{
				fprintf(stderr, "Could not open script file %s\n", file);
				break;
			}
		}

		if(sargc > 0)
		{
			g_context.sargv = (char**) malloc(sizeof(char*)*sargc);
			if(g_context.sargv == NULL)
			{
				fprintf(stderr, "Could not allocate script arguments\n");
				break;
			}
			for(i = 0; i < sargc; i++)
			{
				g_context.sargv[i] = strdup(sargv[i]);
				if(g_context.sargv[i] == NULL)
				{
					fprintf(stderr, "Could not allocate argument string %d\n", i);
					break;
				}
			}

			if(i < sargc)
			{
				break;
			}

			g_context.sargc = sargc;
		}

		ret = 1;
	}
	while(0);

	if(!ret)
	{
		close_script();
	}

	return ret;
}

int in_script(void)
{
	if(g_context.args.exec[0])
	{
		return 1;
	}

	if(g_context.fscript)
	{
		return 1;
	}

	return 0;
}

int execute_script_line(void)
{
	char line[1024];

	if(g_context.args.exec[0])
	{
		int ret = execute_line(g_context.args.exec);
		g_context.args.exec[0] = ' ';
		g_context.args.exec[1] = 0;
		if(ret < 0)
		{
			return -1;
		}

		if(ret > 0)
		{
			return 1;
		}

		/* Only way we get here is if there were no lines to execute */
		close_script();
	}
	else if(g_context.fscript)
	{
		while(fgets(line, sizeof(line), g_context.fscript))
		{
			int ret = execute_line(line);
			if(ret < 0)
			{
				return -1;
			}

			/* Only return if we have sent a line to the PSP */
			if(ret > 0)
			{
				return 1;
			}
		}

		/* Only way we get here is if there were no lines to execute */
		close_script();
	}

	return 0;
}

int close_cmd(int argc, char **argv)
{
	if(g_context.args.script == 0)
	{
		rl_callback_handler_remove();
		rl_callback_handler_install("", cli_handler);
	}
	g_context.exit = 1;
	return 0;
}

int execute_script(const char *cmd)
{
	char args[4096];
	char *argv[16];
	int  argc;
	int len;

	len = strlen(cmd);

	if(len > 0)
	{
		char redir[PATH_MAX];
		int  type;
		int binlen = parse_cli(cmd, args, &argc, argv, 16, 0, NULL, &type, redir);
		if(binlen > 0)
		{
			if(open_script(argv[0], argc, argv))
			{
				if(execute_script_line() > 0)
				{
					return 1;
				}
			}
		}
	}

	return 0;
}

int exit_cmd(int argc, char **argv)
{
	g_context.exit = 1;
	return 1;
}

void cli_handler(char *buf)
{
	if(buf)
	{
		while(isspace(*buf))
		{
			buf++;
		}

		if(*buf == 0)
		{
			return;
		}

		if(in_script())
		{
			/* When a script is running only accept stop */
			if(strcmp(buf, "stop") == 0)
			{
				close_script();
			}

			return;
		}

		add_history(buf);
		if(buf[0] == '!')
		{
			if(strncmp(&buf[1], "cd ", 3) == 0)
			{
				chdir(&buf[4]);
			}
			else
			{
				system(&buf[1]);
			}
			return;
		}
		else if(buf[0] == '@')
		{
			/* Send to hostfs */
			return;
		}
		else if(buf[0] == '%')
		{
			execute_script(&buf[1]);
			return;
		}

		execute_line(buf);
	}
}

int cli_reset()
{
	execute_line("reset");

	return 0;
}

int cli_step()
{
	execute_line("step");

	return 0;
}

int cli_skip()
{
	execute_line("skip");

	return 0;
}

/* Complete a command name */
char *command_gen(const char *text, int state)
{
	static int cmdno;
	static int driveno;
	static int locals;
	static int len;

	if(state == 0)
	{
		cmdno = 0;
		driveno = 0;
		locals = 0;
		len = strlen(text);
	}

	while(g_commands[cmdno].name)
	{
		const char *name = g_commands[cmdno].name;
		const char *syn = g_commands[cmdno].syn;

		cmdno++;

		if(strncmp(name, text, len) == 0)
		{
			return strdup(name);
		}
		else if((syn) && (strncmp(syn, text, len) == 0))
		{
			return strdup(syn);
		}
	}

	while(g_drive_prefix[driveno])
	{
		const char *name = g_drive_prefix[driveno];

		driveno++;
		if(strncmp(name, text, len) == 0)
		{
			rl_completion_append_character = 0;
			return strdup(name);
		}
	}

	return NULL;
}

struct TabEntry* add_drive_prefixes(const char *text, struct TabEntry *pEntry)
{
	struct TabEntry head;
	struct TabEntry *pCurr;
	int len;
	int i;

	len = strlen(text);
	pCurr = &head;
	memset(&head, 0, sizeof(head));
	i = 0;
	while(g_drive_prefix[i])
	{
		if(strncmp(g_drive_prefix[i], text, len) == 0)
		{
			pCurr->pNext = (struct TabEntry *) malloc(sizeof(struct TabEntry));
			memset(pCurr->pNext, 0, sizeof(struct TabEntry));
			pCurr->pNext->name = strdup(g_drive_prefix[i]);
			pCurr = pCurr->pNext;
		}
		i++;
	}

	if(head.pNext)
	{
		pCurr->pNext = pEntry;
		return head.pNext;
	}

	return pEntry;
}

/* Complete a file name */
char *filename_gen(const char *text, int state)
{
	static char dirpath[PATH_MAX];
	static struct TabEntry *pEntry = NULL;
	char *name;

	name = NULL;
	if(state == 0)
	{
		char cmd[PATH_MAX*2+5];
		char *curr;
		char filepath[PATH_MAX];
		char *slash;

		/* Free list if it exists */
		while(pEntry)
		{
			struct TabEntry *pTemp;
			pTemp = pEntry->pNext;
			free(pEntry->name);
			free(pEntry);
			pEntry = pTemp->pNext;
		}

		strcpy(dirpath, text);
		memset(filepath, 0, sizeof(filepath));
		slash = strrchr(dirpath, '/');
		if(slash)
		{
			slash++;
			strcpy(filepath, slash);
			*slash = 0;
		}
		else
		{
			strcpy(filepath, dirpath);
			strcpy(dirpath, "");
		}

		curr = cmd;
		strcpy(curr, "tab");
		curr += strlen(curr)+1;
		strcpy(curr, dirpath);
		curr += strlen(curr)+1;
		if(filepath[0])
		{
			strcpy(curr, filepath);
			curr += strlen(curr)+1;
		}
		*curr++ = 1;
		write(g_context.sock, cmd, curr-cmd);
		pEntry = read_tab_completion();
		if(pEntry == NULL)
		{
			pEntry = add_drive_prefixes(text, pEntry);
			dirpath[0] = 0;
			filepath[0] = 0;
		}
	}

	if(pEntry)
	{
		struct TabEntry *pNext;
		
		name = (char*) malloc(strlen(dirpath) + strlen(pEntry->name) + 1);
		if(name)
		{
			sprintf(name, "%s%s", dirpath, pEntry->name);
		}
		pNext = pEntry->pNext;
		free(pEntry->name);
		free(pEntry);
		pEntry = pNext;
		if(name[strlen(name)-1] == '/')
		{
			rl_completion_append_character = 0;
		}
		else
		{
			rl_completion_append_character = ' ';
		}
	}

	return name;
}

/* Completion display, get readline to still do most of the work for us */
void completion_display(char **matches, int num, int max)
{
	int temp = rl_filename_completion_desired;

	rl_filename_completion_desired = 1;
	rl_display_match_list(matches, num, max);
	rl_forced_update_display ();
	rl_filename_completion_desired = temp;
}

char** shell_completion(const char *text, int start, int end)
{
	char **matches = (char**) NULL;
	char *curr_line = rl_line_buffer;

	while(isspace(*curr_line))
	{
		curr_line++;
	}

	rl_completion_append_character = ' ';
	rl_completion_display_matches_hook = NULL;

	/* Find if this is current in shell or usbhostfs modes */
	if((*curr_line == '!') || (*curr_line == '@') || (*curr_line == '%'))
	{
		/* Do normal (local) filename completion */
		matches = rl_completion_matches(text, rl_filename_completion_function);
	}
	else if(start == (curr_line-rl_line_buffer)) /* If this is the first word in the list */
	{
		if(strchr(text, '.') || strchr(text, '/'))
		{
			rl_completion_display_matches_hook = completion_display;
			matches = rl_completion_matches(text, filename_gen);
		}
		else
		{
			matches = rl_completion_matches(text, command_gen);
		}
	}
	else
	{
		rl_completion_display_matches_hook = completion_display;
		matches = rl_completion_matches(text, filename_gen);
	}

	return matches;
}

int init_readline(void)
{
	rl_bind_key_in_map(META('r'), cli_reset, emacs_standard_keymap);
	rl_bind_key_in_map(META('s'), cli_step, emacs_standard_keymap);
	rl_bind_key_in_map(META('k'), cli_skip, emacs_standard_keymap);
	rl_attempted_completion_function = shell_completion;
	rl_callback_handler_install("", cli_handler);

	return 1;
}

int parse_args(int argc, char **argv, struct Args *args)
{
	memset(args, 0, sizeof(*args));
	args->port = DEFAULT_PORT;
	args->ip = DEFAULT_IP;

	while(1)
	{
		int ch;
		int error = 0;

		ch = getopt(argc, argv, "np:h:i:e:");
		if(ch < 0)
		{
			break;
		}

		switch(ch)
		{
			case 'p': args->port = atoi(optarg);
					  break;
			case 'h': args->hist = optarg;
					  break;
			case 'n': args->notty = 1;
					  break;
			case 'i': args->ip = optarg;
					  break;
			case 'e': snprintf(args->exec, sizeof(args->exec), "%s", optarg);
					  args->script = 1;
					  break;
			default : error = 1;
					  break;
		};

		if(error)
		{
			return 0;
		}
	}

	argc -= optind;
	argv += optind;
	if(args->exec[0] && (argc > 0))
	{
		if(!open_script(argv[0], argc, argv))
		{
			return 0;
		}
		args->script = 1;
		args->notty = 1;
	}

	return 1;
}

void print_help(void)
{
	fprintf(stderr, "PSPSH Help\n");
	fprintf(stderr, "Usage: pspsh [options] [script args...]\n");
	fprintf(stderr, "Options:\n");
	fprintf(stderr, "-i ipaddr   : Specify the IP address to connect to\n");
	fprintf(stderr, "-p port     : Specify the port number\n");
	fprintf(stderr, "-h history  : Specify the history file (default ~/%s)\n", HISTORY_FILE);
	fprintf(stderr, "-e cmd      : Execute a command and exit\n");
	fprintf(stderr, "-n          : Do not connect up the tty (stdin/stdout/stderr)\n");
}

int init_sockaddr(struct sockaddr_in *name, const char *ipaddr, unsigned short port)
{
	struct hostent *hostinfo;

	name->sin_family = AF_INET;
	name->sin_port = htons(port);
	hostinfo = gethostbyname(ipaddr);
	if(hostinfo == NULL)
	{
		fprintf(stderr, "Unknown host %s\n", ipaddr);
		return 0;
	}
	name->sin_addr = *(struct in_addr *) hostinfo->h_addr;

	return 1;
}

int help_cmd(int argc, char **argv)
{
	int cmd_loop;

	if(argc < 1)
	{
		fprintf(stderr, "Command Categories\n\n");
		for(cmd_loop = 0; g_commands[cmd_loop].name; cmd_loop++)
		{
			if(g_commands[cmd_loop].help == NULL)
			{
				fprintf(stderr, "%-10s - %s\n", g_commands[cmd_loop].name, g_commands[cmd_loop].desc);
			}
		}
		fprintf(stderr, "\nType 'help category' for more information\n");
	}
	else
	{
		const struct sh_command* found_cmd;

		found_cmd = find_command(argv[0]);
		if((found_cmd != NULL) && (found_cmd->desc))
		{
			if(found_cmd->help == NULL)
			{
				/* Print the commands listed under the separator */
				fprintf(stderr, "Category %s\n\n", found_cmd->name);
				for(cmd_loop = 1; found_cmd[cmd_loop].name && found_cmd[cmd_loop].help != NULL; cmd_loop++)
				{
					if(found_cmd[cmd_loop].desc)
					{
						fprintf(stderr, "%-10s - %s\n", found_cmd[cmd_loop].name, found_cmd[cmd_loop].desc);
					}
				}
			}
			else
			{
				fprintf(stderr, "Command: %s\t\n", found_cmd->name);
				if(found_cmd->syn)
				{
					fprintf(stderr, "Synonym: %s\n", found_cmd->syn);
				}
				fprintf(stderr, "Usage: %s %s\n", found_cmd->name, found_cmd->help);
				fprintf(stderr, "%s\n\n", found_cmd->desc);
				fprintf(stderr, "Detail:\n");
				fprintf(stderr, "%s\n", found_cmd->detail);
			}
		}
		else
		{
			fprintf(stderr, "Unknown command %s, type help for information\n", argv[0]);
		}
	}

	return 0;
}

int env_cmd(int argc, char **argv)
{
	int i = 0;
	while(environ[i])
	{
		printf("%s\n", environ[i]);
		i++;
	}

	return 0;
}

int set_cmd(int argc, char **argv)
{
	char *name;
	char *value;
	int i;

	do
	{
		name = argv[0];
		value = strchr(argv[0], '=');
		if(value)
		{
			*value++ = 0;
		}
		else
		{
			printf("Error, no value specified\n");
			break;
		}

		if(argv[0][0] == 0)
		{
			printf("Error, no name specified\n");
			break;
		}

		if(!isalpha(name[0]))
		{
			printf("Error, variable names must start with a letter\n");
			break;
		}

		i = 0;
		while(name[i])
		{
			if(!isalnum(name[i]))
			{
				printf("Error, variable names must be alphanumeric\n");
				break;
			}
			i++;
		}

		if(setenv(name, value, 1) < 0)
		{
			perror("setenv");
			break;
		}
	}
	while(0);

	return 0;
}

int unset_cmd(int argc, char **argv)
{
	(void) unsetenv(argv[0]);

	return 0;
}

int echo_cmd(int argc, char **argv)
{
	int i;
	for(i = 0; i < argc; i++)
	{
		printf("%s", argv[i]);
		if(i < (argc-1))
		{
			printf(" ");
		}
	}
	printf("\n");

	return 0;
}

int process_cmd(const unsigned char *str)
{
	if(*str < 128)
	{
		if(g_context.fredir)
		{
			fprintf(g_context.fredir, "%s", str);
		}
		else
		{
			printf("%s", str);
		}
	}
	else
	{
		if(*str == SHELL_CMD_CWD)
		{
			snprintf(g_context.currpath, PATH_MAX, "%s", str+1);
		}
		else if((*str == SHELL_CMD_SUCCESS) || (*str == SHELL_CMD_ERROR))
		{
			char prompt[PATH_MAX];

			if(*str == SHELL_CMD_ERROR)
			{
				/* If there was a command then print the help */
				if(g_context.currcmd[0])
				{
					const struct sh_command *cmd = find_command(g_context.currcmd);
					if(cmd == NULL)
					{
						fprintf(stderr, "Unknown command %s\n", g_context.currcmd);
					}
					else
					{
						if(cmd->help)
						{
							fprintf(stderr, "Usage: %s\n", cmd->help);
						}
						else
						{
							fprintf(stderr, "Command %s has no help associated\n", g_context.currcmd);
						}
					}
				}

				/* On error stop any executing script */
				if(in_script())
				{
					close_script();
				}

				/* Set return code */
				g_context.lasterr = 1;
			}
			else
			{
				g_context.lasterr = 0;
			}

			(void) setenv("?", (char*) (str+1), 1);
			g_context.currcmd[0] = 0;
			if(g_context.fredir)
			{
				fclose(g_context.fredir);
				g_context.fredir = NULL;
			}
			else
			{
				fflush(stdout);
			}

			/* Only restore if there is no pending script and we didn't execute a line */
			if((execute_script_line() <= 0) && (g_context.args.script == 0))
			{
				rl_callback_handler_remove();
				snprintf(prompt, PATH_MAX, "%s> ", g_context.currpath);
				rl_callback_handler_install(prompt, cli_handler);
			}
		}
		else if(*str == SHELL_CMD_TAB)
		{
			fprintf(stderr, "Mismatched Tab Match: %s\n", str+1);
		}
	}

	return 1;
}

int read_socket(int sock)
{
	static unsigned char linebuf[16*1024];
	static int pos = 0;
	unsigned char buf[1024];
	unsigned char *curr;
	int len;

	len = read(sock, buf, sizeof(buf)-1);
	if(len < 0)
	{
		perror("read");
		return -1;
	}

	/* EOF */
	if(len == 0)
	{
		return -1;
	}

	buf[len] = 0;
	curr = buf;

	while(*curr)
	{
		if(*curr == SHELL_CMD_BEGIN)
		{
			/* Reset start pos, discard any data we ended up missing */
			pos = 1;
		}
		else if(*curr == SHELL_CMD_END)
		{
			if(pos > 1)
			{
				linebuf[pos-1] = 0;
				/* Found a command, process */
				process_cmd(linebuf);
				pos = 0;
			}
		}
		else
		{
			if(pos > 0)
			{
				linebuf[pos-1] = *curr;
				pos++;
			}
		}

		curr++;
	}

	return len;
}

int read_outsocket(int sock)
{
	char buf[1024];
	int len;

	len = read(sock, buf, sizeof(buf)-1);
	if(len < 0)
	{
		perror("read");
		return -1;
	}

	/* EOF */
	if(len == 0)
	{
		return -1;
	}

	buf[len] = 0;

	fprintf(g_context.fstdout, "%s", buf);

	return len;
}

int read_errsocket(int sock)
{
	char buf[1024];
	int len;

	len = read(sock, buf, sizeof(buf)-1);
	if(len < 0)
	{
		perror("read");
		return -1;
	}

	/* EOF */
	if(len == 0)
	{
		return -1;
	}

	buf[len] = 0;

	fprintf(g_context.fstderr, "%s", buf);

	return len;
}

int connect_to(const char *ipaddr, unsigned short port)
{
	int sock = -1;
	int success = 0;
	struct sockaddr_in name;

	do
	{
		if(!init_sockaddr(&name, ipaddr, port))
		{
			break;
		}
		sock = socket(PF_INET, SOCK_STREAM, 0);
		if(sock < 0)
		{
			perror("socket");
			break;
		}

		if(connect(sock, (struct sockaddr *) &name, sizeof(name)) < 0)
		{
			perror("connect");
			break;
		}
		else
		{
			FD_SET(sock, &g_context.readsave);
		}

		success = 1;
	}
	while(0);

	if(!success)
	{
		if(sock >= 0)
		{
			close(sock);
			sock = -1;
		}
	}

	return sock;
}

int read_tab(int sock, struct TabEntry *head)
{
	static unsigned char linebuf[16*1024];
	static int pos = 0;
	unsigned char buf[1024];
	unsigned char *curr;
	int len;
	int ret = 0;
	struct TabEntry *tabcurr;

	if(head == NULL)
	{
		fprintf(stderr, "Internal Error (head==NULL)\n");
		return -1;
	}

	tabcurr = head;
	while(tabcurr->pNext)
	{
		tabcurr = tabcurr->pNext;
	}

	len = read(sock, buf, sizeof(buf)-1);
	if(len < 0)
	{
		perror("read");
		return -1;
	}

	/* EOF */
	if(len == 0)
	{
		return -1;
	}

	buf[len] = 0;
	curr = buf;

	while(*curr)
	{
		if(*curr == SHELL_CMD_BEGIN)
		{
			/* Reset start pos, discard any data we ended up missing */
			pos = 1;
		}
		else if(*curr == SHELL_CMD_END)
		{
			if(pos > 1)
			{
				linebuf[pos-1] = 0;
				/* Found a command, process */
				if(linebuf[0] < 128)
				{
					printf("%s", linebuf);
					fflush(stdout);
				}
				else if((linebuf[0] == SHELL_CMD_SUCCESS) || (linebuf[0] == SHELL_CMD_ERROR))
				{
					ret = -1;
				}
				else if(linebuf[0] == SHELL_CMD_TAB)
				{
					tabcurr->pNext = (struct TabEntry*) malloc(sizeof(struct TabEntry));
					if(tabcurr->pNext != NULL)
					{
						memset(tabcurr->pNext, 0, sizeof(struct TabEntry));
						tabcurr->pNext->name = strdup((char *) (linebuf+1));
						if(tabcurr->pNext->name == NULL)
						{
							free(tabcurr->pNext);
							tabcurr->pNext = NULL;
						}
						else
						{
							tabcurr = tabcurr->pNext;
						}
					}
				}

				pos = 0;
			}
		}
		else
		{
			if(pos > 0)
			{
				linebuf[pos-1] = *curr;
				pos++;
			}
		}

		curr++;
	}

	return ret;
}

struct TabEntry* read_tab_completion(void)
{
	fd_set readset;
	fd_set readsave;
	struct timeval tv;
	struct TabEntry head;

	if(g_context.sock < 0)
	{
		return NULL;
	}
	memset(&head, 0, sizeof(head));
	
	FD_ZERO(&readsave);
	FD_SET(g_context.sock, &readsave);

	while(1)
	{
		int ret;

		tv.tv_sec = 2;
		tv.tv_usec = 0;
		readset = readsave;
		ret = select(FD_SETSIZE, &readset, NULL, NULL, &tv);
		if(ret < 0)
		{
			if(errno == EINTR)
			{
				continue;
			}

			perror("select");
			break;
		}
		else if(ret == 0)
		{
			/* Timeout */
			break;
		}
		else
		{
			if(FD_ISSET(g_context.sock, &readset))
			{
				/* Do read */
				if(read_tab(g_context.sock, &head) < 0)
				{
					break;
				}
			}
		}
	}

	return head.pNext;
}

int shell(void)
{
	fd_set readset;
	FD_ZERO(&g_context.readsave);

	fprintf(stderr, "Opening connection to %s port %d\n", g_context.args.ip, g_context.args.port);
	if((g_context.sock = connect_to(g_context.args.ip, g_context.args.port)) < 0)
	{
		return 1;
	}

	if(g_context.args.notty == 0)
	{
		if((g_context.outsock = connect_to(g_context.args.ip, g_context.args.port+2)) < 0)
		{
			fprintf(stderr, "Could not connect to stdout channel\n");
		}
		if((g_context.errsock = connect_to(g_context.args.ip, g_context.args.port+3)) < 0)
		{
			fprintf(stderr, "Could not connect to stderr channel\n");
		}
	}

	if(!g_context.args.script)
	{
		init_readline();
		read_history(g_context.history_file);
		history_set_pos(history_length);

		FD_SET(STDIN_FILENO, &g_context.readsave);
	}

	/* Change to the current directory, should return our path */
	execute_line("cd .");

	while(!g_context.exit)
	{
		int ret;

		readset = g_context.readsave;
		ret = select(FD_SETSIZE, &readset, NULL, NULL, NULL);
		if(ret < 0)
		{
			if(errno == EINTR)
			{
				continue;
			}

			perror("select");
			break;
		}
		else if(ret == 0)
		{
			continue;
		}
		else
		{
			if(!g_context.args.script)
			{
				if(FD_ISSET(STDIN_FILENO, &readset))
				{
					rl_callback_read_char();
				}
			}

			if(FD_ISSET(g_context.sock, &readset))
			{
				/* Do read */
				if(read_socket(g_context.sock) < 0)
				{
					close(g_context.sock);
					g_context.sock = -1;
					break;
				}
			}

			if((g_context.outsock >= 0) && FD_ISSET(g_context.outsock, &readset))
			{
				if(read_outsocket(g_context.outsock) < 0)
				{
					FD_CLR(g_context.outsock, &g_context.readsave);
					close(g_context.outsock);
					g_context.outsock = -1;
				}
			}
			if((g_context.errsock >= 0) && FD_ISSET(g_context.errsock, &readset))
			{
				if(read_errsocket(g_context.errsock) < 0)
				{
					FD_CLR(g_context.errsock, &g_context.readsave);
					close(g_context.errsock);
					g_context.errsock = -1;
				}
			}
		}
	}

	if(!g_context.args.script)
	{
		write_history(g_context.history_file);
		rl_callback_handler_remove();
	}

	return 0;
}

void sig_call(int sig)
{
	if((sig == SIGINT) || (sig == SIGTERM))
	{
		fprintf(stderr, "Exiting\n");
		if(g_context.sock >= 0)
		{
			close(g_context.sock);
			g_context.sock = -1;
		}
		if(g_context.outsock >= 0)
		{
			close(g_context.outsock);
			g_context.outsock = -1;
		}
		if(g_context.errsock >= 0)
		{
			close(g_context.errsock);
			g_context.errsock = -1;
		}

		if(!g_context.args.script)
		{
			rl_callback_handler_remove();
		}

		exit(1);
	}
}

void build_histfile(void)
{
	if(g_context.args.hist == NULL)
	{
		char *home;

		home = getenv("HOME");
		if(home == NULL)
		{
			snprintf(g_context.history_file, PATH_MAX, "%s", HISTORY_FILE);
		}
		else
		{
			snprintf(g_context.history_file, PATH_MAX, "%s/%s", home, HISTORY_FILE);
		}
	}
	else
	{
		snprintf(g_context.history_file, PATH_MAX, "%s", g_context.args.hist);
	}
}

int main(int argc, char **argv)
{
	int ret = 1;

	memset(&g_context, 0, sizeof(g_context));
	g_context.sock = -1;
	g_context.outsock = -1;
	g_context.errsock = -1;
	g_context.fstdout = stdout;
	g_context.fstderr = stderr;
	if(parse_args(argc, argv, &g_context.args))
	{
		build_histfile();
		if(shell() == 0)
		{
			ret = g_context.lasterr;
		}
		if(g_context.sock >= 0)
		{
			close(g_context.sock);
		}
		if(g_context.outsock >= 0)
		{
			close(g_context.outsock);
		}
		if(g_context.errsock >= 0)
		{
			close(g_context.errsock);
		}
	}
	else
	{
		print_help();
	}

	return ret;
}
