#include "safearea.h"

#include "am2r.h"
#include "instance.h"
#include "runner.h"
#include "rvalue.h"
#include "stb_ds.h"
#include "vm.h"

static Instance *first_of(Runner *runner, int32_t obj)
{
	Instance **list;

	if (obj < 0 || !runner->instancesByExactObject || !(list = runner->instancesByExactObject[obj]) || !arrlen(list))
		return NULL;
	return list[0];
}

/* The surface oControl paints the HUD into every step (1.5.5), or -1. */
int32_t WiiSafe_guiSurface(Runner *runner)
{
	static int32_t obj = -2, var = -2;
	Instance *control;
	RValue v;

	if (!runner)
		return -1;
	if (obj == -2) {
		obj = Wii_objectIndex(runner, "oControl");
		var = Wii_varId(runner, "gui_surface", INSTANCE_SELF);
	}
	if (var < 0 || !(control = first_of(runner, obj)))
		return -1;
	v = Instance_getSelfVar(control, var);
	return v.type == RVALUE_UNDEFINED ? -1 : RValue_toInt32(v);
}
