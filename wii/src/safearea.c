#include "safearea.h"

#include "am2r.h"
#include "instance.h"
#include "runner.h"
#include "rvalue.h"
#include "stb_ds.h"
#include "vm.h"

#define NOTIFY_HIDDEN 30 /* oNotification's offset when it is fully slid out */

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

/* How far up to draw what the current instance draws. The notification box lifts as it slides in, so a box that is
 * slid out stays below the screen. */
float WiiSafe_lift(Runner *runner)
{
	static int32_t notify = -2, options, popup, offset;
	Instance *inst = runner && runner->vmContext ? runner->vmContext->currentInstance : NULL;
	float off;

	if (!inst)
		return 0.0f;
	if (notify == -2) {
		notify = Wii_objectIndex(runner, "oNotification");
		options = Wii_objectIndex(runner, "oOptionsFG");
		popup = Wii_objectIndex(runner, "oPopupText");
		offset = Wii_varId(runner, "offset", INSTANCE_SELF);
	}
	if (inst->objectIndex == options || inst->objectIndex == popup)
		return -WII_SAFE_ROWS;
	if (inst->objectIndex != notify || offset < 0)
		return 0.0f;
	off = (float)RValue_toReal(Instance_getSelfVar(inst, offset));
	if (off < 0.0f)
		off = 0.0f;
	if (off > NOTIFY_HIDDEN)
		off = NOTIFY_HIDDEN;
	return -WII_SAFE_ROWS * (NOTIFY_HIDDEN - off) / NOTIFY_HIDDEN;
}
