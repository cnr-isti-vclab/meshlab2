// The single translation unit that compiles stb_image. Nothing else in the project may
// define STB_IMAGE_IMPLEMENTATION: tinygltf includes the same header in the glTF plugin
// and relies on these symbols being linked in from here. For the same reason this file
// disables none of stb's optional pieces -- the declarations tinygltf compiles against
// come from the unmodified header, so anything trimmed here would go missing at link.
//
// stb is here for one job: decoding what Qt's own image plugins decline. See
// TextureAssociationUtils::readImageFile.

#if defined(MESHLAB2_HAS_STB_IMAGE)

#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

#endif
