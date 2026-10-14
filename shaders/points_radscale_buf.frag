/****************************************************************************
* Radiance Scaling — gradient buffer pass for points                       *
*                                                                           *
* The points counterpart of fill_radscale_buf.frag: renders the same        *
* (gx, gy, logZ, 1.0) into the same buffer, from the point's own normal, so *
* that a point cloud is read by the radiance-scaling option of              *
* overlay_points.frag exactly as a surface is by the fill shaders. Run with *
* overlay_points.vert.                                                      *
****************************************************************************/
#version 440

layout(location = 0) in vec4 vMeshColor;      // unused; present for vertex stage compat
layout(location = 1) in vec4 vNormalAndFlag;  // view-space normal, a > 0.5 when the point has one
layout(location = 2) in vec3 vViewPos;

layout(location = 0) out vec4 fragColor;      // (gx, gy, logZ, 1.0)

void main()
{
    // The same round dot overlay_points.frag draws.
    vec2 coord = gl_PointCoord - vec2(0.5);
    if (dot(coord, coord) > 0.25)
        discard;

    // A point without a normal is a flat patch facing the viewer: it occludes, and has no
    // curvature of its own. Normals are two-sided, as in the lighting of the points.
    vec3 n = vec3(0.0, 0.0, 1.0);
    if (vNormalAndFlag.a > 0.5 && length(vNormalAndFlag.xyz) > 1e-6) {
        n = normalize(vNormalAndFlag.xyz);
        if (n.z < 0.0)
            n = -n;
    }

    // Same encoding as the fill pass.
    const float eps            = 0.01;
    const float foreshortening = 0.4;
    float gs = pow(max(n.z, eps), -foreshortening);
    fragColor = vec4(-n.x * gs, -n.y * gs, log(max(-vViewPos.z, 0.001)), 1.0);
}
