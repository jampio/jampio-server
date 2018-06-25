#include <jampio/common/com.h>
#include <cstdio>

int main(int argc, const char **argv) {
	CvarSystem cvars;
	CommandSystem cmd(cvars);
	CommandBuffer cbuf(cmd);
	CommandLine cli(cbuf, cvars, argc, argv);
	Com_Init(cvars, cli, cbuf, cmd);
	while (1) {
		Com_Frame(cvars, cbuf);
	}
	return EXIT_SUCCESS;
}