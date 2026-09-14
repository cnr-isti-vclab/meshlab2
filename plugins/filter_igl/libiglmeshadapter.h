#pragma once

#include "vcgmesh.h"

#include <Eigen/Dense>
#include <QMatrix4x4>
#include <QString>
#include <vector>

namespace meshlab::libigl {

using VertexMatrix = Eigen::Matrix<double, Eigen::Dynamic, 3, Eigen::RowMajor>;
using FaceMatrix = Eigen::Matrix<int, Eigen::Dynamic, 3, Eigen::RowMajor>;

struct EigenMesh
{
    VertexMatrix vertices;
    FaceMatrix faces;
    std::vector<int> vertexToSourceIndex;
    std::vector<int> faceToSourceIndex;
    int skippedFaces = 0;
};

bool meshToEigen(
    const VCGMesh &mesh,
    EigenMesh &out,
    QString &error,
    const QMatrix4x4 *transform = nullptr);

bool meshVerticesToEigen(
    const VCGMesh &mesh,
    VertexMatrix &vertices,
    std::vector<int> &vertexToSourceIndex,
    QString &error,
    const QMatrix4x4 *transform = nullptr);

bool eigenToMesh(
    const VertexMatrix &vertices,
    const FaceMatrix &faces,
    VCGMesh &out,
    QString &error);

// Attribute transfer uses EigenMesh's compact-row -> source-vector mapping, so
// every libigl filter handles deleted and unreferenced vertices identically.
bool selectedVertexRows(
    const VCGMesh &mesh,
    const EigenMesh &source,
    Eigen::VectorXi &rows,
    QString &error);

bool writeVertexScalars(
    VCGMesh &mesh,
    const EigenMesh &source,
    const Eigen::VectorXd &values,
    QString &error);

bool writeVertexScalars(
    VCGMesh &mesh,
    const std::vector<int> &vertexToSourceIndex,
    const Eigen::VectorXd &values,
    QString &error);

bool writeVertexCurvature(
    VCGMesh &mesh,
    const EigenMesh &source,
    const Eigen::MatrixXd &maximumDirections,
    const Eigen::MatrixXd &minimumDirections,
    const Eigen::VectorXd &maximumValues,
    const Eigen::VectorXd &minimumValues,
    QString &error);

} // namespace meshlab::libigl
