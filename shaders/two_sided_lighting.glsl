// The diffuse term of the fill under the three ways the old MeshLab could light the back of a
// surface, chosen by lightingParams.w (the fill's lighting switch: 0 off, then 1 + the mode).
// Shared by fill_smooth.frag and fill_flat.frag, which declare the uniform block `ub` first.
//
//   Single  only the light: a surface turned away from it gets the ambient and nothing else.
//   Double  the light and one opposite to it, alike: a surface is lit from whichever side it faces.
//   Fancy   the same two lights in two tones, so that the side of a surface can be read from its
//           colour: bluish where the light reaches it, pinkish where only the opposite one does.
//           The old defaults, 204,204,255 and 255,204,204.
const vec3 kFancyFrontTint = vec3(0.8, 0.8, 1.0);
const vec3 kFancyBackTint  = vec3(1.0, 0.8, 0.8);

// ndl is dot(N, L). Returns the diffuse intensity, and in `tint` the colour of the light it comes
// from, white unless fancy.
float sidedDiffuse(float ndl, out vec3 tint)
{
    int mode = int(ub.lightingParams.w + 0.5) - 1;
    tint = (mode >= 2) ? (ndl >= 0.0 ? kFancyFrontTint : kFancyBackTint) : vec3(1.0);
    return (mode >= 1) ? abs(ndl) : max(ndl, 0.0);
}
