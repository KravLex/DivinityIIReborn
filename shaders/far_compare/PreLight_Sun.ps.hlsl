// replaces: e6bef6f7376173eb73df5390447a1067939c5e8a ps_3_0  div2_PreLightPass.fx / div2_PreLightPass_Sun_Packed_ViewSpace_VSM_Char / p0
// preshader-registers: c39
// Set "far" for comparison screenshots: F11 "SMAA + TAA, original lighting" (d3d9.postAADebugViews) switches to the
// engine's hard cascade switch and shadow distance. Not shipped.
#define TERRAIN 0
#define AO_ENABLED 0
#define SHADOW_FAR 1
#define SHADOW_FAR_COMPARE 1
#include "prelight_sun.hlsli"
