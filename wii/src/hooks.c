#include "hooks.h"

#include <stddef.h>

__attribute__((weak)) void Wii_hooksBoot(int argc, char **argv) { (void)argc; (void)argv; }
__attribute__((weak)) const char *Wii_hooksTape(void) { return NULL; }
__attribute__((weak)) void Wii_hooksBeforeLoop(void) {}
__attribute__((weak)) void Wii_hooksExit(void) {}
__attribute__((weak)) void Wii_hooksFrame(struct Runner *runner) { (void)runner; }
__attribute__((weak)) bool Wii_hooksEvents(struct Runner *runner) { (void)runner; return false; }
__attribute__((weak)) void Wii_hooksBeforeCopy(void) {}
__attribute__((weak)) void Wii_hooksAfterPresent(void) {}
__attribute__((weak)) void Wii_hooksPad(struct Runner *runner, unsigned int *wheld, unsigned short *gcheld,
					 struct expansion_t *exp, signed char *sticks)
{
	(void)runner; (void)wheld; (void)gcheld; (void)exp; (void)sticks;
}
