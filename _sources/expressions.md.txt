(expressions)=

# Expressions

Many filters take a formula instead of a number. *Select Faces by Expression* asks for a
condition, *Compute Vertex Color by Expression* for a formula per color channel, *Compute
Vertex Coordinates by Expression* for a formula per axis. The formula is evaluated **once
for every element** (vertex, face or edge), and the properties of that element are
available to it as variables:

| to get | write |
|---|---|
| the faces that look up | `fnz > 0.7` |
| a red that grows with height | `255*(z-zmin)/zdim` |
| a surface rippled along X | `z + 0.1*sin(10*x)` |

Expressions are evaluated by [muParser](https://beltoforion.de/en/muparser/), version 2.3.5
when this page was written. It is not pinned, so a build with a different version could
differ. This page lists what is available on top of it, and what is not: the filter dialogs
only say "Expression for red output in range [0, 255]".

The examples, the function and operator table, and everything under *What to expect* are
run by the test suite, which fails if they stop being true. The variable tables were written
from the source and are not checked mechanically.

## Which filters take expressions

| Filter | Python name | Runs over | Writes | Only selected |
|---|---|---|---|---|
| Select Vertices by Expression | `select_vertices_by_expression` | vertex | vertex selection | no |
| Select Faces by Expression | `select_faces_by_expression` | face | face selection | no |
| Select Edges by Expression | `select_edges_by_expression` | edge | edge selection | no |
| Compute Vertex Coordinates by Expression | `compute_vertex_coordinates_by_expression` | vertex | position | yes |
| Compute Vertex Normals by Expression | `compute_vertex_normals_by_expression` | vertex | normal | yes |
| Compute Face Normals by Expression | `compute_face_normals_by_expression` | face | normal | yes |
| Compute Vertex Color by Expression | `compute_vertex_color_by_expression` | vertex | color | yes |
| Compute Face Color by Expression | `compute_face_color_by_expression` | face | color | yes |
| Compute Edge Color by Expression | `compute_edge_color_by_expression` | edge | color | yes |
| Compute Vertex Scalar by Expression | `compute_vertex_scalar_by_expression` | vertex | scalar | yes |
| Compute Face Scalar by Expression | `compute_face_scalar_by_expression` | face | scalar | yes |
| Compute Edge Scalar by Expression | `compute_edge_scalar_by_expression` | edge | scalar | yes |
| Parametrize per Vertex by Expression | `parametrize_per_vertex_by_expression` | vertex | texture coordinates | yes |
| Parametrize per Wedge by Expression | `parametrize_per_wedge_by_expression` | face | per-wedge texture coordinates | yes |
| Define Custom Vertex Scalar Attribute | `define_custom_vertex_scalar_attribute` | vertex | a named scalar attribute | no |
| Define Custom Face Scalar Attribute | `define_custom_face_scalar_attribute` | face | a named scalar attribute | no |
| Define Custom Vertex Point Attribute | `define_custom_vertex_point_attribute` | vertex | a named 3D point attribute | no |
| Define Custom Face Point Attribute | `define_custom_face_point_attribute` | face | a named 3D point attribute | no |
| Create Isosurface from Expression | `create_isosurface_from_expression` | sample point | a new mesh | no |
| Refine by User Expression | `refine_by_user_expression` | edge | new vertices and faces | no |

*Runs over* is the kind of element the formulas are evaluated on, and so which variables
they can read. *Only selected* says whether the filter has the **Only selected …** option
(see [Working on the selection](#working-on-the-selection)).

Two of the fields are not what their labels suggest:

- In *Compute Vertex Color by Expression* the red, green and blue formulas are labeled
  **X, Y and Z Function** (their Python names are `x`, `y`, `z`); the fourth is the alpha.
  *Compute Face Color* and *Compute Edge Color* label them Red, Green and Blue.
- *Compute Vertex Coordinates by Expression* has an **Alpha expression** field that does
  nothing; it is ignored.

## Writing an expression

**Numbers** are written `3`, `0.5`, `.5` or `1e-3`. **Names are case-sensitive**: `Z` is
not `z`.

**Operators**, from tightest to loosest: `^` (power), `*` `/`, `+` `-`, the comparisons
`<` `>` `<=` `>=` `==` `!=`, `&&`, `||`, and the conditional `condition ? a : b`.
Parentheses group as usual. `^` binds tighter than a minus sign, so `-2^2` is `-4`.

A comparison gives `1` or `0`. In a **selection** filter any non-zero result selects the
element.

### Functions

Angles are in radians.

| | |
|---|---|
| trigonometry | `sin` `cos` `tan` `asin` `acos` `atan` `atan2(y, x)` |
| hyperbolic | `sinh` `cosh` `tanh` `asinh` `acosh` `atanh` |
| exponential | `exp` `sqrt` `log` (natural) `ln` (natural) `log2` `log10` |
| sign and rounding | `abs` `sign` `rint` (to the nearest integer) |
| several values | `min` `max` `sum` `avg`, with any number of arguments |
| random | `rnd()` is a number in `[0, 1)`; `randInt(n)` is a whole number from `0` to `n-1` |

Two constants: `_pi` and `_e`.

**These do not exist**, though most expression languages have them. Each is refused with
`Unexpected token "…"`:

| not available | use instead |
|---|---|
| `floor` `ceil` `round` | `rint` rounds to the nearest; there is no floor or ceiling |
| `pow(a, b)` | `a^b` |
| `clamp(v, lo, hi)` | `min(max(v, lo), hi)` |
| `if(c, a, b)` | `c ? a : b` |
| `!c` | `c == 0` |
| `a % b`, `fmod` | none |
| `pi`, `e` | `_pi`, `_e` |

`rnd()` and `randInt()` draw from a generator seeded by the filter's **Random seed**
parameter. With the default `0` every run gives different values; any other number makes
the run exactly reproducible.

## Variables

Each filter sees the variables of the element it runs over. All of them see the bounding
box and the functions above.

### Bounding box

The bounding box of the layer, taken **before** the filter runs and in the layer's own
coordinates (its transform is not applied):

`xmin` `ymin` `zmin` · `xmax` `ymax` `zmax` · `xmid` `ymid` `zmid` (the center) ·
`xdim` `ydim` `zdim` (the extents) · `bbdiag` (the diagonal length)

### Vertex filters

| variable | meaning |
|---|---|
| `x` `y` `z` | position, in the layer's own coordinates |
| `nx` `ny` `nz` | normal |
| `r` `g` `b` `a` | color, each from 0 to 255 |
| `q` | scalar |
| `vi` | index of the vertex |
| `vtu` `vtv` | texture coordinates; 0 if the layer has none |
| `ti` | texture index of those coordinates; 0 if none |
| `vsel` | 1 if the vertex is selected, else 0 |

### Face filters

A face has its own properties and the properties of its three corners, which are numbered
`0`, `1` and `2`. The corner variables read the corner **vertices**.

| variable | meaning |
|---|---|
| `x0` `y0` `z0`, `x1` `y1` `z1`, `x2` `y2` `z2` | corner positions |
| `nx0` `ny0` `nz0` … `nx2` `ny2` `nz2` | corner normals |
| `r0` `g0` `b0` `a0` … `r2` `g2` `b2` `a2` | corner colors |
| `q0` `q1` `q2` | corner scalars |
| `vi0` `vi1` `vi2` | indices of the corner vertices |
| `vsel0` `vsel1` `vsel2` | 1 if that corner is selected, else 0 |
| `fr` `fg` `fb` `fa` | the face's own color; 255 if the layer has no face color |
| `fnx` `fny` `fnz` | the face's own normal |
| `fq` | the face's own scalar; 0 if the layer has none |
| `fi` | index of the face |
| `fsel` | 1 if the face is selected, else 0 |
| `wtu0` `wtv0` `wtu1` `wtv1` `wtu2` `wtv2` | texture coordinates of the three wedges; 0 if none |
| `ti` | texture index of the first wedge; 0 if none |

### Edge filters

These run on the real edges of a polyline layer, not on the sides of triangles. An edge
has two endpoints, numbered `0` and `1`, which are named as the first two corners of a
face. What it adds is its own properties and a few quantities derived from the endpoints.

| variable | meaning |
|---|---|
| `x0` `y0` `z0`, `x1` `y1` `z1` | endpoint positions |
| `nx0` `ny0` `nz0`, `nx1` `ny1` `nz1` | endpoint normals |
| `r0` `g0` `b0` `a0`, `r1` `g1` `b1` `a1` | endpoint colors |
| `q0` `q1` | endpoint scalars |
| `vi0` `vi1` | indices of the endpoint vertices |
| `vsel0` `vsel1` | 1 if that endpoint is selected, else 0 |
| `er` `eg` `eb` `ea` | the edge's own color |
| `eq` | the edge's own scalar |
| `ei` | index of the edge |
| `esel` | 1 if the edge is selected, else 0 |
| `elen` | length |
| `emx` `emy` `emz` | midpoint |
| `edx` `edy` `edz` | unit direction, from endpoint 0 to endpoint 1; zero for an empty edge |

### The two filters that are different

- ***Create Isosurface from Expression*** evaluates the formula at points of a grid. It sees
  only `x`, `y`, `z`, and the surface is where the result is **zero**. It has no bounding
  box and no mesh to read.
- ***Refine by User Expression*** has two formulas that see different things. The **edge
  predicate** sees the endpoints' `x0 … z1`, `nx0 … nz1`, `r0 g0 b0` and `r1 g1 b1`
  (no alpha), and `q0 q1`. The **split-point formulas** see only `x0 y0 z0 x1 y1 z1`.
  Neither sees the bounding box or custom attributes. The predicate is tried with the
  endpoints in **both** orders and the edge is refined if either is true, so a predicate
  like `q0 > q1` matches every edge whose endpoints differ.

## Custom attributes

*Define Custom … Attribute* stores the result of a formula on every element under a name,
and later expression filters can then read it **by that name**.

- A **scalar** attribute `height` is read as `height`.
- A **point** attribute `dir` is read as three variables, `dir_x`, `dir_y`, `dir_z`.
- An **integer** attribute, such as an identifier or a label from a file, is also read by
  its name. No filter writes one.
- A vertex attribute is seen by vertex filters and a face attribute by face filters, **not
  across**: a face filter that mentions a vertex attribute is refused with
  `Unexpected token`. Edge filters see no custom attributes.

A name may use letters, digits and underscores and must not start with a digit. A name that
is already a variable (`q`, `fi`, `zmid`, …) is refused when you define the attribute.
An attribute that arrives some other way, from a file, with an unusable name is skipped
without a message.

## Working on the selection

Filters marked *yes* under *Only selected* have a checkbox, **Only selected vertices**
(or faces, or edges), with a live count. With it on, **nothing outside the selection
changes**. It is on by default whenever something is selected, and a run with it on and
nothing to work on is refused rather than silently doing nothing.

If you selected the other kind of element (vertices for a face filter, say), the filter
works on the elements lying wholly inside that selection: the faces whose three corners are
all selected. Your own selection is left exactly as it was.

For the scalar filters, *Normalize* and *Map into Color* then use the range of the
selected elements alone.

*Select … by Expression* is the other direction: it **sets** the selection. It replaces
the old one unless **Preserve Existing Selection** is on, which adds to it instead.

Independently of all that, the formulas can look at the selection themselves with `vsel`,
`fsel`, `esel` and the corner versions. With the checkbox off, `fsel*255` as the green
channel marks the selected faces green and leaves the rest black.

## Examples

In each, the filter is listed first and the formulas under its field labels.

**Color the vertices by height**, blue at the bottom to red at the top.
*Compute Vertex Color by Expression*: X `255*(z-zmin)/zdim`, Y `0`,
Z `255-255*(z-zmin)/zdim`, Alpha `255`.

**Select the vertices above the middle.** *Select Vertices by Expression*: `z > zmid`.

**Select the faces that look up.** *Select Faces by Expression*: `fnz > 0.7`.

**Color the faces by their normal**, the usual normal-map look. *Compute Face Color by
Expression*: Red `127.5*(fnx+1)`, Green `127.5*(fny+1)`, Blue `127.5*(fnz+1)`, Alpha `255`.

**Mark the selected faces green, with the checkbox off.** *Compute Face Color by
Expression*: Red `0`, Green `fsel*255`, Blue `0`, Alpha `255`.

**Carry a measurement into a later step.** *Define Custom Vertex Scalar Attribute*: name
`height`, formula `z-zmin`. Then *Compute Vertex Color by Expression*: X `255*height/zdim`,
Y `0`, Z `0`, Alpha `255`.

**Ripple a surface.** *Compute Vertex Coordinates by Expression*: X `x`, Y `y`,
Z `z+0.1*sin(10*x)`.

**Refine only the upper half.** *Refine by User Expression*: edge predicate
`z0 > 0 && z1 > 0`, split point `(x0+x1)/2`, `(y0+y1)/2`, `(z0+z1)/2`.

**Color a polyline by direction.** *Compute Edge Color by Expression*: Red `255*abs(edx)`,
Green `255*abs(edy)`, Blue `255*abs(edz)`, Alpha `255`.

## What to expect

- **Colors** are rounded to the nearest whole number and limited to 0–255: `300` gives 255,
  `-5` gives 0, `127.5` gives 128. A result that is not a number, such as `sqrt(-1)`, gives
  0 rather than stopping the filter.
- **Normals** are stored as the formulas give them, not scaled to unit length.
- **Scalars** are stored exactly as computed; a not-a-number result is stored as it is.
- **A bad expression** is refused before anything is changed, with a message like
  `Unexpected token "floor" found at position 0.` The message does not say which field it
  came from, and a name you misspelled is reported the same way as a function that does not
  exist.
