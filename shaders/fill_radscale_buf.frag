/****************************************************************************
* Radiance Scaling — gradient buffer pass (pass 1 of 2)                    *
*                                                                           *
* Faithful port of 01_buffer.fs (Vergne & Dumas, INRIA 2010).              *
* Renders (gx, gy, logZ, 1.0) into an RGBA32F intermediate texture.        *
* Fragments not covered by mesh geometry remain cleared to vec4(0).        *
*                                                                           *
* Pass 2 -- the radiance-scaling option of fill_smooth.frag and             *
* fill_flat.frag -- reads this texture with a 3x3 neighbourhood to compute  *
* the Hessian and depth-discontinuity weight exactly as in the original     *
* two-pass implementation.                                                  *
****************************************************************************/
#version 440

layout(std140, binding = 0) uniform buf {
    mat4 mvp;
    mat4 modelView;
    mat3 normalMatrix;
    vec4 bboxColor;
    vec4 pointColor;
    vec4 pointParams;
    vec4 wireColor;
    vec4 wireParams;
    vec4 fillColor;
    vec4 lightingParams;
    vec4 edgeColor;
    vec4 materialFlags;  // z=1.0 means flat shading (face normal via dFdx/dFdy)
    vec4 materialParams;
    vec4 lightDir;        // view-space light direction (w unused)
} ub;

layout(location = 0) in vec3 v_normal;
layout(location = 1) in vec4 v_meshColor;  // unused; present for vertex layout compat
layout(location = 2) in vec3 v_texInfo;    // unused
layout(location = 3) in vec3 v_viewPos;

layout(location = 0) out vec4 fragColor;   // (gx, gy, logZ, 1.0)

void main()
{
    // Exact port of 01_buffer.fs:
    //   gs  = pow(1 / max(n.z, eps), foreshortening)
    //   gx  = -n.x * gs
    //   gy  = -n.y * gs
    //   logZ = log(-view.z)      (matches 01_buffer.vs: depth = log(-z))
    const float eps           = 0.01;
    const float foreshortening = 0.4;

    // The normal the layer is shaded with. Smooth: the interpolated per-vertex one. Flat
    // (materialParams.y > 0.5, set by the pass for a flat-shaded layer): the face's own, from the
    // derivatives of the view position, so the curvature is zero inside a face and high across
    // the edges between faces -- the convex ones read as bright lines, which is what radiance
    // scaling is expected to do to a faceted layer. (From the smooth normals instead, the
    // curvature spreads over every face and whitens it towards the silhouette.)
    vec3 n;
    if (ub.materialParams.y > 0.5) {
        n = normalize(cross(dFdx(v_viewPos), dFdy(v_viewPos)));
        // Facing the viewer: positive z in view space.
        if (n.z < 0.0)
            n = -n;
    } else {
        n = normalize(v_normal);
    }

    float gs = pow(max(n.z, eps), -foreshortening);

    float gx   = -n.x * gs;
    float gy   = -n.y * gs;
    float logZ = log(max(-v_viewPos.z, 0.001));

    fragColor = vec4(gx, gy, logZ, 1.0);
}
