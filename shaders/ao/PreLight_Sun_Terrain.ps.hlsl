// replaces: d50c5e8e8201ae4165e7edeca1b991f8cf1c16bf ps_3_0  div2_PreLightPass.fx / div2_PreLightPass_Sun_Packed_ViewSpace_VSM_Char_Terrain / p0
// preshader-registers: c39 c42
// Divinity II Reborn AO: sun + ambient light pre-pass with terrain shadow and screen-space AO on the ambient term.
#define TERRAIN 1
#include "ao_settings.hlsli"
#include "prelight_sun.hlsli"
