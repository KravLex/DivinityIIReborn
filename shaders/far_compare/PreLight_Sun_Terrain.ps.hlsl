// replaces: d50c5e8e8201ae4165e7edeca1b991f8cf1c16bf ps_3_0  div2_PreLightPass.fx / div2_PreLightPass_Sun_Packed_ViewSpace_VSM_Char_Terrain / p0
// preshader-registers: c39 c42
// Set "far" for comparison screenshots: F11 "SMAA + TAA, original lighting" (d3d9.postAADebugViews) switches to the
// engine's hard cascade switch and shadow distance. Not shipped.
#define TERRAIN 1
#define AO_ENABLED 0
#define SHADOW_FAR 1
#define SHADOW_FAR_COMPARE 1
#include "prelight_sun.hlsli"
