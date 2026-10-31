// Divinity II Reborn AO settings, shared by every light pre-pass replacement that applies AO
// (sun, sun + terrain shadow, hemisphere), so all areas of the game look the same.

#define AO_ENABLED     1
#define AO_SAMPLES     16      // samples per pixel (no separate blur pass: the rotation noise is per pixel)
#define AO_RADIUS      1.0     // world units (about metres: the default camera distance is 3.75)
#define AO_MAX_PIXELS  160.0   // radius cap on screen, close to the camera
#define AO_BIAS        0.15    // cosine below which a neighbour does not occlude (avoids self-occlusion)
#define AO_STRENGTH    1.8
#define AO_BACKLIGHT   0.5     // share of AO applied to the sun pass fill (back) light
#define AO_FADE_START  60.0    // world units: AO fades out with distance, where the 16-bit depth gets coarse
#define AO_FADE_LENGTH 40.0
