#version 450
layout(set=1,binding=0) uniform sampler2D atlas;
layout(location=0) in vec2 vTileUv;
layout(location=2) in vec4 vAtlasRect;
layout(location=3) flat in uint vPackedLight;
layout(location=1) in vec3 vNormal;
layout(location=0) out vec4 outColor;
void main(){
    // Explicit gradients avoid the implicit-derivative discontinuity at tile
    // repeats; mipmaps were built independently per atlas tile.
    vec2 scale=vAtlasRect.zw-vAtlasRect.xy;
    vec2 tiled=fract(vTileUv);
    vec2 texelHalf=0.5/vec2(textureSize(atlas,0));
    vec2 uv=clamp(vAtlasRect.xy+tiled*scale,
                  min(vAtlasRect.xy,vAtlasRect.zw)+texelHalf,
                  max(vAtlasRect.xy,vAtlasRect.zw)-texelHalf);
    vec4 texel=textureGrad(atlas,uv,
                           dFdx(vTileUv)*scale,dFdy(vTileUv)*scale);
    float sky=float((vPackedLight>>4u)&15u)/15.0;
    float block=float(vPackedLight&15u)/15.0;
    // Direct sky and propagated block light, with a small safety floor while
    // the game streams its light grid. No GI or dynamic shadow casting yet.
    float brightness=max(max(sky,block),0.20);
    float shade=(0.68+0.32*abs(vNormal.y))*brightness;
    outColor=vec4(texel.rgb*shade,1.0);
}
