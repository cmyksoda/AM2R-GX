#include "am2r.h"

#include "instance.h"
#include "runner.h"
#include "rvalue.h"
#include "stb_ds.h"
#include "vm.h"

#include <string.h>

int32_t Wii_objectIndex(Runner *runner, const char *name)
{
	DataWin *dw = runner ? runner->dataWin : NULL;
	uint32_t i;

	for (i = 0; dw && i < dw->objt.count; i++)
		if (strcmp(dw->objt.objects[i].name, name) == 0)
			return (int32_t)i;
	return -1;
}

int32_t Wii_spriteIndex(Runner *runner, const char *name)
{
	DataWin *dw = runner ? runner->dataWin : NULL;
	uint32_t i;

	for (i = 0; dw && i < dw->sprt.count; i++)
		if (dw->sprt.sprites[i].name && strcmp(dw->sprt.sprites[i].name, name) == 0)
			return (int32_t)i;
	return -1;
}

int32_t Wii_varId(Runner *runner, const char *name, int32_t scope)
{
	DataWin *dw = runner ? runner->dataWin : NULL;
	uint32_t i;

	for (i = 0; dw && i < dw->vari.variableCount; i++) {
		Variable *v = &dw->vari.variables[i];

		if (v->instanceType == scope && v->varID >= 0 && v->name && strcmp(v->name, name) == 0)
			return v->varID;
	}
	return -1;
}

/* global.controltype picks the prompt glyph set: 0 keyboard, 1 numbered pad, 2 named A/B pad.
 * check_keys resets it every step from the last input seen. Pin it to 2 after the step so prompts
 * stay the named-button set on a controller-only console. Gameplay unaffected. */
void Wii_forceGamepadProfile(Runner *runner)
{
	static int32_t varId = -2;
	VMContext *vm;

	if (!runner || !runner->vmContext)
		return;
	vm = runner->vmContext;
	if (varId == -2) {
		ptrdiff_t slot = shgeti(vm->varNameMap, "controltype");
		varId = slot >= 0 ? vm->varNameMap[slot].value : -1;
	}
	if (varId >= 0 && vm->globalScopeInstance)
		Instance_setSelfVar(vm->globalScopeInstance, varId, RValue_makeInt32(2));
}
