// replaces: d50c5e8e8201ae4165e7edeca1b991f8cf1c16bf ps_3_0  div2_PreLightPass.fx / div2_PreLightPass_Sun_Packed_ViewSpace_VSM_Char_Terrain / p0
// preshader-registers: c42
// The engine's sun + ambient light pre-pass with terrain shadow (common/prelight_sun.hlsli) with the cascades blended
// and the shadow distance extended (SHADOW_FAR): no other change to the look.
#define TERRAIN 1
#define AO_ENABLED 0
#define SHADOW_FAR 1
#include "prelight_sun.hlsli"
