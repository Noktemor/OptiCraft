#version 430 core
// Фрагментный шейдер блоков формы (u_kind 1), флоры (2) и жидкости (3) GPU-мешера. Освещение — то же,
// что в terrain_gpu_fragment.glsl / flora_fragment.glsl / liquid_fragment.glsl; меняешь одно — меняй все.
out vec4 FragColor;

in vec2 Local;
in vec2 TexCoord;
in float NormalY;
in vec3 FragPos;
in vec3 WorldNormal;
in float BlockLight;
in float SkyLight;
in float Appearance;
flat in vec4 Rect;

uniform sampler2D textureAtlas;
uniform float ambient;
uniform float maxLightLevel;
uniform float time;
uniform vec3 cameraPosition;
uniform vec3 fogColor;
uniform vec2 fogRange;
uniform vec3 sunDirection;
uniform int u_kind;
// Per-face atlas rectangle comes from the packed GPU quad; no global water sprite uniforms.
// The liquid kind is encoded in bit 15 of rec.y (0 = water, 1 = lava).
flat in uint LiquidIsLava;

float appearance_hash(vec3 cell_position) {
    vec3 p = fract(cell_position * vec3(0.1031, 0.11369, 0.13787));
    p += dot(p, p.yzx + 19.19);
    return fract((p.x + p.y) * p.z);
}

void main() {
    vec3 block_cell = floor(FragPos - WorldNormal * 0.001);
    // Crossed flora has no meaningful face normal; offset its whole plant into one voxel cell.
    if (u_kind == 2) block_cell = floor(FragPos - vec3(0.0, 0.5, 0.0));
    if (appearance_hash(block_cell) > Appearance) discard;

    float light_intensity = 0.7;
    if (NormalY > 0.9) light_intensity = 1.0;
    else if (NormalY < -0.9) light_intensity = 0.5;

    vec4 tex_color;
    float shimmer = 1.0;
    float alpha_scale = 1.0;
    if (u_kind == 1) {
        const vec2 f = clamp(fract(Local), 0.0005, 0.9995);
        tex_color = texture(textureAtlas, mix(Rect.xy, Rect.zw, f));
    } else if (u_kind == 2) {
        tex_color = texture(textureAtlas, TexCoord);
        if (tex_color.a < 0.1) discard;
    } else {
        // Each liquid quad already stores its own UV rect; animate within THAT tile.
        // Using a single water tile here previously painted lava faces with water.
        const bool lava = LiquidIsLava != 0u;
        const vec2 flow = lava ? vec2(time * 0.045, time * 0.025)
                               : vec2(sin(time * 0.15) * 0.15, time * 0.35);
        const vec2 uv = clamp(fract(Local + flow), vec2(0.001), vec2(0.999));
        tex_color = texture(textureAtlas, mix(Rect.xy, Rect.zw, uv));
        // Lava is opaque and glows subtly; water remains translucent.
        shimmer = lava ? (0.97 + 0.03 * sin(FragPos.x + FragPos.z + time * 0.8))
                       : (0.985 + 0.015 * sin(FragPos.x * 1.3 + FragPos.z * 1.1 + time * 0.7));
        alpha_scale = lava ? 1.0 : 0.78;
    }

    const float sky_scene_light = (SkyLight / 15.0) * ambient;
    const float block_scene_light = BlockLight / maxLightLevel;
    const float sun_term = max(dot(normalize(WorldNormal), normalize(sunDirection)), 0.0);
    // Directional sunlight is strongest above the horizon and fades smoothly at dawn/dusk.
    // At night the existing ambient/block-light model remains unchanged. The 0.32 scene floor
    // is intentional and must stay stable while this directional contrast is added.
    const float sun_strength = smoothstep(-0.12, 0.10, sunDirection.y);
    const float direct_light = mix(0.80, 1.08, sun_term);
    const float directional_light = mix(1.0, direct_light, sun_strength);
    const float scene_light = max(max(sky_scene_light, block_scene_light) * directional_light, 0.32);
    const vec3 lit_color = tex_color.rgb * light_intensity * shimmer * scene_light;
    const float fog_amount = smoothstep(fogRange.x, fogRange.y, distance(FragPos, cameraPosition));
    FragColor = vec4(mix(lit_color, fogColor, fog_amount), tex_color.a * alpha_scale);
}
