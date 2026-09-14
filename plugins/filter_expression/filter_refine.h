#pragma once

#include <muParser.h>
#include <vcg/complex/algorithms/refine.h>
#include <vcg/math/base.h>
#include <cmath>
#include <random>
#include <string>

namespace meshlab::filters {

inline std::string parserStringToStd(const mu::string_type &s)
{
#ifdef _UNICODE
    return std::string(s.begin(), s.end());
#else
    return s;
#endif
}

// The generator behind the expression language's rnd()/randInt(). thread_local so
// that concurrent evaluations never share state; seeded per filter run by
// seedParserRandom() from the filter's randomSeed parameter.
inline std::mt19937 &parserRandomGenerator()
{
    thread_local std::mt19937 gen(std::random_device{}());
    return gen;
}

inline void seedParserRandom(unsigned int seed)
{
    parserRandomGenerator().seed(std::mt19937::result_type(seed));
}

inline double parserRnd()
{
    thread_local std::uniform_real_distribution<double> dist(0.0, 1.0);
    return dist(parserRandomGenerator());
}

inline double parserRandInt(double upper)
{
    if (!std::isfinite(upper) || upper <= 0.0)
        return 0.0;
    return std::floor(upper * parserRnd());
}

inline void defineParserCustomFunctions(mu::Parser &parser)
{
    parser.DefineFun("rnd", &parserRnd);
    parser.DefineFun("randInt", &parserRandInt);
}

template<class MeshType>
class MidPointCustom
{
public:
    using FaceType = typename MeshType::FaceType;
    using CoordType = typename MeshType::CoordType;

    MidPointCustom(
        MeshType &mesh,
        const std::string &exprX,
        const std::string &exprY,
        const std::string &exprZ,
        bool &error,
        std::string &message)
        : m_mesh(mesh)
    {
        defineVariables(m_parserX);
        defineVariables(m_parserY);
        defineVariables(m_parserZ);
        defineParserCustomFunctions(m_parserX);
        defineParserCustomFunctions(m_parserY);
        defineParserCustomFunctions(m_parserZ);

        try {
            m_parserX.SetExpr(exprX);
            m_parserY.SetExpr(exprY);
            m_parserZ.SetExpr(exprZ);
            m_parserX.Eval();
            m_parserY.Eval();
            m_parserZ.Eval();
        } catch (mu::Parser::exception_type &e) {
            message = parserStringToStd(e.GetMsg());
            error = true;
        }
    }

    void operator()(typename MeshType::VertexType &newVertex, vcg::face::Pos<FaceType> edgePos)
    {
        m_x0 = edgePos.V()->P()[0];
        m_y0 = edgePos.V()->P()[1];
        m_z0 = edgePos.V()->P()[2];
        m_x1 = edgePos.VFlip()->P()[0];
        m_y1 = edgePos.VFlip()->P()[1];
        m_z1 = edgePos.VFlip()->P()[2];

        newVertex.P() = CoordType(m_parserX.Eval(), m_parserY.Eval(), m_parserZ.Eval());

        if (vcg::tri::HasPerVertexColor(m_mesh)) {
            const float distR = vcg::math::Abs(float(edgePos.V()->C()[0]) - float(edgePos.VFlip()->C()[0]));
            const float distG = vcg::math::Abs(float(edgePos.V()->C()[1]) - float(edgePos.VFlip()->C()[1]));
            const float distB = vcg::math::Abs(float(edgePos.V()->C()[2]) - float(edgePos.VFlip()->C()[2]));
            const double edgeLen = vcg::Distance(edgePos.V()->P(), edgePos.VFlip()->P());
            if (edgeLen > 0.0) {
                const double localLen = vcg::Distance(edgePos.V()->P(), newVertex.P());
                const double r = localLen * (distR / edgeLen);
                const double g = localLen * (distG / edgeLen);
                const double b = localLen * (distB / edgeLen);

                newVertex.C()[0] =
                    edgePos.V()->C()[0] < edgePos.VFlip()->C()[0]
                    ? uint8_t(edgePos.V()->C()[0] + r)
                    : uint8_t(edgePos.V()->C()[0] - r);
                newVertex.C()[1] =
                    edgePos.V()->C()[1] < edgePos.VFlip()->C()[1]
                    ? uint8_t(edgePos.V()->C()[1] + g)
                    : uint8_t(edgePos.V()->C()[1] - g);
                newVertex.C()[2] =
                    edgePos.V()->C()[2] < edgePos.VFlip()->C()[2]
                    ? uint8_t(edgePos.V()->C()[2] + b)
                    : uint8_t(edgePos.V()->C()[2] - b);
            }
        }

        if (vcg::tri::HasPerVertexQuality(m_mesh)) {
            const double edgeLen = vcg::Distance(edgePos.V()->P(), edgePos.VFlip()->P());
            if (edgeLen > 0.0) {
                const double localLen = vcg::Distance(edgePos.V()->P(), newVertex.P());
                const double deltaQ = localLen
                    * (vcg::math::Abs(edgePos.V()->Q() - edgePos.VFlip()->Q()) / edgeLen);
                newVertex.Q() =
                    edgePos.V()->Q() < edgePos.VFlip()->Q()
                    ? edgePos.V()->Q() + deltaQ
                    : edgePos.V()->Q() - deltaQ;
            }
        }
    }

    template<class ScalarType>
    vcg::Color4<ScalarType> WedgeInterp(vcg::Color4<ScalarType> &c0, vcg::Color4<ScalarType> &c1)
    {
        vcg::Color4<ScalarType> out;
        return out.lerp(c0, c1, 0.5f);
    }

    template<class FloatType>
    vcg::TexCoord2<FloatType, 1> WedgeInterp(vcg::TexCoord2<FloatType, 1> &t0, vcg::TexCoord2<FloatType, 1> &t1)
    {
        vcg::TexCoord2<FloatType, 1> out;
        out.n() = t0.n();
        out.t() = (t0.t() + t1.t()) / 2.0;
        return out;
    }

private:
    void defineVariables(mu::Parser &parser)
    {
        parser.DefineVar("x0", &m_x0);
        parser.DefineVar("y0", &m_y0);
        parser.DefineVar("z0", &m_z0);
        parser.DefineVar("x1", &m_x1);
        parser.DefineVar("y1", &m_y1);
        parser.DefineVar("z1", &m_z1);
    }

    MeshType &m_mesh;
    mu::Parser m_parserX;
    mu::Parser m_parserY;
    mu::Parser m_parserZ;
    double m_x0 = 0.0;
    double m_y0 = 0.0;
    double m_z0 = 0.0;
    double m_x1 = 0.0;
    double m_y1 = 0.0;
    double m_z1 = 0.0;
};

template<class MeshType>
class CustomEdge
{
public:
    using FaceType = typename MeshType::FaceType;

    CustomEdge(const std::string &expr, bool &error, std::string &message)
    {
        defineVariables(m_parser);
        defineParserCustomFunctions(m_parser);
        try {
            m_parser.SetExpr(expr);
            m_parser.Eval();
        } catch (mu::Parser::exception_type &e) {
            message = parserStringToStd(e.GetMsg());
            error = true;
        }
    }

    bool operator()(vcg::face::Pos<FaceType> edgePos)
    {
        setVariableValues(edgePos);
        const bool firstOrientation = bool(m_parser.Eval());
        edgePos.FlipV();
        setVariableValues(edgePos);
        const bool secondOrientation = bool(m_parser.Eval());
        return firstOrientation || secondOrientation;
    }

private:
    void defineVariables(mu::Parser &parser)
    {
        parser.DefineVar("x0", &m_x0);
        parser.DefineVar("y0", &m_y0);
        parser.DefineVar("z0", &m_z0);
        parser.DefineVar("x1", &m_x1);
        parser.DefineVar("y1", &m_y1);
        parser.DefineVar("z1", &m_z1);

        parser.DefineVar("nx0", &m_nx0);
        parser.DefineVar("ny0", &m_ny0);
        parser.DefineVar("nz0", &m_nz0);
        parser.DefineVar("nx1", &m_nx1);
        parser.DefineVar("ny1", &m_ny1);
        parser.DefineVar("nz1", &m_nz1);

        parser.DefineVar("r0", &m_r0);
        parser.DefineVar("g0", &m_g0);
        parser.DefineVar("b0", &m_b0);
        parser.DefineVar("r1", &m_r1);
        parser.DefineVar("g1", &m_g1);
        parser.DefineVar("b1", &m_b1);

        parser.DefineVar("q0", &m_q0);
        parser.DefineVar("q1", &m_q1);
    }

    void setVariableValues(vcg::face::Pos<FaceType> &edgePos)
    {
        m_x0 = edgePos.V()->P()[0];
        m_y0 = edgePos.V()->P()[1];
        m_z0 = edgePos.V()->P()[2];
        m_x1 = edgePos.VFlip()->P()[0];
        m_y1 = edgePos.VFlip()->P()[1];
        m_z1 = edgePos.VFlip()->P()[2];

        m_nx0 = edgePos.V()->N()[0];
        m_ny0 = edgePos.V()->N()[1];
        m_nz0 = edgePos.V()->N()[2];
        m_nx1 = edgePos.VFlip()->N()[0];
        m_ny1 = edgePos.VFlip()->N()[1];
        m_nz1 = edgePos.VFlip()->N()[2];

        m_r0 = edgePos.V()->C()[0];
        m_g0 = edgePos.V()->C()[1];
        m_b0 = edgePos.V()->C()[2];
        m_r1 = edgePos.VFlip()->C()[0];
        m_g1 = edgePos.VFlip()->C()[1];
        m_b1 = edgePos.VFlip()->C()[2];

        m_q0 = edgePos.V()->Q();
        m_q1 = edgePos.VFlip()->Q();
    }

    mu::Parser m_parser;
    double m_x0 = 0.0;
    double m_y0 = 0.0;
    double m_z0 = 0.0;
    double m_x1 = 0.0;
    double m_y1 = 0.0;
    double m_z1 = 0.0;
    double m_nx0 = 0.0;
    double m_ny0 = 0.0;
    double m_nz0 = 0.0;
    double m_nx1 = 0.0;
    double m_ny1 = 0.0;
    double m_nz1 = 0.0;
    double m_r0 = 0.0;
    double m_g0 = 0.0;
    double m_b0 = 0.0;
    double m_r1 = 0.0;
    double m_g1 = 0.0;
    double m_b1 = 0.0;
    double m_q0 = 0.0;
    double m_q1 = 0.0;
};

} // namespace meshlab::filters
