#include "controls.h"

#include "am2r.h"
#include "gml_array.h"
#include "instance.h"
#include "log.h"
#include "runner.h"
#include "rvalue.h"
#include "spatial_grid.h"
#include "stb_ds.h"
#include "vm.h"

/* AM2R's Control Options rows (oOptionsControl): 6 is Keyboard Settings, 7 and 9 are Android-only and already hidden. */
#define ROW_KEYBOARD 6
#define ROW_EXIT 10

/* Looked up by scope: canedit, for one, is also a global somewhere in the data, listed first. */
static int32_t var_id(Runner *runner, const char *name, int32_t scope)
{
	static struct {
		const char *name;
		int32_t scope, id;
	} cache[12];
	static unsigned used;
	unsigned k;

	for (k = 0; k < used; k++)
		if (cache[k].name == name && cache[k].scope == scope)
			return cache[k].id;
	if (used == sizeof cache / sizeof cache[0])
		return Wii_varId(runner, name, scope);
	cache[used].name = name;
	cache[used].scope = scope;
	cache[used].id = Wii_varId(runner, name, scope);
	return cache[used++].id;
}

static Instance *first_instance(Runner *runner, const char *object)
{
	int32_t idx = Wii_objectIndex(runner, object);

	if (idx < 0 || !runner->instancesByExactObject || arrlen(runner->instancesByExactObject[idx]) == 0)
		return NULL;
	return runner->instancesByExactObject[idx][0];
}

static RValue *array_slot(Runner *runner, Instance *inst, const char *name, int32_t i)
{
	int32_t id = var_id(runner, name, INSTANCE_SELF);
	RValue v = id >= 0 ? Instance_getSelfVar(inst, id) : (RValue){ .type = RVALUE_UNDEFINED };

	return v.type == RVALUE_ARRAY ? GMLArray_slot(v.array, i) : NULL;
}

static Instance *op_row(Runner *runner, Instance *menu, int32_t i)
{
	RValue *v = array_slot(runner, menu, "op", i);
	int key;

	if (!v)
		return NULL;
	key = RValue_toInt32(*v);
	return hmget(runner->instancesById, key);
}

static double var_num(Runner *runner, Instance *inst, const char *name, int32_t scope, double fallback)
{
	int32_t id = inst ? var_id(runner, name, scope) : -1;
	RValue v = id >= 0 ? Instance_getSelfVar(inst, id) : (RValue){ .type = RVALUE_UNDEFINED };

	return v.type == RVALUE_UNDEFINED ? fallback : RValue_toReal(v);
}

/* Hidden the way the game hides its Android rows (invisible, never selected); the rows below move up a line. */
static void hide_keyboard_row(Runner *runner, Instance *menu)
{
	RValue *edit = array_slot(runner, menu, "canedit", ROW_KEYBOARD);
	float sep = (float)var_num(runner, menu, "sep", INSTANCE_SELF, 16);
	Instance *row;
	int32_t i;

	if (!edit)
		return;
	RValue_free(edit);
	*edit = RValue_makeInt32(0);
	if ((row = op_row(runner, menu, ROW_KEYBOARD)))
		row->visible = false;
	for (i = ROW_KEYBOARD + 1; i <= ROW_EXIT; i++)
		if ((row = op_row(runner, menu, i))) {
			row->y -= sep;
			if (runner->spatialGrid)
				SpatialGrid_markInstanceAsDirty(runner->spatialGrid, row);
		}
	logInfo("controls: Keyboard Settings hidden\n");
}

void WiiControls_frame(Runner *runner)
{
	static uint32_t controlMenu;
	Instance *menu;

	if (!runner || !runner->vmContext)
		return;
	if ((menu = first_instance(runner, "oOptionsControl")) && menu->instanceId != controlMenu) {
		controlMenu = menu->instanceId;
		hide_keyboard_row(runner, menu);
	}
}
