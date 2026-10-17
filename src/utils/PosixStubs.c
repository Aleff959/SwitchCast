/* POSIX stubs exigidos pelo civetweb no libnx/newlib.
 *
 * O Horizon OS não possui sinais POSIX, usuários/grupos nem popen(). O civetweb
 * referencia essas funções em caminhos que o SwitchCast nunca executa (opção
 * `run_as_user`, SSI com #exec, instalação de handler de SIGPIPE), mas o linker
 * ainda precisa resolvê-las. Todas retornam "não suportado". */
#include <grp.h>
#include <pwd.h>
#include <signal.h>
#include <stdio.h>
#include <sys/types.h>
#include <unistd.h>

int sigaction(int sig, const struct sigaction* act, struct sigaction* old) {
    (void)sig; (void)act; (void)old;
    return 0;  // ignorar: sockets do Horizon não geram SIGPIPE
}

FILE* popen(const char* cmd, const char* mode) { (void)cmd; (void)mode; return NULL; }
int pclose(FILE* f) { (void)f; return -1; }

uid_t getuid(void) { return 0; }
struct passwd* getpwnam(const char* name) { (void)name; return NULL; }
int setuid(uid_t uid) { (void)uid; return -1; }
int setgid(gid_t gid) { (void)gid; return -1; }
int setgroups(int n, const gid_t* list) { (void)n; (void)list; return -1; }
