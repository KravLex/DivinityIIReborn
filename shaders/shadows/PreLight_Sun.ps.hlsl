// replaces: e6bef6f7376173eb73df5390447a1067939c5e8a ps_3_0  div2_PreLightPass.fx / div2_PreLightPass_Sun_Packed_ViewSpace_VSM_Char / p0
// preshader-registers: c39
// Divinity II Reborn NG-6: sun + ambient light pre-pass with AO and the shadows of common/shadows.hlsli.
#define TERRAIN 0
#define SHADOW_QUALITY 1
#include "ao_settings.hlsli"
#include "prelight_sun.hlsli"
