/* The 100% secret ending (rm_secretEnding): the hornoad's Box2D body. */
#include "ending.h"

#include "am2r.h"
#include "data_win.h"
#include "instance.h"
#include "runner.h"
#include "rvalue.h"
#include "spatial_grid.h"
#include "stb_ds.h"
#include "vm.h"

#include <math.h>
#include <stdbool.h>

#define MAX_SOLIDS 16
#define CONTACT_EPS 0.01f
#define RESTITUTION_THRESHOLD 1.0f /* m/s, Box2D's b2_velocityThreshold */

typedef struct {
	float l, t, r, b;
} Box;

typedef struct {
	Box box;
	float friction, restitution;
} Solid;

/* AM2R's only physics bodies: one dynamic hornoad with fixed rotation on static boxes, so axis-aligned boxes are
 * all Box2D has to do here. Positions in pixels, velocities in pixels per second. */
static struct {
	uint32_t id;
	int32_t room;
	float vx, vy, invMass;
	Box shape;
	float friction, restitution, damping;
} hornoad = { .room = -1 };

static Solid solids[MAX_SOLIDS];
static int solidCount;
static int32_t hornoadEndStepCode = -1;

static Box object_box(const GameObject *obj, float x, float y, float sx, float sy)
{
	Box b = { INFINITY, INFINITY, -INFINITY, -INFINITY };

	for (int32_t i = 0; i < obj->physicsVertexCount; i++) {
		float vx = x + obj->physicsVertices[i].x * sx, vy = y + obj->physicsVertices[i].y * sy;

		b.l = fminf(b.l, vx);
		b.r = fmaxf(b.r, vx);
		b.t = fminf(b.t, vy);
		b.b = fmaxf(b.b, vy);
	}
	return b;
}

static void bodies_for_room(Runner *runner, Instance *self)
{
	const GameObject *obj = &runner->dataWin->objt.objects[self->objectIndex];
	float mpp = runner->currentRoom->metersPerPixel;

	hornoad.id = self->instanceId;
	hornoad.room = runner->currentRoomIndex;
	hornoad.vx = hornoad.vy = 0.0f;
	hornoad.shape = object_box(obj, 0.0f, 0.0f, self->imageXscale, self->imageYscale);
	hornoad.invMass = 1.0f / (obj->density * (hornoad.shape.r - hornoad.shape.l) * (hornoad.shape.b - hornoad.shape.t) *
				  mpp * mpp * mpp);
	hornoad.friction = obj->friction;
	hornoad.restitution = obj->restitution;
	hornoad.damping = obj->linearDamping;
	solidCount = 0;
	for (int32_t i = 0; i < (int32_t)arrlen(runner->instances) && solidCount < MAX_SOLIDS; i++) {
		Instance *inst = runner->instances[i];
		const GameObject *o = &runner->dataWin->objt.objects[inst->objectIndex];

		if (inst->destroyed || !o->usesPhysics || o->density != 0.0f || o->physicsVertexCount < 3)
			continue;
		solids[solidCount].box = object_box(o, inst->x, inst->y, inst->imageXscale, inst->imageYscale);
		solids[solidCount].friction = o->friction;
		solids[solidCount].restitution = o->restitution;
		solidCount++;
	}
}

static bool is_hornoad(Runner *runner, Instance *self)
{
	if (!self || !runner->currentRoom || !runner->currentRoom->world)
		return false;
	if (hornoad.id != self->instanceId || hornoad.room != runner->currentRoomIndex)
		bodies_for_room(runner, self);
	return true;
}

/* With fixed rotation the point of application changes nothing, and the local frame is the world frame. */
static RValue wii_physics_apply_local_impulse(VMContext *ctx, RValue *args, int32_t argCount)
{
	if (argCount >= 4 && is_hornoad(ctx->runner, ctx->currentInstance)) {
		hornoad.vx += (float)RValue_toReal(args[2]) * hornoad.invMass;
		hornoad.vy += (float)RValue_toReal(args[3]) * hornoad.invMass;
	}
	return RValue_makeUndefined();
}

/* One Box2D-style step: gravity and damping, then landing (restitution, and friction bounded by the normal impulse),
 * then walls. */
static void hornoad_step(Runner *runner, Instance *self)
{
	Room *room = runner->currentRoom;
	float mpp = room->metersPerPixel, dt = 1.0f / (float)(room->speed ? room->speed : 60);
	float damp = 1.0f / (1.0f + dt * hornoad.damping), x = self->x, y = self->y, nx, ny, top = INFINITY;
	const Box *s = &hornoad.shape;
	const Solid *floor = NULL;

	hornoad.vx = (hornoad.vx + room->gravityX / mpp * dt) * damp;
	hornoad.vy = (hornoad.vy + room->gravityY / mpp * dt) * damp;
	ny = y + hornoad.vy * dt;
	for (int i = 0; i < solidCount; i++) {
		const Box *b = &solids[i].box;

		if (x + s->l >= b->r || x + s->r <= b->l)
			continue;
		if (hornoad.vy > 0.0f && y + s->b <= b->t + CONTACT_EPS && ny + s->b >= b->t && b->t < top) {
			top = b->t;
			floor = &solids[i];
		} else if (hornoad.vy < 0.0f && y + s->t >= b->b - CONTACT_EPS && ny + s->t <= b->b) {
			ny = b->b - s->t;
			hornoad.vy = 0.0f;
		}
	}
	if (floor) {
		float e = fmaxf(hornoad.restitution, floor->restitution), mu = sqrtf(hornoad.friction * floor->friction);
		float vn = hornoad.vy * mpp > RESTITUTION_THRESHOLD ? -e * hornoad.vy : 0.0f;
		float slow = mu * (hornoad.vy - vn);

		ny = top - s->b;
		hornoad.vy = vn;
		hornoad.vx = hornoad.vx > 0.0f ? fmaxf(0.0f, hornoad.vx - slow) : fminf(0.0f, hornoad.vx + slow);
	}
	nx = x + hornoad.vx * dt;
	for (int i = 0; i < solidCount; i++) {
		const Box *b = &solids[i].box;

		if (ny + s->t >= b->b || ny + s->b <= b->t + CONTACT_EPS)
			continue;
		if (hornoad.vx > 0.0f && x + s->r <= b->l + CONTACT_EPS && nx + s->r > b->l) {
			nx = b->l - s->r;
			hornoad.vx = 0.0f;
		} else if (hornoad.vx < 0.0f && x + s->l >= b->r - CONTACT_EPS && nx + s->l < b->r) {
			nx = b->r - s->l;
			hornoad.vx = 0.0f;
		}
	}
	if (nx != self->x || ny != self->y) {
		self->x = nx;
		self->y = ny;
		SpatialGrid_markInstanceAsDirty(runner->spatialGrid, self);
	}
}

/* oEndingHornoad's End Step applies the jump impulses; the world steps right after, once a frame. */
static RValue wii_hornoad_end_step(VMContext *ctx)
{
	RValue r = VM_interpretCode(ctx, hornoadEndStepCode);

	if (is_hornoad(ctx->runner, ctx->currentInstance))
		hornoad_step(ctx->runner, ctx->currentInstance);
	return r;
}

static int32_t code_index(VMContext *ctx, const char *name)
{
	ptrdiff_t k = shgeti(ctx->codeIndexByName, name);

	return k >= 0 ? ctx->codeIndexByName[k].value : -1;
}

BuiltinFunc WiiEnding_impulseBuiltin(void)
{
	return wii_physics_apply_local_impulse;
}

void WiiEnding_hook(VMContext *ctx)
{
	hornoadEndStepCode = code_index(ctx, "gml_Object_oEndingHornoad_Step_2");
	if (hornoadEndStepCode >= 0)
		VM_setNativeCode(ctx, hornoadEndStepCode, wii_hornoad_end_step);
}
