

int printk(char *fmt, ...);

int stdioTtyInit(void);
int stdioInstallStdinHandler(PspDebugInputHandler handler);
int stdioInstallStdoutHandler(PspDebugPrintHandler handler);
int stdioInstallStderrHandler(PspDebugPrintHandler handler);

int print_modinfo(SceUID uid, int verbose);
int modlist_cmd(void);

int parse_cmd(char *cmdbuf);

