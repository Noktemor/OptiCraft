// Camera-local moving light; no mesh rebuild or propagation. No shadow rays.
vec3 dynamicLight(vec3 p,vec3 n,vec3 eye,vec4 light){
    if(light.w<=0.0)return vec3(0.0);
    vec3 delta=eye-p;
    float d=length(delta);
    if(d>=light.w)return vec3(0.0);
    float attenuation=1.0-d/light.w;
    float diffuse=0.25+0.75*max(dot(n,delta/max(d,0.001)),0.0);
    return light.rgb*attenuation*diffuse*min(light.w/15.0,1.0);
}
