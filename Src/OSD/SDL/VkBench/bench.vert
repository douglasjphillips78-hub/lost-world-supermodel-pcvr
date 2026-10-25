#version 450
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNrm;
layout(location = 2) in vec2 aUV;
layout(location = 3) in vec4 aCol;
layout(push_constant) uniform PC { mat4 mvp; vec4 tint; vec4 params; } pc;
layout(location = 0) out vec2 vUV;
layout(location = 1) out vec4 vCol;
void main()
{
    gl_Position = pc.mvp * vec4(aPos, 1.0);
    float shade = max(dot(normalize(aNrm), normalize(pc.params.xyz)), 0.0) * 0.7 + 0.3;
    vUV = aUV;
    vCol = vec4(aCol.rgb * shade, aCol.a);
}
