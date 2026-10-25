#version 450
layout(set = 0, binding = 0) uniform sampler2D uTex;
layout(push_constant) uniform PC { mat4 mvp; vec4 tint; vec4 params; } pc;
layout(location = 0) in vec2 vUV;
layout(location = 1) in vec4 vCol;
layout(location = 0) out vec4 outCol;
void main()
{
    vec4 t = texture(uTex, vUV);
    vec3 c = mix(t.rgb * vCol.rgb, pc.tint.rgb, 0.2);
    outCol = vec4(c, pc.tint.a);
}
