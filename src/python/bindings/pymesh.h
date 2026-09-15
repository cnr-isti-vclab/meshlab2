#pragma once

#include <nanobind/nanobind.h>
#include <nanobind/ndarray.h>
#include <cstdint>
#include <string>

class Document;
class VCGMesh;

namespace nb = nanobind;

class PyMesh
{
public:
    PyMesh(Document *doc, int index);

    std::uint64_t id() const;
    int index() const;
    bool isVisible() const;
    int vertexNumber() const;
    int faceNumber() const;
    int edgeNumber() const;
    std::string label() const;
    bool isCompact() const;
    bool isPointCloud() const;

    nb::ndarray<nb::numpy, double, nb::shape<-1, 3>> vertexMatrix() const;
    nb::ndarray<nb::numpy, int32_t, nb::shape<-1, 3>> faceMatrix() const;
    nb::ndarray<nb::numpy, double, nb::shape<-1, 3>> vertexNormalMatrix() const;
    nb::ndarray<nb::numpy, double, nb::shape<-1, 4>> vertexColorMatrix() const;
    nb::ndarray<nb::numpy, double, nb::shape<-1>> vertexScalarArray() const;
    nb::ndarray<nb::numpy, double, nb::shape<-1, 3>> faceNormalMatrix() const;
    nb::ndarray<nb::numpy, double, nb::shape<-1, 4>> faceColorMatrix() const;

    nb::ndarray<nb::numpy, double, nb::shape<-1, 2>> vertexTexCoordMatrix() const;
    nb::ndarray<nb::numpy, double, nb::shape<-1, 3, 2>> wedgeTexCoordMatrix() const;
    bool hasVertexTexCoord() const;
    bool hasWedgeTexCoord() const;

    nb::ndarray<nb::numpy, double, nb::shape<-1, 3>> vertexCurvaturePrincipalDir1Matrix() const;
    nb::ndarray<nb::numpy, double, nb::shape<-1, 3>> vertexCurvaturePrincipalDir2Matrix() const;
    bool hasVertexCurvature() const;

private:
    const VCGMesh &cm() const;
    Document *m_doc;
    int m_index;
};

void registerPyMesh(nb::module_ &m);
