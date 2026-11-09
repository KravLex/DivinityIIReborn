// replaces: e6bef6f7376173eb73df5390447a1067939c5e8a ps_3_0  div2_PreLightPass.fx / div2_PreLightPass_Sun_Packed_ViewSpace_VSM_Char / p0
// Exact rewrite of the engine's sun + ambient light pre-pass (common/prelight_sun.hlsli).
#define TERRAIN 0
#define AO_ENABLED 0
#include "prelight_sun.hlsli"
