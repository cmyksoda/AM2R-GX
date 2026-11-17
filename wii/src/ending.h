#ifndef AM2R_WII_ENDING_H
#define AM2R_WII_ENDING_H

#include "vm.h"

/* physics_apply_local_impulse for the secret ending's hornoad, AM2R's only Box2D body. */
BuiltinFunc WiiEnding_impulseBuiltin(void);

/* Steps the hornoad's body after its End Step. */
void WiiEnding_hook(VMContext *ctx);

#endif
